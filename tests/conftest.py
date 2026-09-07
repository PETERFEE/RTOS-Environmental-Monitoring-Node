"""
Shared pytest fixtures.

Two kinds of test live in this directory:

* Host tests -- compile the firmware's platform-independent modules
  (processing.c, telemetry.c, cmd_parser.c) into a shared library and drive
  them through ctypes. These test the code that actually ships, not a Python
  re-implementation of it, and they need no hardware.

* Hardware tests -- talk to a flashed board over its serial port. Skipped
  unless --port is given:

      pytest tests --port /dev/ttyACM0        (Linux)
      pytest tests --port COM5                (Windows)
"""
from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
NATIVE_DIR = REPO_ROOT / "tests" / "native"
BUILD_DIR = NATIVE_DIR / "build"

LIB_NAMES = ("libapp_logic.so", "libapp_logic.dylib", "app_logic.dll")


def pytest_addoption(parser):
    parser.addoption(
        "--port",
        action="store",
        default=None,
        help="Serial port of a flashed board, e.g. /dev/ttyACM0 or COM5. "
        "Hardware tests are skipped when this is not given.",
    )
    parser.addoption(
        "--baud", action="store", default=115200, type=int, help="Serial baud rate."
    )


def _find_library() -> Path | None:
    for name in LIB_NAMES:
        for candidate in (BUILD_DIR / name, BUILD_DIR / "Debug" / name):
            if candidate.exists():
                return candidate
    return None


