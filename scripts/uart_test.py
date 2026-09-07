#!/usr/bin/env python3
"""
uart_test.py -- talk to the node over its serial console.

The board's console is the ST-LINK virtual COM port, so this needs no hardware
beyond the USB cable already used to flash it.

    python scripts/uart_test.py --list
    python scripts/uart_test.py -p COM5 monitor
    python scripts/uart_test.py -p /dev/ttyACM0 send STATUS
    python scripts/uart_test.py -p /dev/ttyACM0 selftest
    python scripts/uart_test.py -p /dev/ttyACM0 bench --period 500 --seconds 30

`bench` is the one that produces numbers for the README: it measures the actual
interval between telemetry lines and reports mean/min/max jitter against the
period the firmware was told to use.
"""
from __future__ import annotations

import argparse
import statistics
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")


TELEMETRY_KEYS = ("T", "H", "P", "G")


def parse_fields(line: str) -> dict[str, str]:
    return dict(p.split("=", 1) for p in line.strip().split(",") if "=" in p)


def is_telemetry(line: str) -> bool:
    f = parse_fields(line)
    return all(k in f for k in TELEMETRY_KEYS)


def open_port(args) -> serial.Serial:
    try:
        conn = serial.Serial(args.port, args.baud, timeout=args.timeout)
    except serial.SerialException as exc:
        sys.exit(f"cannot open {args.port}: {exc}")
    time.sleep(0.1)
    conn.reset_input_buffer()
    return conn


def ask(conn: serial.Serial, command: str, timeout: float = 3.0) -> str | None:
    """Send a command and return the first line that is not stray telemetry."""
    conn.reset_input_buffer()
    conn.write((command + "\n").encode())
    conn.flush()

    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = conn.readline().decode(errors="replace").strip()
        if not raw or is_telemetry(raw):
            continue
        if raw.startswith("[") and " ms] " in raw:
            continue                       # a log line, not our answer
        return raw
    return None


# ---------------------------------------------------------------------------
def cmd_list(args) -> int:
    ports = list(list_ports.comports())
    if not ports:
        print("no serial ports found")
        return 1
    for p in ports:
        print(f"{p.device:20} {p.description}")
    return 0


def cmd_monitor(args) -> int:
    conn = open_port(args)
    print(f"monitoring {args.port} at {args.baud} baud -- Ctrl-C to stop\n")
    try:
        while True:
            raw = conn.readline().decode(errors="replace").rstrip()
            if raw:
                print(raw)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        conn.close()
    return 0


def cmd_send(args) -> int:
    conn = open_port(args)
    try:
        reply = ask(conn, " ".join(args.command))
        if reply is None:
            print("no response", file=sys.stderr)
            return 1
        print(reply)
        return 0 if not reply.startswith("ERR") else 1
    finally:
        conn.close()


