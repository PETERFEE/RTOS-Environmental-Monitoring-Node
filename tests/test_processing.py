"""
Unit tests for the firmware's signal-processing logic.

These drive the ACTUAL C code from Core/Src/processing.c, compiled for the host
and loaded through ctypes -- not a Python model of it. A bug in the moving
average or the threshold ladder fails here, on a laptop, in milliseconds,
instead of on a board with a serial cable attached.
"""
from __future__ import annotations

import ctypes
import math

import pytest

from conftest import (
    ENV_COLD,
    ENV_DRY,
    ENV_HUMID,
    ENV_NORMAL,
    ENV_POOR_AIR,
    ENV_SENSOR_FAULT,
    ENV_WARM,
    PROCESSING_WINDOW_LEN,
    MovingAvg,
    ProcessedData,
    make_sample,
)


# ---------------------------------------------------------------------------
#  Moving average
# ---------------------------------------------------------------------------
class TestMovingAverage:
    def test_empty_window_reads_zero(self, lib):
        avg = MovingAvg()
        lib.moving_avg_reset(ctypes.byref(avg))
        assert lib.moving_avg_value(ctypes.byref(avg)) == 0.0
        assert not lib.moving_avg_ready(ctypes.byref(avg))

    def test_partial_window_averages_only_real_samples(self, lib):
        """A half-filled window must not average in the empty slots as zeros."""
        avg = MovingAvg()
        lib.moving_avg_reset(ctypes.byref(avg))
        for value in (10.0, 20.0, 30.0):
            lib.moving_avg_push(ctypes.byref(avg), value)

        assert lib.moving_avg_value(ctypes.byref(avg)) == pytest.approx(20.0)
        assert not lib.moving_avg_ready(ctypes.byref(avg))

    def test_full_window_reports_ready(self, lib):
        avg = MovingAvg()
        lib.moving_avg_reset(ctypes.byref(avg))
        for i in range(PROCESSING_WINDOW_LEN):
            lib.moving_avg_push(ctypes.byref(avg), float(i))

        assert lib.moving_avg_ready(ctypes.byref(avg))
        assert lib.moving_avg_value(ctypes.byref(avg)) == pytest.approx(3.5)

    def test_oldest_sample_is_evicted(self, lib):
        """Push one full window of 0.0, then one of 100.0: only the new one counts."""
        avg = MovingAvg()
        lib.moving_avg_reset(ctypes.byref(avg))
        for _ in range(PROCESSING_WINDOW_LEN):
            lib.moving_avg_push(ctypes.byref(avg), 0.0)
        for _ in range(PROCESSING_WINDOW_LEN):
            lib.moving_avg_push(ctypes.byref(avg), 100.0)

        assert lib.moving_avg_value(ctypes.byref(avg)) == pytest.approx(100.0)

    def test_nan_is_ignored(self, lib):
        avg = MovingAvg()
        lib.moving_avg_reset(ctypes.byref(avg))
        lib.moving_avg_push(ctypes.byref(avg), 20.0)
        lib.moving_avg_push(ctypes.byref(avg), float("nan"))

        value = lib.moving_avg_value(ctypes.byref(avg))
        assert not math.isnan(value)
        assert value == pytest.approx(20.0)


# ---------------------------------------------------------------------------
#  Plausibility
# ---------------------------------------------------------------------------
class TestPlausibility:
    def test_typical_indoor_sample_accepted(self, lib):
        s = make_sample()
        assert lib.processing_sample_is_plausible(ctypes.byref(s))

    def test_invalid_flag_rejected(self, lib):
        s = make_sample(valid=False)
        assert not lib.processing_sample_is_plausible(ctypes.byref(s))

    @pytest.mark.parametrize(
        "field,value",
        [
            ("temperature", -60.0),   # below the BME680's range
            ("temperature", 120.0),
            ("humidity", -1.0),
            ("humidity", 101.0),
            ("pressure", 100.0),      # a disconnected sensor reads nonsense
            ("pressure", 1500.0),
            ("gas_resistance", -1.0),
            ("temperature", float("nan")),
        ],
    )
    def test_out_of_range_rejected(self, lib, field, value):
        s = make_sample()
        setattr(s, field, value)
        assert not lib.processing_sample_is_plausible(ctypes.byref(s))


# ---------------------------------------------------------------------------
#  Air quality approximation
# ---------------------------------------------------------------------------
class TestAirQuality:
    def test_clean_air_at_optimal_humidity_scores_full(self, lib):
        assert lib.processing_air_quality(50_000.0, 40.0) == 100

    def test_score_is_monotonic_in_gas_resistance(self, lib):
        scores = [lib.processing_air_quality(g, 40.0)
                  for g in (5_000.0, 15_000.0, 30_000.0, 50_000.0)]
        assert scores == sorted(scores)
        assert scores[0] < scores[-1]

    def test_humidity_penalty_is_symmetric_about_optimum(self, lib):
        """20 %RH and 70 %RH are both 20 points of 'distance' from 40 %RH."""
        dry = lib.processing_air_quality(50_000.0, 20.0)
        wet = lib.processing_air_quality(50_000.0, 70.0)
        assert dry == wet

    def test_saturates_rather_than_overflowing(self, lib):
        assert lib.processing_air_quality(10_000_000.0, 40.0) == 100

    def test_nan_inputs_score_zero(self, lib):
        assert lib.processing_air_quality(float("nan"), 40.0) == 0
        assert lib.processing_air_quality(50_000.0, float("nan")) == 0

    def test_bounded(self, lib):
        for gas in (0.0, 1.0, 1e3, 1e5, 1e7):
            for hum in (0.0, 25.0, 50.0, 100.0):
                assert 0 <= lib.processing_air_quality(gas, hum) <= 100


