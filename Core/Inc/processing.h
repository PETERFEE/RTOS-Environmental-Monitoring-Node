/**
 * @file    processing.h
 * @brief   Pure signal-processing logic: moving average, thresholds, IAQ.
 *
 * No HAL, no FreeRTOS -- so tests/native builds this file for the host and
 * tests/test_processing.py drives it through ctypes with known vectors.
 */
#ifndef PROCESSING_H
#define PROCESSING_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "telemetry.h"

/** Fixed-length ring buffer implementing a moving average without division
 *  drift: the running sum is recomputed from the window on every update. */
typedef struct
{
    float    window[PROCESSING_WINDOW_LEN];
    uint8_t  count;            /**< samples held, saturates at window length */
    uint8_t  head;             /**< next write index                         */
} moving_avg_t;

typedef struct
{
    moving_avg_t temperature;
    moving_avg_t humidity;
    moving_avg_t pressure;
    moving_avg_t gas;

    uint32_t samples_processed;
    uint32_t anomalies_detected;
} processing_ctx_t;

void  moving_avg_reset(moving_avg_t *avg);
void  moving_avg_push(moving_avg_t *avg, float sample);
float moving_avg_value(const moving_avg_t *avg);
bool  moving_avg_ready(const moving_avg_t *avg);

void processing_init(processing_ctx_t *ctx);

/**
 * Fold one raw sample into the running statistics and classify it.
 *
 * @param ctx     running state, must have been passed to processing_init()
 * @param sample  raw reading from the sensor task
 * @param out     result, always fully populated
 * @return true if the sample was usable, false if it was rejected as invalid
 */
bool processing_update(processing_ctx_t *ctx,
                       const sensor_data_t *sample,
                       processed_data_t *out);

/**
 * Approximate an indoor-air-quality score from gas resistance and humidity.
 * 100 = clean and comfortable, 0 = poor. This is a weighted heuristic, not
 * Bosch's BSEC IAQ index -- see docs/measurements.md for the reasoning.
 */
uint8_t processing_air_quality(float gas_resistance_ohm, float humidity_pct);

/** Threshold classification for one averaged reading. */
env_status_t processing_classify(float temperature_c, float humidity_pct, float gas_ohm);

/** Reject physically impossible readings (sensor wiring faults, NaN, etc.). */
bool processing_sample_is_plausible(const sensor_data_t *sample);

#endif /* PROCESSING_H */
