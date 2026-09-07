"""
Tests for the wire protocol: the telemetry line format and the command parser.

The formatting and parsing functions are the firmware's own C code compiled for
the host, so these tests pin down exactly what goes on the wire. The ESP32
gateway and scripts/uart_test.py both depend on that format, and a silent change
to it would break them without breaking the build.

The hardware round-trip tests at the bottom run only with --port.
"""
from __future__ import annotations

import ctypes
import re
import time

import pytest

from conftest import (
    CMD_BAD_ARGUMENT,
    CMD_GET_PERIOD,
    CMD_HELP,
    CMD_NONE,
    CMD_PING,
    CMD_RESET_STATS,
    CMD_SAMPLE_NOW,
    CMD_SET_PERIOD,
    CMD_STATUS,
    CMD_UNKNOWN,
    ENV_NORMAL,
    ENV_POOR_AIR,
    Cmd,
    ProcessedData,
    make_sample,
)

SENSOR_PERIOD_MS_MIN = 100
SENSOR_PERIOD_MS_MAX = 60_000


def ftoa(lib, value, decimals):
    buf = ctypes.create_string_buffer(32)
    lib.telemetry_ftoa(value, decimals, buf, len(buf))
    return buf.value.decode()


def parse(lib, text):
    cmd = Cmd()
    ok = lib.cmd_parse(text.encode() if isinstance(text, str) else text, ctypes.byref(cmd))
    return ok, cmd


def fields(line: str) -> dict[str, str]:
    return dict(part.split("=", 1) for part in line.strip().split(",") if "=" in part)


# ---------------------------------------------------------------------------
#  Fixed-point float formatting
#
#  The firmware links newlib-nano, whose printf has no %f, so floats are
#  rendered by hand. That code is easy to get subtly wrong, hence the coverage.
# ---------------------------------------------------------------------------
class TestFtoa:
    @pytest.mark.parametrize(
        "value,decimals,expected",
        [
            (25.3, 2, "25.30"),
            (0.0, 2, "0.00"),
            (1012.4, 1, "1012.4"),
            (45231.0, 0, "45231"),
            (7.0, 3, "7.000"),
            (0.5, 1, "0.5"),
        ],
    )
    def test_basic_values(self, lib, value, decimals, expected):
        assert ftoa(lib, value, decimals) == expected

    def test_negative_values_keep_their_sign(self, lib):
        assert ftoa(lib, -3.25, 2) == "-3.25"
        assert ftoa(lib, -0.5, 1) == "-0.5"

    def test_negative_zero_has_no_sign(self, lib):
        """-0.004 rounds to zero; printing '-0.00' would be noise."""
        assert ftoa(lib, -0.004, 2) == "0.00"

    def test_rounds_half_away_from_zero(self, lib):
        assert ftoa(lib, 2.345, 2) == "2.35"
        assert ftoa(lib, 99.995, 2) == "100.00"

    def test_fraction_is_zero_padded(self, lib):
        """25.05 must not come out as '25.5'."""
        assert ftoa(lib, 25.05, 2) == "25.05"
        assert ftoa(lib, 1.002, 3) == "1.002"

    def test_nan_is_reported_not_garbage(self, lib):
        assert ftoa(lib, float("nan"), 2) == "nan"

    def test_huge_values_do_not_wrap(self, lib):
        """The float->uint32 conversion must be guarded, not silently overflow."""
        assert ftoa(lib, 1e12, 2) in ("inf", "-inf")

    def test_undersized_buffer_stays_terminated(self, lib):
        buf = ctypes.create_string_buffer(4)
        lib.telemetry_ftoa(ctypes.c_float(12345.678), 2, buf, len(buf))
        assert len(buf.value) < 4          # NUL terminated, no overrun