# ---------------------------------------------------------------------------
#  Threshold classification
# ---------------------------------------------------------------------------
class TestClassification:
    def test_comfortable_room_is_normal(self, lib):
        assert lib.processing_classify(22.0, 45.0, 50_000.0) == ENV_NORMAL

    @pytest.mark.parametrize(
        "temp,hum,gas,expected",
        [
            (35.0, 45.0, 50_000.0, ENV_WARM),
            (2.0, 45.0, 50_000.0, ENV_COLD),
            (22.0, 80.0, 50_000.0, ENV_HUMID),
            (22.0, 10.0, 50_000.0, ENV_DRY),
            (22.0, 45.0, 5_000.0, ENV_POOR_AIR),
        ],
    )
    def test_each_band(self, lib, temp, hum, gas, expected):
        assert lib.processing_classify(temp, hum, gas) == expected

    def test_air_quality_outranks_temperature(self, lib):
        """Both conditions true: the more actionable one must be reported."""
        assert lib.processing_classify(35.0, 45.0, 5_000.0) == ENV_POOR_AIR

    def test_nan_is_a_sensor_fault(self, lib):
        assert lib.processing_classify(float("nan"), 45.0, 50_000.0) == ENV_SENSOR_FAULT

    def test_gas_of_zero_does_not_trip_poor_air(self, lib):
        """The driver reports 0 when the heater has not stabilised yet. That is
        'no reading', not 'terrible air', and must not be classified as POOR."""
        assert lib.processing_classify(22.0, 45.0, 0.0) == ENV_NORMAL


# ---------------------------------------------------------------------------
#  End-to-end pipeline
# ---------------------------------------------------------------------------
class TestProcessingUpdate:
    def test_first_sample_populates_result(self, lib, ctx):
        s = make_sample(temperature=25.0, humidity=45.0)
        out = ProcessedData()

        assert lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))
        assert out.raw.temperature == pytest.approx(25.0)
        assert out.temperature_avg == pytest.approx(25.0)
        assert ctx.samples_processed == 1

    def test_average_converges_over_the_window(self, lib, ctx):
        out = ProcessedData()
        for _ in range(PROCESSING_WINDOW_LEN):
            s = make_sample(temperature=20.0)
            lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))

        assert out.temperature_avg == pytest.approx(20.0)

        s = make_sample(temperature=28.0)
        lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))

        # One 28 C sample among seven 20 C ones: 21 C.
        assert out.temperature_avg == pytest.approx(21.0)
        assert out.temperature_delta == pytest.approx(7.0)

    def test_invalid_sample_is_rejected_but_keeps_history(self, lib, ctx):
        out = ProcessedData()
        for _ in range(PROCESSING_WINDOW_LEN):
            good = make_sample(temperature=21.0)
            lib.processing_update(ctypes.byref(ctx), ctypes.byref(good), ctypes.byref(out))

        bad = make_sample(valid=False)
        assert not lib.processing_update(ctypes.byref(ctx), ctypes.byref(bad), ctypes.byref(out))

        assert out.status == ENV_SENSOR_FAULT
        # The last known good average survives, so a dropout does not blank the
        # dashboard.
        assert out.temperature_avg == pytest.approx(21.0)
        assert ctx.samples_processed == PROCESSING_WINDOW_LEN

    def test_anomaly_needs_history_first(self, lib, ctx):
        """A big jump in the very first sample has nothing to be anomalous
        against; flagging it would make every boot look like an event."""
        out = ProcessedData()
        s = make_sample(temperature=80.0)
        lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))
        assert not out.anomaly

    def test_anomaly_detected_after_window_fills(self, lib, ctx):
        out = ProcessedData()
        for _ in range(PROCESSING_WINDOW_LEN):
            s = make_sample(temperature=20.0)
            lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))

        spike = make_sample(temperature=40.0)
        lib.processing_update(ctypes.byref(ctx), ctypes.byref(spike), ctypes.byref(out))

        assert out.anomaly
        assert ctx.anomalies_detected == 1

    def test_small_drift_is_not_an_anomaly(self, lib, ctx):
        out = ProcessedData()
        for _ in range(PROCESSING_WINDOW_LEN):
            s = make_sample(temperature=20.0)
            lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))

        s = make_sample(temperature=22.0)
        lib.processing_update(ctypes.byref(ctx), ctypes.byref(s), ctypes.byref(out))
        assert not out.anomaly

    def test_null_arguments_do_not_crash(self, lib, ctx):
        out = ProcessedData()
        assert not lib.processing_update(None, None, ctypes.byref(out))
