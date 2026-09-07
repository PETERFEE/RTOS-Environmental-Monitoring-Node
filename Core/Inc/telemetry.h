/**
 * @file    telemetry.h
 * @brief   Shared data contract between tasks + the wire format sent to the ESP32.
 *
 * This module is deliberately free of HAL and FreeRTOS dependencies so that
 * tests/native can compile it for the host and exercise the formatter against
 * known inputs (see tests/test_uart_protocol.py).
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/*  Raw sample produced by the sensor task                                    */
/* ------------------------------------------------------------------------ */
typedef struct
{
    float    temperature;      /**< degC                                     */
    float    humidity;         /**< %RH                                      */
    float    pressure;         /**< hPa                                      */
    float    gas_resistance;   /**< Ohm (higher = cleaner air)               */

    uint32_t timestamp;        /**< ms since boot (RTOS tick)                */
    uint32_t sequence;         /**< monotonic sample counter                 */
    bool     valid;            /**< false if the BME680 read failed          */
    bool     forced;           /**< true if triggered by the button, not the period */
} sensor_data_t;

/* ------------------------------------------------------------------------ */
/*  Environment classification produced by the processing task                */
/* ------------------------------------------------------------------------ */
typedef enum
{
    ENV_STATUS_NORMAL = 0,
    ENV_STATUS_WARM,
    ENV_STATUS_COLD,
    ENV_STATUS_HUMID,
    ENV_STATUS_DRY,
    ENV_STATUS_POOR_AIR,
    ENV_STATUS_SENSOR_FAULT,
    ENV_STATUS_COUNT
} env_status_t;

typedef struct
{
    sensor_data_t raw;         /**< the sample this result came from         */

    float temperature_avg;     /**< moving average over PROCESSING_WINDOW_LEN */
    float humidity_avg;
    float pressure_avg;
    float gas_avg;

    float temperature_delta;   /**< sample - average, i.e. short-term trend  */

    uint8_t      air_quality;  /**< 0..100 approximation, higher is better   */
    env_status_t status;
    bool         anomaly;      /**< sample deviates sharply from the average */
} processed_data_t;

/* ------------------------------------------------------------------------ */
/*  Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/** Human-readable name for a status code, e.g. "NORMAL". Never NULL. */
const char *telemetry_status_name(env_status_t status);

/**
 * Format a float with a fixed number of decimals, without pulling printf's
 * floating-point support into the image (newlib-nano's printf has no %f).
 *
 * @return number of characters written, excluding the terminating NUL.
 */
size_t telemetry_ftoa(float value, uint8_t decimals, char *out, size_t out_size);

/**
 * Build the compact key=value telemetry line sent to the ESP32, e.g.
 *   "T=25.30,H=45.80,P=1012.40,G=45231,A=72,S=NORMAL,TS=12345,N=42\n"
 *
 * @return number of characters written, excluding the NUL (0 on bad args).
 */
size_t telemetry_format_line(const processed_data_t *data, char *out, size_t out_size);

/**
 * Build the response to a STATUS command, e.g.
 *   "TEMP=25.30,HUM=48.20,PERIOD=1000,HEAP=8192,UPTIME=12345,SAMPLES=42,ERRORS=0\n"
 */
size_t telemetry_format_status(const processed_data_t *data,
                               uint32_t period_ms,
                               uint32_t free_heap,
                               uint32_t uptime_ms,
                               uint32_t samples,
                               uint32_t errors,
                               char *out, size_t out_size);

#endif /* TELEMETRY_H */
