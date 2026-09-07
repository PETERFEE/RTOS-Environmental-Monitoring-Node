# RTOS-Based Wireless Environmental Monitoring Node

A FreeRTOS firmware for the **STM32 NUCLEO-F103RB** that samples a **BME680**
environmental sensor on a deterministic schedule, derives statistics from it, and
streams telemetry to an **ESP32** gateway that republishes it over MQTT.

Five tasks, three queues, a mutex, an interrupt, and DMA in both directions —
built as a single CMake project that compiles from a clean clone with nothing but
`arm-none-eabi-gcc`. No CubeMX generation step, no submodules.

**Flash 36.3 KB / 128 KB · SRAM 13.0 KB / 20 KB** (Release build, all features on)

```
                    ┌─────────────────────┐
                    │   STM32F103RB       │
                    │   FreeRTOS 11.1     │
                    │   64 MHz            │
                    └───┬──────┬──────┬───┘
                        │      │      │
                      I²C    USART2  USART1
                        │      │      │
                    BME680   PC     ESP32 ──▶ Wi-Fi ──▶ MQTT ──▶ dashboard
                             console
```

---

## Contents

- [What it does](#what-it-does)
- [Architecture](#architecture)
- [Hardware and wiring](#hardware-and-wiring)
- [Building](#building)
- [Flashing and running](#flashing-and-running)
- [Command interface](#command-interface)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Design decisions](#design-decisions)
- [Development phases](#development-phases)
- [Third-party code](#third-party-code)

---

## What it does

Once per second (configurable at runtime from 100 ms to 60 s):

1. The **sensor task** runs a BME680 forced-mode conversion — temperature,
   humidity, pressure and gas resistance — and posts the sample to a queue.
2. The **processing task** validates it, folds it into 8-sample moving averages,
   scores air quality and classifies the environment.
3. The **communication task** formats the result and pushes it to the ESP32 over
   DMA.
4. The **logging task** drains debug output to the PC console.
5. Every five seconds the **health task** reports free heap, queue occupancy and
   per-task stack high-water marks.

Pressing the user button forces an immediate sample. Sending `SET_PERIOD=500`
from either the PC or MQTT changes the sampling rate on the fly.

---

## Architecture

```
   ┌──────────────────────────────────────────────────────────┐
   │                    FreeRTOS Scheduler                     │
   └───┬───────────────┬───────────────┬──────────┬───────────┘
       │               │               │          │
  Sensor Task     Processing Task  Comm Task  Logging Task   Health Task
   (prio 3)         (prio 2)       (prio 2)    (prio 1)       (prio 1)
       │               │               │          │              │
       ▼               ▼               ▼          ▼              ▼
   BME680 read    moving avg,     UART DMA    console UART   heap/stack/
   forced mode    thresholds,     + command                  queue report
       │          IAQ score       parsing         ▲              │
       │               │               │          │              │
       └─[sensorQ]────▶┴─[telemetryQ]─▶┘          └──[logQ]──────┘
                                                       ▲
   ┌──────────────┐                                    │
   │ EXTI button  │──▶ task notification ──▶ Sensor Task
   └──────────────┘
```

| Task | Prio | Stack | Blocks on |
|---|---|---|---|
| Sensor | 3 | 256 words | notification + timeout |
| Processing | 2 | 192 words | sensor queue |
| Comm | 2 | 256 words | notification bits |
| Logging | 1 | 192 words | log queue |
| Health | 1 | 192 words | periodic delay |

Plus a FreeRTOS **software timer** that blinks LD2 — 1 Hz while readings are
good, 5 Hz once the sensor returns unusable data — so the board's state is
readable across the room with no terminal attached.

Priorities encode **deadlines, not importance**: the sensor task is highest
because it is the only one whose lateness changes what the data means.

Full reasoning in **[docs/architecture.md](docs/architecture.md)**.

---

## Hardware and wiring

| Item | Notes |
|---|---|
| NUCLEO-F103RB | STM32F103RBT6, on-board ST-LINK |
| BME680 breakout | I²C, address 0x76 or 0x77 (auto-detected) |
| ESP32 DevKit | any standard module |
| 4 jumper wires + 3 more | I²C, then UART |

### BME680 → STM32 (I²C)

| BME680 | STM32 |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SCL | **PB6** |
| SDA | **PB7** |

> Check your breakout's supply rating before applying 5 V — some accept it, bare
> modules do not.

### ESP32 → STM32 (UART)

| STM32 | ESP32 |
|---|---|
| **PA9** (USART1_TX) | GPIO16 (RX2) |
| **PA10** (USART1_RX) | GPIO17 (TX2) |
| GND | GND |

**Both boards must share ground.** Without a common reference the receiver sees
noise, and this is the most common reason a UART link "doesn't work".

The console (logs *and* commands) is on **USART2 → the ST-LINK virtual COM
port**, so no second USB-serial adapter is needed. That differs from the original
brief, which put the ESP32 on USART2 — on a Nucleo-64 those pins belong to the
on-board debugger. Build with `-DSWAP_UART_ROLES=ON` to follow the brief exactly.

Pin maps, DMA channels, interrupt priorities and the 72 MHz HSE variant:
**[docs/wiring.md](docs/wiring.md)**.

---

## Building

Needs CMake ≥ 3.20 and `arm-none-eabi-gcc`. Everything else is vendored.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
cmake --build build
```

Produces `build/rtos-iot-sensor-node.{elf,hex,bin}` and prints the memory report:

```
Memory region         Used Size  Region Size  %age Used
           FLASH:       36276 B       128 KB     27.68%
             RAM:         13 KB        20 KB     65.00%
```

**CLion** is the intended IDE: set the CMake option
`-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake` in the profile and the
targets appear in the dropdown. Step-by-step, including FreeRTOS-aware
debugging where CLion lists the tasks as threads:
**[docs/clion_setup.md](docs/clion_setup.md)**.

### Build options

| Option | Default | Effect |
|---|---|---|
| `USE_UART_DMA` | `ON` | DMA transmit plus circular DMA receive with IDLE-line framing. `OFF` reverts to blocking transmit and disables reception, i.e. the phase-4 behaviour with no command interface — useful for A/B timing comparisons. |
| `SWAP_UART_ROLES` | `OFF` | `ON` puts telemetry on USART2 and the console on USART1. |

---

## Flashing and running

With OpenOCD on `PATH`:

```bash
cmake --build build --target flash
```

Then open the console at **115200 8N1**:

```bash
python scripts/uart_test.py -p /dev/ttyACM0 monitor      # or COM5 on Windows
```

```
=== rtos-iot-sensor-node v1.0 ===
BME680 found at 0x76
starting scheduler
[12 ms] logging task started
[13 ms] comm task started
[1204 ms] T=24.85,H=41.20,P=1011.90,G=48210,A=94,S=NORMAL,TS=1204,N=1
[2204 ms] T=24.87,H=41.18,P=1011.90,G=48355,A=94,S=NORMAL,TS=2204,N=2
[5000 ms] --- health ---
[5000 ms]   heap.free=2024
[5000 ms]   heap.minEver=2024
[5000 ms]   q.sensor=0
[5000 ms]   q.telemetry=0
[5000 ms]   stack.sensor=118
[5000 ms]   stack.comm=131
[5000 ms]   samples=4
[5000 ms]   bus.chipId=97
```

Each reading appears on **both** links: as a log line on the console (shown
above) and as the raw `T=..,H=..` telemetry line on the ESP32 link. The node is
therefore fully observable with nothing attached but the ST-LINK cable.

**LD2 blinks once per second** the whole time. If it speeds up to ~5 Hz the
sensor has started returning unusable data; a fast, urgent blink with no console
output at all means `Error_Handler()` — the board never reached the scheduler.

Press the blue **user button** and a `sensor: forced sample` line appears
immediately, followed by an out-of-schedule reading.

For the ESP32 half, see **[ESP32/README.md](ESP32/README.md)**.

---

## Command interface

Accepted on **either** UART — the PC console or the ESP32 link — and answered on
whichever one asked. Lines are terminated by `\n` or `\r`, and verbs are
case-insensitive.

| Command | Response | Effect |
|---|---|---|
| `STATUS` | `TEMP=..,HUM=..,PRESS=..,GAS=..,AQ=..,PERIOD=..,HEAP=..,UPTIME=..,SAMPLES=..,ERRORS=..` | current state |
| `SAMPLE_NOW` | `OK SAMPLE_NOW` | forces an immediate acquisition |
| `SET_PERIOD=<ms>` | `OK PERIOD=<ms>` | 100–60000 ms |
| `GET_PERIOD` | `PERIOD=<ms>` | |
| `RESET_STATS` | `OK RESET_STATS` | zeroes the counters |
| `PING` | `PONG` | liveness check |
| `HELP` | `COMMANDS: ...` | |
| anything else | `ERR UNKNOWN_COMMAND` | |

Telemetry line format:

```
T=25.30,H=45.80,P=1012.40,G=45231,A=72,S=NORMAL,TS=12345,N=42
```

`T` °C · `H` %RH · `P` hPa · `G` Ω · `A` air-quality 0-100 · `S` status ·
`TS` ms since boot · `N` sample number.

---

## Testing

The firmware's platform-independent modules — `processing.c`, `telemetry.c`,
`cmd_parser.c` — contain no HAL and no FreeRTOS calls, so they compile for the
host and are driven from Python through `ctypes`. **These tests exercise the code
that actually ships, not a re-implementation of it**, and they need no hardware.

```bash
pip install -r tests/requirements.txt
pytest tests -q
```

```
93 passed, 7 skipped
```

The 7 skipped tests are the hardware round-trip suite. With a flashed board:

```bash
pytest tests -q --port /dev/ttyACM0
```

They check that `PING` answers `PONG`, that `STATUS` reports every documented
field, that `SET_PERIOD` is accepted *and takes effect*, that out-of-range and
malformed commands are refused, and that telemetry arrives on schedule.

There is also an end-to-end command check and a jitter benchmark:

```bash
python scripts/uart_test.py -p /dev/ttyACM0 selftest
python scripts/uart_test.py -p /dev/ttyACM0 bench --period 500 --seconds 30
```

---

## Repository layout

```
rtos-iot-sensor-node/
├── CMakeLists.txt              build for the firmware
├── cmake/arm-none-eabi-gcc.cmake
├── linker/STM32F103RB.ld       written for this project
├── openocd/stm32f103rb.cfg     ST-LINK + FreeRTOS thread awareness
│
├── Core/
│   ├── Inc/                    app_config.h is the single source of pins,
│   │                           periods, stack sizes and thresholds
│   └── Src/
│       ├── main.c              bring-up only: clocks, peripherals, scheduler
│       ├── app_tasks.c         every queue/mutex/task created in one place
│       ├── sensor_task.c       ─┐
│       ├── processing_task.c    │
│       ├── comm_task.c          ├─ RTOS glue
│       ├── logging_task.c       │
│       ├── health_task.c       ─┘
│       ├── processing.c        ─┐
│       ├── telemetry.c          ├─ portable logic, unit-tested on the host
│       ├── cmd_parser.c        ─┘
│       ├── bme680_driver.c     Bosch API port layer
│       ├── uart_dma.c          DMA TX + circular RX with IDLE framing
│       ├── perf.c              DWT cycle counter
│       └── stm32f1xx_it.c      ISRs, incl. a fault handler that prints the frame
│
├── Drivers/                    CMSIS, STM32F1 HAL, Bosch BME68x  (vendored)
├── Middleware/FreeRTOS-Kernel/ FreeRTOS 11.1.0                   (vendored)
├── ESP32/mqtt_gateway.ino      UART → JSON → MQTT
├── tests/                      pytest + ctypes; native/ builds the host library
├── scripts/uart_test.py        monitor / send / selftest / bench
└── docs/                       architecture, wiring, measurements, CLion setup
```

---

## Design decisions

The parts worth talking through — expanded in
[docs/architecture.md](docs/architecture.md).

**A mutex for I²C, not a binary semaphore.** The low-priority health task and
the high-priority sensor task share the bus. FreeRTOS mutexes implement priority
inheritance; binary semaphores do not. Without it, a mid-priority task running
while `Health` holds the bus blocks `Sensor` for an unbounded time. The driver
also *releases* the bus across the ~180 ms gas-heater conversion rather than
holding it, so the worst wait is one short register burst.

**A direct-to-task notification for the button, not a semaphore.** There is
exactly one receiving task, so the notification value already in its TCB is
enough: no kernel object allocated (~80 bytes of heap), shorter signalling path.
A semaphore would be right for multiple waiters; a queue for carrying data.

**Notification *bits* for the comm task.** It has two independent wake sources —
telemetry queued, and bytes received. One `xTaskNotifyWait()` with a bit per
source serves both without polling or a queue set. Telemetry is drained
unconditionally on every wake, because notifications coalesce and the queue, not
the wake count, is the authority on outstanding work.

**Absolute wake times, not `vTaskDelay`.** `vTaskDelay(1000)` sleeps 1000 ms
*after the work finishes*, so every cycle costs `period + execution_time` and the
rate drifts — badly, when the work is a variable ~180 ms conversion. The sensor
task tracks an absolute next-wake tick and blocks for the remainder, using signed
tick arithmetic so counter wrap is handled correctly.

**Circular DMA + IDLE-line detection for receive.** An interrupt per byte costs
~3.5 µs of ISR entry/exit per character at 115200 (~4 % CPU on a continuous
stream) and still cannot frame a variable-length message. Circular DMA plus the
USART's IDLE detector gives zero interrupts during a burst and exactly one at the
end, whatever the length. The IDLE flag must be cleared before calling
`HAL_UART_IRQHandler()`, which has no concept of idle-line reception and would
otherwise re-enter forever.

**One SysTick for both time bases.** CubeMX's default is a separate TIM for the
HAL. Both want 1 kHz, so `SysTick_Handler` calls `HAL_IncTick()` and forwards to
`xPortSysTickHandler()` once the scheduler is running. A TIM stays free.

**No `printf`.** newlib-nano's `printf` has no `%f`, and pulling in the full
version costs several KB on a 128 KB part. Floats are formatted in fixed point by
`telemetry_ftoa()` — which is exactly the kind of hand-rolled code that goes
wrong quietly, so it has its own test class.

**Static allocation for the Idle and Timer tasks.** Their ~1.5 KB of stacks land
in `.bss`, so over-committing RAM is a link error rather than a
`pvPortMalloc()` failure discovered on hardware.

**Zero-timeout queue sends with drop counters.** Every producer refuses to block.
A stall is absorbed if transient and *reported* if not — silent backpressure on
the one task with a deadline would be worse than a counted loss.

---

## Development phases

The git history is the build order, one commit per phase, each one compiling:

| Phase | Commit | Introduces |
|---|---|---|
| — | vendor + build system | CMSIS, HAL, FreeRTOS, Bosch driver, CMake, linker script |
| 0 | bare-metal bring-up | clocks, I²C, UART, BME680, fault handler |
| 1 | FreeRTOS basics | scheduler, two demo tasks, priorities, tick |
| 2–3 | sensor + queue | `xTaskDelayUntil`, queue decoupling, blocking receive |
| 4 | comm + logging | telemetry format, async logging, UART abstraction |
| 5 | mutex + health | priority inheritance, heap/stack/queue reporting |
| 6 | interrupt | EXTI, `FromISR` APIs, task notification, debounce |
| 7 | DMA | DMA TX, circular RX, IDLE framing, command interface |
| 8 | testing + docs | ESP32 gateway, pytest suite, tooling, documentation |

`git log --oneline` reads as a walkthrough; each commit message explains the
decision it encodes.

---

## Third-party code

Everything under `Drivers/` and `Middleware/` is unmodified upstream code,
vendored at a pinned commit — see **[VENDOR.md](VENDOR.md)** for versions and
licences (Apache-2.0, BSD-3-Clause, MIT).

Application code in `Core/`, `ESP32/`, `tests/`, `scripts/` and the linker script
are original to this project.

The air-quality score is a documented weighted heuristic over gas resistance and
humidity. It is **not** Bosch's calibrated BSEC IAQ index, which is a separate
closed-source library; see [docs/measurements.md](docs/measurements.md).
