/**
 * @file    processing.c
 * @brief   Moving average, plausibility checks, threshold classification and a
 *          lightweight indoor-air-quality approximation.
 *
 * Pure C with no platform dependencies: tests/native compiles this file for the
 * host so tests/test_processing.py can drive it with reference vectors.
 */
#include "processing.h"

#include <string.h>

/* -------------------------------------------------------------------------
 *  Small float helpers (avoids pulling in libm on the target)
 * ---------------------------------------------------------------------- */
static float f_abs(float v)
{
    return (v < 0.0f) ? -v : v;
}

static float f_clamp(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

static bool f_is_nan(float v)
{
    return v != v;   /* only NaN compares unequal to itself */
}

/* -------------------------------------------------------------------------
 *  Moving average
 * ---------------------------------------------------------------------- */
void moving_avg_reset(moving_avg_t *avg)
{
    if (avg == NULL) { return; }
    memset(avg, 0, sizeof(*avg));
}

void moving_avg_push(moving_avg_t *avg, float sample)
{
    if (avg == NULL || f_is_nan(sample)) { return; }

    avg->window[avg->head] = sample;
    avg->head = (uint8_t)((avg->head + 1U) % PROCESSING_WINDOW_LEN);

    if (avg->count < PROCESSING_WINDOW_LEN)
    {
        avg->count++;
    }
}

float moving_avg_value(const moving_avg_t *avg)
{
    if (avg == NULL || avg->count == 0U) { return 0.0f; }

    /* Summing the window every time (rather than keeping a running total that
     * gains and loses terms) keeps rounding error bounded by the window size
     * instead of growing without limit over days of uptime. */
    float sum = 0.0f;
    for (uint8_t i = 0U; i < avg->count; i++)
    {
        sum += avg->window[i];
    }
    return sum / (float)avg->count;
}

bool moving_avg_ready(const moving_avg_t *avg)
{
    return (avg != NULL) && (avg->count >= PROCESSING_WINDOW_LEN);
}

/* -------------------------------------------------------------------------
 *  Validation
 * ---------------------------------------------------------------------- */
bool processing_sample_is_plausible(const sensor_data_t *sample)
{
    if (sample == NULL || !sample->valid)
    {
        return false;
    }
    if (f_is_nan(sample->temperature) || f_is_nan(sample->humidity) ||
        f_is_nan(sample->pressure)    || f_is_nan(sample->gas_resistance))
    {
        return false;
    }

    /* BME680 operating envelope, per the Bosch datasheet, widened slightly. */
    if (sample->temperature < -45.0f || sample->temperature > 90.0f)  { return false; }
    if (sample->humidity    <   0.0f || sample->humidity    > 100.0f) { return false; }
    if (sample->pressure    < 250.0f || sample->pressure    > 1200.0f){ return false; }
    if (sample->gas_resistance < 0.0f)                                { return false; }

    return true;
}

/* -------------------------------------------------------------------------
 *  Air quality approximation
 *
 *  Two contributions, weighted:
 *    - gas resistance relative to a clean-air baseline (VOC proxy)
 *    - distance of relative humidity from a comfortable 40 %RH
 *
 *  Higher gas resistance means fewer reducing gases, so the score rises with
 *  resistance. This is a heuristic in the spirit of Bosch's published examples,
 *  NOT the calibrated BSEC IAQ index -- see docs/measurements.md.
 * ---------------------------------------------------------------------- */
uint8_t processing_air_quality(float gas_resistance_ohm, float humidity_pct)
{
    if (f_is_nan(gas_resistance_ohm) || f_is_nan(humidity_pct))
    {
        return 0U;
    }

    const float gas_ratio = f_clamp(gas_resistance_ohm / IAQ_GAS_BASELINE_OHM, 0.0f, 1.0f);
    const float gas_score = gas_ratio * IAQ_GAS_WEIGHT * 100.0f;

    humidity_pct = f_clamp(humidity_pct, 0.0f, 100.0f);

    float hum_ratio;
    if (humidity_pct <= IAQ_HUMIDITY_OPTIMAL_PCT)
    {
        hum_ratio = humidity_pct / IAQ_HUMIDITY_OPTIMAL_PCT;
    }
    else
    {
        hum_ratio = (100.0f - humidity_pct) / (100.0f - IAQ_HUMIDITY_OPTIMAL_PCT);
    }
    const float hum_score = f_clamp(hum_ratio, 0.0f, 1.0f) * IAQ_HUMIDITY_WEIGHT * 100.0f;

    const float total = f_clamp(gas_score + hum_score, 0.0f, 100.0f);
    return (uint8_t)(total + 0.5f);
}

/* -------------------------------------------------------------------------
 *  Threshold classification
 *
 *  Ordered by severity: air quality first, then temperature, then humidity,
 *  so the reported status names the most actionable condition.
 * ---------------------------------------------------------------------- */
env_status_t processing_classify(float temperature_c, float humidity_pct, float gas_ohm)
{
    if (f_is_nan(temperature_c) || f_is_nan(humidity_pct) || f_is_nan(gas_ohm))
    {
        return ENV_STATUS_SENSOR_FAULT;
    }

    if (gas_ohm > 0.0f && gas_ohm < THRESH_GAS_POOR_OHM)  { return ENV_STATUS_POOR_AIR; }
    if (temperature_c > THRESH_TEMP_HIGH_C)               { return ENV_STATUS_WARM; }
    if (temperature_c < THRESH_TEMP_LOW_C)                { return ENV_STATUS_COLD; }
    if (humidity_pct  > THRESH_HUMIDITY_HIGH_PCT)         { return ENV_STATUS_HUMID; }
    if (humidity_pct  < THRESH_HUMIDITY_LOW_PCT)          { return ENV_STATUS_DRY; }

    return ENV_STATUS_NORMAL;
}

/* -------------------------------------------------------------------------
 *  Pipeline entry point
 * ---------------------------------------------------------------------- */
void processing_init(processing_ctx_t *ctx)
{
    if (ctx == NULL) { return; }

    memset(ctx, 0, sizeof(*ctx));
    moving_avg_reset(&ctx->temperature);
    moving_avg_reset(&ctx->humidity);
    moving_avg_reset(&ctx->pressure);
    moving_avg_reset(&ctx->gas);
}

bool processing_update(processing_ctx_t *ctx,
                       const sensor_data_t *sample,
                       processed_data_t *out)
{
    if (ctx == NULL || sample == NULL || out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->raw = *sample;

    if (!processing_sample_is_plausible(sample))
    {
        out->status      = ENV_STATUS_SENSOR_FAULT;
        out->air_quality = 0U;
        /* Report the last known averages so a single bad read does not blank
         * the dashboard. */
        out->temperature_avg = moving_avg_value(&ctx->temperature);
        out->humidity_avg    = moving_avg_value(&ctx->humidity);
        out->pressure_avg    = moving_avg_value(&ctx->pressure);
        out->gas_avg         = moving_avg_value(&ctx->gas);
        return false;
    }

    /* Anomaly detection uses the average from BEFORE this sample is folded in,
     * otherwise a large excursion partly hides itself in its own reference. */
    const float prev_temp_avg = moving_avg_value(&ctx->temperature);
    const bool  have_history  = moving_avg_ready(&ctx->temperature);

    moving_avg_push(&ctx->temperature, sample->temperature);
    moving_avg_push(&ctx->humidity,    sample->humidity);
    moving_avg_push(&ctx->pressure,    sample->pressure);
    moving_avg_push(&ctx->gas,         sample->gas_resistance);

    out->temperature_avg   = moving_avg_value(&ctx->temperature);
    out->humidity_avg      = moving_avg_value(&ctx->humidity);
    out->pressure_avg      = moving_avg_value(&ctx->pressure);
    out->gas_avg           = moving_avg_value(&ctx->gas);
    out->temperature_delta = sample->temperature - out->temperature_avg;

    out->air_quality = processing_air_quality(sample->gas_resistance, sample->humidity);

    /* Classify on the averaged values: a single noisy sample should not flap
     * the reported status. */
    out->status = processing_classify(out->temperature_avg,
                                      out->humidity_avg,
                                      out->gas_avg);

    /* > 5 degC away from the established average is treated as an anomaly. */
    out->anomaly = have_history && (f_abs(sample->temperature - prev_temp_avg) > 5.0f);

    ctx->samples_processed++;
    if (out->anomaly)
    {
        ctx->anomalies_detected++;
    }

    return true;
}
