/**
 * @file    bme680_driver.h
 * @brief   Thin STM32 I2C port layer around Bosch's official BME68x_SensorAPI.
 *
 * The Bosch driver is bus-agnostic: it calls back into read/write/delay
 * functions supplied by the integrator. Those callbacks live in
 * bme680_driver.c and are the only place this project touches the I2C
 * peripheral, which is what makes the I2C mutex a single, auditable choke point.
 */
#ifndef BME680_DRIVER_H
#define BME680_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "telemetry.h"

typedef enum
{
    BME680_OK = 0,
    BME680_ERR_NOT_FOUND,      /**< no ACK at either I2C address           */
    BME680_ERR_INIT,           /**< chip answered but init/self-test failed */
    BME680_ERR_CONFIG,
    BME680_ERR_MEASURE,        /**< forced-mode conversion failed           */
    BME680_ERR_NO_DATA         /**< conversion produced no valid frame      */
} bme680_status_t;

/**
 * Probe the sensor (primary address first, then secondary), initialise it and
 * apply the oversampling / filter / gas-heater configuration.
 */
bme680_status_t bme680_driver_init(void);

/** The 7-bit address the sensor actually answered on, or 0 before init. */
uint8_t bme680_driver_address(void);

/**
 * Run one forced-mode conversion and fill @p out.
 *
 * Blocks for the conversion duration (typically ~150-200 ms with the gas heater
 * enabled). Under FreeRTOS that wait yields the CPU rather than spinning.
 */
bme680_status_t bme680_driver_read(sensor_data_t *out);

/**
 * Read the chip ID register through the shared I2C bus.
 *
 * Exists so a second task (the health monitor) genuinely contends for the bus,
 * which is what the I2C mutex is protecting against.
 */
bool bme680_driver_read_chip_id(uint8_t *chip_id);

/** Human-readable status, for logging. */
const char *bme680_status_name(bme680_status_t status);

/* --------------------------------------------------------------------------
 *  Port hooks.
 *
 *  Declared weak with no-op defaults so the driver works before the RTOS
 *  exists (phase 0). app_tasks.c provides the strong definitions once the
 *  scheduler owns the bus, turning every sensor access into a mutex-protected
 *  critical section without the driver knowing FreeRTOS exists.
 * ----------------------------------------------------------------------- */
bool bme680_port_lock(uint32_t timeout_ms);
void bme680_port_unlock(void);
void bme680_port_delay_ms(uint32_t ms);

#endif /* BME680_DRIVER_H */