# ---------------------------------------------------------------------------
#  Telemetry line
# ---------------------------------------------------------------------------
class TestTelemetryLine:
    def _line(self, lib, **kwargs):
        data = ProcessedData()
        data.raw = make_sample(**kwargs)
        data.air_quality = 72
        data.status = ENV_NORMAL

        buf = ctypes.create_string_buffer(160)
        n = lib.telemetry_format_line(ctypes.byref(data), buf, len(buf))
        return n, buf.value.decode()

    def test_shape_matches_the_documented_format(self, lib):
        n, line = self._line(
            lib, temperature=25.3, humidity=45.8, pressure=1012.4,
            gas=45231.0, timestamp=12345, sequence=42,
        )
        assert n == len(line)
        assert line.endswith("\n")

        f = fields(line)
        assert f["T"] == "25.30"
        assert f["H"] == "45.80"
        assert f["P"] == "1012.40"
        assert f["G"] == "45231"
        assert f["A"] == "72"
        assert f["S"] == "NORMAL"
        assert f["TS"] == "12345"
        assert f["N"] == "42"

    def test_key_order_is_stable(self, lib):
        """The ESP32 parser is order-independent, but a stable order keeps logs
        diffable and makes regressions obvious."""
        _, line = self._line(lib)
        keys = [p.split("=")[0] for p in line.strip().split(",")]
        assert keys == ["T", "H", "P", "G", "A", "S", "TS", "N"]

    def test_no_spaces_anywhere(self, lib):
        _, line = self._line(lib)
        assert " " not in line

    def test_status_name_appears_verbatim(self, lib):
        data = ProcessedData()
        data.raw = make_sample()
        data.status = ENV_POOR_AIR
        buf = ctypes.create_string_buffer(160)
        lib.telemetry_format_line(ctypes.byref(data), buf, len(buf))
        assert "S=POOR_AIR" in buf.value.decode()

    def test_negative_temperature_survives_the_round_trip(self, lib):
        _, line = self._line(lib, temperature=-12.5)
        assert fields(line)["T"] == "-12.50"

    def test_truncation_is_reported_not_silent(self, lib):
        """A buffer too small must return 0, not a half-line the gateway would
        happily publish as if it were complete."""
        data = ProcessedData()
        data.raw = make_sample()
        buf = ctypes.create_string_buffer(12)
        assert lib.telemetry_format_line(ctypes.byref(data), buf, len(buf)) == 0

    def test_null_data_is_handled(self, lib):
        buf = ctypes.create_string_buffer(64)
        assert lib.telemetry_format_line(None, buf, len(buf)) == 0


class TestStatusResponse:
    def test_contains_every_documented_field(self, lib):
        data = ProcessedData()
        data.raw = make_sample(temperature=25.3, humidity=48.2)
        data.air_quality = 65

        buf = ctypes.create_string_buffer(256)
        n = lib.telemetry_format_status(
            ctypes.byref(data), 1000, 8192, 12345, 42, 0, buf, len(buf)
        )
        assert n > 0

        f = fields(buf.value.decode())
        assert f["TEMP"] == "25.30"
        assert f["HUM"] == "48.20"
        assert f["PERIOD"] == "1000"
        assert f["HEAP"] == "8192"
        assert f["UPTIME"] == "12345"
        assert f["SAMPLES"] == "42"
        assert f["ERRORS"] == "0"

    def test_works_before_the_first_sample(self, lib):
        """STATUS must answer immediately after boot, not wait for data."""
        buf = ctypes.create_string_buffer(256)
        n = lib.telemetry_format_status(None, 1000, 8192, 5, 0, 0, buf, len(buf))
        assert n > 0
        assert "PERIOD=1000" in buf.value.decode()