def cmd_selftest(args) -> int:
    """Exercise every command and check the shape of each reply."""
    conn = open_port(args)
    failures = 0

    def check(label: str, command: str, predicate, hint: str) -> None:
        nonlocal failures
        reply = ask(conn, command)
        ok = reply is not None and predicate(reply)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label:34} {reply!r}")
        if not ok:
            failures += 1
            print(f"         expected: {hint}")

    try:
        print(f"self-test against {args.port}\n")
        check("PING answers PONG", "PING",
              lambda r: r == "PONG", "PONG")
        check("STATUS reports all fields", "STATUS",
              lambda r: all(k in parse_fields(r) for k in
                            ("TEMP", "HUM", "PERIOD", "HEAP", "UPTIME")),
              "TEMP=..,HUM=..,PERIOD=..,HEAP=..,UPTIME=..")
        check("GET_PERIOD answers", "GET_PERIOD",
              lambda r: r.startswith("PERIOD="), "PERIOD=<ms>")
        check("SET_PERIOD accepted", "SET_PERIOD=500",
              lambda r: r == "OK PERIOD=500", "OK PERIOD=500")
        check("period actually changed", "GET_PERIOD",
              lambda r: r == "PERIOD=500", "PERIOD=500")
        check("SET_PERIOD restored", "SET_PERIOD=1000",
              lambda r: r == "OK PERIOD=1000", "OK PERIOD=1000")
        check("too-small period refused", "SET_PERIOD=5",
              lambda r: r.startswith("ERR"), "ERR ...")
        check("too-large period refused", "SET_PERIOD=999999",
              lambda r: r.startswith("ERR"), "ERR ...")
        check("garbage argument refused", "SET_PERIOD=abc",
              lambda r: r.startswith("ERR"), "ERR ...")
        check("unknown command refused", "NONSENSE",
              lambda r: r.startswith("ERR"), "ERR UNKNOWN_COMMAND")
        check("SAMPLE_NOW accepted", "SAMPLE_NOW",
              lambda r: r.startswith("OK"), "OK SAMPLE_NOW")
        check("HELP lists commands", "HELP",
              lambda r: "STATUS" in r and "SET_PERIOD" in r, "COMMANDS: ...")

        print(f"\n{'all checks passed' if failures == 0 else f'{failures} check(s) failed'}")
        return 1 if failures else 0
    finally:
        conn.close()


def cmd_bench(args) -> int:
    """Measure the real telemetry interval and its jitter."""
    conn = open_port(args)
    try:
        reply = ask(conn, f"SET_PERIOD={args.period}")
        if reply is None or not reply.startswith("OK"):
            print(f"could not set period: {reply!r}", file=sys.stderr)
            return 1

        print(f"measuring for {args.seconds}s at a {args.period} ms period...\n")
        conn.reset_input_buffer()

        stamps: list[float] = []
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            raw = conn.readline().decode(errors="replace")
            if raw and is_telemetry(raw):
                stamps.append(time.perf_counter())

        if len(stamps) < 3:
            print(f"only {len(stamps)} telemetry lines seen -- is the sensor "
                  "connected and is this the telemetry link?", file=sys.stderr)
            return 1

        gaps = [(b - a) * 1000.0 for a, b in zip(stamps, stamps[1:])]
        mean = statistics.mean(gaps)

        print(f"  samples          {len(stamps)}")
        print(f"  commanded period {args.period:.1f} ms")
        print(f"  mean interval    {mean:.2f} ms")
        print(f"  min / max        {min(gaps):.2f} / {max(gaps):.2f} ms")
        print(f"  std deviation    {statistics.pstdev(gaps):.2f} ms")
        print(f"  peak jitter      {max(abs(g - args.period) for g in gaps):.2f} ms")
        print(f"  mean error       {mean - args.period:+.2f} ms")
        print("\nNote: host-side timestamps include USB and OS scheduling "
              "latency, so this is an upper bound on the firmware's own jitter.")
        return 0
    finally:
        ask(conn, "SET_PERIOD=1000")
        conn.close()


# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Talk to the rtos-iot-sensor-node over serial.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-p", "--port", help="serial port, e.g. COM5 or /dev/ttyACM0")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=2.0)

    sub = parser.add_subparsers(dest="cmd")
    sub.add_parser("list", help="list available serial ports")
    sub.add_parser("monitor", help="print everything the board sends")
    sub.add_parser("selftest", help="exercise every command and check the replies")

    p_send = sub.add_parser("send", help="send one command and print the reply")
    p_send.add_argument("command", nargs="+")

    p_bench = sub.add_parser("bench", help="measure telemetry interval and jitter")
    p_bench.add_argument("--period", type=int, default=1000)
    p_bench.add_argument("--seconds", type=float, default=30.0)

    args = parser.parse_args()

    if args.cmd is None:
        parser.print_help()
        return 1
    if args.cmd == "list":
        return cmd_list(args)
    if not args.port:
        return parser.error("--port is required for this command "
                            "(use 'list' to find one)")

    return {
        "monitor": cmd_monitor,
        "send": cmd_send,
        "selftest": cmd_selftest,
        "bench": cmd_bench,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
