/**
 * @file    bme680_driver.c
 * @brief   STM32 I2C port layer for Bosch's BME68x_SensorAPI.
 *
 * Responsibilities
 *   - supply the read / write / delay callbacks the Bosch driver needs
 *   - probe both possible I2C addresses so either breakout wiring works
 *   - drive one forced-mode conversion per call and convert the results into
 *     the project's sensor_data_t (pressure in hPa rather than Pa)
 *
 * Bus ownership
 *   The I2C bus is claimed around the two short register bursts (trigger and
 *   fetch) and deliberately RELEASED during the ~180 ms gas-heater conversion.
 *   Holding a mutex across a long blocking wait would serialise every other
 *   would-be bus user behind the sensor for no benefit -- see docs/architecture.md.
 */
#include "bme680_driver.h"

#include <string.h>

#include "bme68x.h"
#include "main.h"
#include "perf.h"

#define BME680_I2C_TIMEOUT_MS   100U
#define BME680_LOCK_TIMEOUT_MS  200U
#define BME680_AMBIENT_TEMP_C   25

static struct bme68x_dev        s_dev;
static struct bme68x_conf       s_conf;
static struct bme68x_heatr_conf s_heatr_conf;

static uint8_t  s_i2c_addr    = 0U;      /* 7-bit address that ACKed          */
static uint32_t s_meas_dur_ms = 0U;      /* cached conversion time            */
static uint32_t s_sequence    = 0U;

/* --------------------------------------------------------------------------
 *  Weak port hooks -- overridden by app_tasks.c once FreeRTOS is running.
 * ----------------------------------------------------------------------- */
__attribute__((weak)) bool bme680_port_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;                  /* no scheduler yet: nothing to contend with */
}

__attribute__((weak)) void bme680_port_unlock(void)
{
}

__attribute__((weak)) void bme680_port_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);                /* busy-wait before the scheduler starts     */
}

/* --------------------------------------------------------------------------
 *  Bosch driver callbacks
 * ----------------------------------------------------------------------- */
static BME68X_INTF_RET_TYPE bme680_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                            uint32_t length, void *intf_ptr)
{
    const uint8_t addr = *(uint8_t *)intf_ptr;

    const HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1,
                                                  (uint16_t)(addr << 1),
                                                  reg_addr,
                                                  I2C_MEMADD_SIZE_8BIT,
                                                  reg_data,
                                                  (uint16_t)length,
                                                  BME680_I2C_TIMEOUT_MS);
    return (st == HAL_OK) ? BME68X_INTF_RET_SUCCESS : (BME68X_INTF_RET_TYPE)-1;
}

static BME68X_INTF_RET_TYPE bme680_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                             uint32_t length, void *intf_ptr)
{
    const uint8_t addr = *(uint8_t *)intf_ptr;

    const HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1,
                                                   (uint16_t)(addr << 1),
                                                   reg_addr,
                                                   I2C_MEMADD_SIZE_8BIT,
                                                   (uint8_t *)reg_data,
                                                   (uint16_t)length,
                                                   BME680_I2C_TIMEOUT_MS);
    return (st == HAL_OK) ? BME68X_INTF_RET_SUCCESS : (BME68X_INTF_RET_TYPE)-1;
}

static void bme680_delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;

    /* Short waits spin on the cycle counter; anything long enough to matter
     * goes through the port hook so the RTOS build yields instead. */
    if (period >= 1000U)
    {
        bme680_port_delay_ms((period + 999U) / 1000U);
    }
    else
    {
        perf_delay_us(period);
    }
}

/* --------------------------------------------------------------------------
 *  Public API
 * ----------------------------------------------------------------------- */
const char *bme680_status_name(bme680_status_t status)
{
    switch (status)
    {
        case BME680_OK:            return "OK";
        case BME680_ERR_NOT_FOUND: return "NOT_FOUND";
        case BME680_ERR_INIT:      return "INIT_FAILED";
        case BME680_ERR_CONFIG:    return "CONFIG_FAILED";
        case BME680_ERR_MEASURE:   return "MEASURE_FAILED";
        case BME680_ERR_NO_DATA:   return "NO_DATA";
        default:                   return "UNKNOWN";
    }
}

uint8_t bme680_driver_address(void)
{
    return s_i2c_addr;
}

static bool probe_address(uint8_t addr)
{
    /* One ACK poll is enough to tell whether anything lives at this address. */
    return HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 3, 50U) == HAL_OK;
}