# ---------------------------------------------------------------------------
#  Command parser
# ---------------------------------------------------------------------------
class TestCommandParser:
    @pytest.mark.parametrize(
        "text,expected",
        [
            ("STATUS", CMD_STATUS),
            ("SAMPLE_NOW", CMD_SAMPLE_NOW),
            ("GET_PERIOD", CMD_GET_PERIOD),
            ("RESET_STATS", CMD_RESET_STATS),
            ("PING", CMD_PING),
            ("HELP", CMD_HELP),
        ],
    )
    def test_each_verb(self, lib, text, expected):
        ok, cmd = parse(lib, text)
        assert ok and cmd.type == expected

    def test_case_insensitive(self, lib):
        for text in ("status", "Status", "sTaTuS"):
            ok, cmd = parse(lib, text)
            assert ok and cmd.type == CMD_STATUS

    def test_surrounding_whitespace_and_line_endings_ignored(self, lib):
        for text in ("  STATUS", "STATUS\r\n", "\tSTATUS \r", " STATUS  \n"):
            ok, cmd = parse(lib, text)
            assert ok and cmd.type == CMD_STATUS

    def test_empty_line_is_not_an_error(self, lib):
        """A bare newline from a terminal must not produce an error reply."""
        for text in ("", "   ", "\r\n"):
            ok, cmd = parse(lib, text)
            assert not ok and cmd.type == CMD_NONE

    def test_set_period_parses_its_argument(self, lib):
        ok, cmd = parse(lib, "SET_PERIOD=500")
        assert ok and cmd.type == CMD_SET_PERIOD and cmd.arg == 500

    def test_set_period_tolerates_spaces_around_equals(self, lib):
        ok, cmd = parse(lib, "SET_PERIOD = 500")
        assert ok and cmd.arg == 500

    @pytest.mark.parametrize("value", [SENSOR_PERIOD_MS_MIN, 500, 1000, SENSOR_PERIOD_MS_MAX])
    def test_period_limits_are_inclusive(self, lib, value):
        ok, cmd = parse(lib, f"SET_PERIOD={value}")
        assert ok and cmd.arg == value

    @pytest.mark.parametrize("value", [0, 1, SENSOR_PERIOD_MS_MIN - 1, SENSOR_PERIOD_MS_MAX + 1, 999999])
    def test_out_of_range_period_rejected(self, lib, value):
        ok, cmd = parse(lib, f"SET_PERIOD={value}")
        assert not ok and cmd.type == CMD_BAD_ARGUMENT

    @pytest.mark.parametrize(
        "text", ["SET_PERIOD", "SET_PERIOD=", "SET_PERIOD=abc", "SET_PERIOD=-100", "SET_PERIOD 500"]
    )
    def test_malformed_argument_rejected(self, lib, text):
        ok, cmd = parse(lib, text)
        assert not ok and cmd.type == CMD_BAD_ARGUMENT

    def test_integer_overflow_in_argument_rejected(self, lib):
        """A 20-digit number must not wrap into a value that looks valid."""
        ok, cmd = parse(lib, "SET_PERIOD=99999999999999999999")
        assert not ok and cmd.type == CMD_BAD_ARGUMENT

    @pytest.mark.parametrize("text", ["STATUSX", "PINGER", "FOO", "ST", "SAMPLE"])
    def test_partial_and_unknown_verbs_rejected(self, lib, text):
        ok, cmd = parse(lib, text)
        assert not ok and cmd.type == CMD_UNKNOWN

    def test_verb_with_trailing_junk_rejected(self, lib):
        ok, cmd = parse(lib, "STATUS extra")
        assert not ok and cmd.type == CMD_UNKNOWN

    def test_null_input_is_safe(self, lib):
        cmd = Cmd()
        assert not lib.cmd_parse(None, ctypes.byref(cmd))
        assert cmd.type == CMD_NONE

    def test_names_round_trip(self, lib):
        assert lib.cmd_name(CMD_STATUS).decode() == "STATUS"
        assert lib.cmd_name(CMD_SET_PERIOD).decode() == "SET_PERIOD"


# ---------------------------------------------------------------------------
#  Hardware round trip -- needs a flashed board:  pytest tests --port COM5
# ---------------------------------------------------------------------------
@pytest.mark.hardware
class TestBoard:
    @staticmethod
    def _ask(conn, command, expect_prefix=None, timeout=3.0):
        conn.reset_input_buffer()
        conn.write((command + "\n").encode())
        conn.flush()

        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = conn.readline().decode(errors="replace").strip()
            if not raw:
                continue
            if expect_prefix is None or raw.startswith(expect_prefix):
                return raw
        pytest.fail(f"no response to {command!r} within {timeout}s")

    def test_ping(self, serial_port):
        assert self._ask(serial_port, "PING", "PONG") == "PONG"

    def test_status_reports_the_documented_fields(self, serial_port):
        line = self._ask(serial_port, "STATUS", "TEMP=")
        f = fields(line)
        for key in ("TEMP", "HUM", "PERIOD", "HEAP", "UPTIME", "SAMPLES", "ERRORS"):
            assert key in f
        assert float(f["TEMP"]) > -50.0
        assert int(f["HEAP"]) > 0

    def test_set_period_is_acknowledged_and_takes_effect(self, serial_port):
        try:
            assert self._ask(serial_port, "SET_PERIOD=500", "OK PERIOD=500")
            assert self._ask(serial_port, "GET_PERIOD", "PERIOD=") == "PERIOD=500"
        finally:
            self._ask(serial_port, "SET_PERIOD=1000", "OK PERIOD=1000")

    def test_out_of_range_period_is_refused_by_the_board(self, serial_port):
        assert self._ask(serial_port, "SET_PERIOD=5", "ERR").startswith("ERR")

    def test_unknown_command_is_refused(self, serial_port):
        assert self._ask(serial_port, "NONSENSE", "ERR").startswith("ERR")

    def test_sample_now_is_acknowledged(self, serial_port):
        assert self._ask(serial_port, "SAMPLE_NOW", "OK SAMPLE_NOW")

    def test_telemetry_arrives_on_schedule(self, serial_port):
        """With a 1000 ms period, at least two samples must appear in 4 s."""
        self._ask(serial_port, "SET_PERIOD=1000", "OK")
        serial_port.reset_input_buffer()

        seen, deadline = 0, time.time() + 4.5
        pattern = re.compile(r"\[\d+ ms\]|T=-?\d+\.\d+")
        while time.time() < deadline:
            raw = serial_port.readline().decode(errors="replace")
            if pattern.search(raw):
                seen += 1
        assert seen >= 2, f"only {seen} lines seen in 4.5 s"