def _build_library() -> Path:
    lib = _find_library()
    if lib is not None:
        return lib

    try:
        subprocess.run(
            ["cmake", "-S", str(NATIVE_DIR), "-B", str(BUILD_DIR)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["cmake", "--build", str(BUILD_DIR)], check=True, capture_output=True
        )
    except FileNotFoundError:
        pytest.skip("cmake not found; cannot build the host test library")
    except subprocess.CalledProcessError as exc:
        pytest.fail(
            "Failed to build the host test library:\n"
            + exc.stderr.decode(errors="replace")
        )

    lib = _find_library()
    if lib is None:
        pytest.fail(f"Build succeeded but no library found in {BUILD_DIR}")
    return lib


# ---------------------------------------------------------------------------
#  ctypes mirrors of the firmware's structures
# ---------------------------------------------------------------------------
PROCESSING_WINDOW_LEN = 8


class SensorData(ctypes.Structure):
    _fields_ = [
        ("temperature", ctypes.c_float),
        ("humidity", ctypes.c_float),
        ("pressure", ctypes.c_float),
        ("gas_resistance", ctypes.c_float),
        ("timestamp", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("valid", ctypes.c_bool),
        ("forced", ctypes.c_bool),
    ]


class ProcessedData(ctypes.Structure):
    _fields_ = [
        ("raw", SensorData),
        ("temperature_avg", ctypes.c_float),
        ("humidity_avg", ctypes.c_float),
        ("pressure_avg", ctypes.c_float),
        ("gas_avg", ctypes.c_float),
        ("temperature_delta", ctypes.c_float),
        ("air_quality", ctypes.c_uint8),
        ("status", ctypes.c_int),
        ("anomaly", ctypes.c_bool),
    ]


class MovingAvg(ctypes.Structure):
    _fields_ = [
        ("window", ctypes.c_float * PROCESSING_WINDOW_LEN),
        ("count", ctypes.c_uint8),
        ("head", ctypes.c_uint8),
    ]


class ProcessingCtx(ctypes.Structure):
    _fields_ = [
        ("temperature", MovingAvg),
        ("humidity", MovingAvg),
        ("pressure", MovingAvg),
        ("gas", MovingAvg),
        ("samples_processed", ctypes.c_uint32),
        ("anomalies_detected", ctypes.c_uint32),
    ]


class Cmd(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("arg", ctypes.c_uint32)]


# Mirrors env_status_t
ENV_NORMAL, ENV_WARM, ENV_COLD, ENV_HUMID, ENV_DRY, ENV_POOR_AIR, ENV_SENSOR_FAULT = range(7)

# Mirrors cmd_type_t
(
    CMD_NONE,
    CMD_STATUS,
    CMD_SAMPLE_NOW,
    CMD_SET_PERIOD,
    CMD_GET_PERIOD,
    CMD_RESET_STATS,
    CMD_PING,
    CMD_HELP,
    CMD_UNKNOWN,
    CMD_BAD_ARGUMENT,
) = range(10)


@pytest.fixture(scope="session")
def lib():
    """The firmware's portable logic, compiled for the host."""
    dll = ctypes.CDLL(str(_build_library()))

    dll.moving_avg_reset.argtypes = [ctypes.POINTER(MovingAvg)]
    dll.moving_avg_push.argtypes = [ctypes.POINTER(MovingAvg), ctypes.c_float]
    dll.moving_avg_value.argtypes = [ctypes.POINTER(MovingAvg)]
    dll.moving_avg_value.restype = ctypes.c_float
    dll.moving_avg_ready.argtypes = [ctypes.POINTER(MovingAvg)]
    dll.moving_avg_ready.restype = ctypes.c_bool

    dll.processing_init.argtypes = [ctypes.POINTER(ProcessingCtx)]
    dll.processing_update.argtypes = [
        ctypes.POINTER(ProcessingCtx),
        ctypes.POINTER(SensorData),
        ctypes.POINTER(ProcessedData),
    ]
    dll.processing_update.restype = ctypes.c_bool
    dll.processing_air_quality.argtypes = [ctypes.c_float, ctypes.c_float]
    dll.processing_air_quality.restype = ctypes.c_uint8
    dll.processing_classify.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_float]
    dll.processing_classify.restype = ctypes.c_int
    dll.processing_sample_is_plausible.argtypes = [ctypes.POINTER(SensorData)]
    dll.processing_sample_is_plausible.restype = ctypes.c_bool

    dll.telemetry_ftoa.argtypes = [
        ctypes.c_float,
        ctypes.c_uint8,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    dll.telemetry_ftoa.restype = ctypes.c_size_t
    dll.telemetry_format_line.argtypes = [
        ctypes.POINTER(ProcessedData),
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    dll.telemetry_format_line.restype = ctypes.c_size_t
    dll.telemetry_format_status.argtypes = [
        ctypes.POINTER(ProcessedData),
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    dll.telemetry_format_status.restype = ctypes.c_size_t
    dll.telemetry_status_name.argtypes = [ctypes.c_int]
    dll.telemetry_status_name.restype = ctypes.c_char_p

    dll.cmd_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(Cmd)]
    dll.cmd_parse.restype = ctypes.c_bool
    dll.cmd_name.argtypes = [ctypes.c_int]
    dll.cmd_name.restype = ctypes.c_char_p

    return dll


def make_sample(temperature=25.0, humidity=45.0, pressure=1012.0, gas=50000.0,
                timestamp=0, sequence=1, valid=True, forced=False) -> SensorData:
    return SensorData(
        temperature=temperature,
        humidity=humidity,
        pressure=pressure,
        gas_resistance=gas,
        timestamp=timestamp,
        sequence=sequence,
        valid=valid,
        forced=forced,
    )


@pytest.fixture
def ctx(lib):
    c = ProcessingCtx()
    lib.processing_init(ctypes.byref(c))
    return c


@pytest.fixture(scope="session")
def serial_port(request):
    """An open serial connection to a flashed board, or a skip."""
    port = request.config.getoption("--port")
    if not port:
        pytest.skip("no --port given; hardware tests skipped")

    serial = pytest.importorskip("serial", reason="pyserial is not installed")

    baud = request.config.getoption("--baud")
    with serial.Serial(port, baud, timeout=2.0) as conn:
        conn.reset_input_buffer()
        yield conn