bme680_status_t bme680_driver_init(void)
{
    memset(&s_dev, 0, sizeof(s_dev));
    s_sequence = 0U;

    if (!bme680_port_lock(BME680_LOCK_TIMEOUT_MS))
    {
        return BME680_ERR_NOT_FOUND;
    }

    s_i2c_addr = 0U;
    if (probe_address(BME680_I2C_ADDR_PRIMARY))
    {
        s_i2c_addr = BME680_I2C_ADDR_PRIMARY;
    }
    else if (probe_address(BME680_I2C_ADDR_SECONDARY))
    {
        s_i2c_addr = BME680_I2C_ADDR_SECONDARY;
    }
    else
    {
        bme680_port_unlock();
        return BME680_ERR_NOT_FOUND;
    }

    s_dev.intf     = BME68X_I2C_INTF;
    s_dev.intf_ptr = &s_i2c_addr;
    s_dev.read     = bme680_i2c_read;
    s_dev.write    = bme680_i2c_write;
    s_dev.delay_us = bme680_delay_us;
    s_dev.amb_temp = BME680_AMBIENT_TEMP_C;

    int8_t rslt = bme68x_init(&s_dev);
    if (rslt != BME68X_OK)
    {
        bme680_port_unlock();
        return BME680_ERR_INIT;
    }

    /* Oversampling chosen for a 1 Hz indoor node: heaviest on pressure (the
     * noisiest channel), lightest on humidity (already slow-moving).
     * IIR filter 3 smooths the temperature/pressure short-term noise. */
    s_conf.os_hum  = BME68X_OS_2X;
    s_conf.os_temp = BME68X_OS_8X;
    s_conf.os_pres = BME68X_OS_4X;
    s_conf.filter  = BME68X_FILTER_SIZE_3;
    s_conf.odr     = BME68X_ODR_NONE;      /* forced mode: we set the cadence */

    rslt = bme68x_set_conf(&s_conf, &s_dev);
    if (rslt != BME68X_OK)
    {
        bme680_port_unlock();
        return BME680_ERR_CONFIG;
    }

    /* 300 degC for 100 ms is Bosch's reference gas-heater profile. */
    s_heatr_conf.enable     = BME68X_ENABLE;
    s_heatr_conf.heatr_temp = 300U;
    s_heatr_conf.heatr_dur  = 100U;

    rslt = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &s_heatr_conf, &s_dev);
    if (rslt != BME68X_OK)
    {
        bme680_port_unlock();
        return BME680_ERR_CONFIG;
    }

    /* TPH conversion time (us) + heater duration (ms), plus a small margin. */
    const uint32_t tph_dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &s_conf, &s_dev);
    s_meas_dur_ms = ((tph_dur_us + 999U) / 1000U) + s_heatr_conf.heatr_dur + 10U;

    bme680_port_unlock();
    return BME680_OK;
}

bool bme680_driver_read_chip_id(uint8_t *chip_id)
{
    if (chip_id == NULL || s_i2c_addr == 0U)
    {
        return false;
    }

    if (!bme680_port_lock(BME680_LOCK_TIMEOUT_MS))
    {
        return false;
    }

    uint8_t value = 0U;
    const int8_t rslt = bme68x_get_regs(BME68X_REG_CHIP_ID, &value, 1U, &s_dev);

    bme680_port_unlock();

    if (rslt != BME68X_OK)
    {
        return false;
    }

    *chip_id = value;
    return (value == BME68X_CHIP_ID);
}

bme680_status_t bme680_driver_read(sensor_data_t *out)
{
    if (out == NULL)
    {
        return BME680_ERR_MEASURE;
    }

    memset(out, 0, sizeof(*out));
    out->timestamp = HAL_GetTick();
    out->sequence  = ++s_sequence;
    out->valid     = false;

    if (s_i2c_addr == 0U)
    {
        return BME680_ERR_NOT_FOUND;
    }

    /* --- 1. Trigger the conversion (bus held briefly) --------------------- */
    if (!bme680_port_lock(BME680_LOCK_TIMEOUT_MS))
    {
        return BME680_ERR_MEASURE;
    }
    int8_t rslt = bme68x_set_op_mode(BME68X_FORCED_MODE, &s_dev);
    bme680_port_unlock();

    if (rslt != BME68X_OK)
    {
        return BME680_ERR_MEASURE;
    }

    /* --- 2. Wait out the heater profile with the bus RELEASED ------------- */
    bme680_port_delay_ms(s_meas_dur_ms);

    /* --- 3. Fetch the result (bus held briefly) --------------------------- */
    if (!bme680_port_lock(BME680_LOCK_TIMEOUT_MS))
    {
        return BME680_ERR_MEASURE;
    }
    struct bme68x_data data;
    uint8_t            n_fields = 0U;

    rslt = bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &s_dev);
    bme680_port_unlock();

    if (rslt != BME68X_OK)
    {
        return BME680_ERR_MEASURE;
    }
    if (n_fields == 0U)
    {
        return BME680_ERR_NO_DATA;
    }

    out->temperature    = data.temperature;
    out->humidity       = data.humidity;
    out->pressure       = data.pressure / 100.0f;   /* Pa -> hPa */
    out->gas_resistance = data.gas_resistance;
    out->timestamp      = HAL_GetTick();

    /* The gas reading is only meaningful once the heater has stabilised; the
     * first few samples after power-up will not have HEAT_STAB_MSK set. */
    const bool gas_valid = ((data.status & BME68X_GASM_VALID_MSK) != 0U) &&
                           ((data.status & BME68X_HEAT_STAB_MSK) != 0U);
    if (!gas_valid)
    {
        out->gas_resistance = 0.0f;
    }

    out->valid = ((data.status & BME68X_NEW_DATA_MSK) != 0U);

    /* Feed the measured temperature back as the ambient reference so the
     * heater resistance calculation stays accurate as the room changes. */
    s_dev.amb_temp = (int8_t)out->temperature;

    return out->valid ? BME680_OK : BME680_ERR_NO_DATA;
}
