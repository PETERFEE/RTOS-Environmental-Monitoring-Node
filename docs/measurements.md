# Measurements

Two kinds of number live here.

**Static figures** come out of the build and are reproducible on any machine —
they are filled in below.

**Runtime figures** depend on your board, your sensor and your room, so they are
left as a table to fill in with the procedure that produces each one. Copying
someone else's runtime numbers into a README is the fastest way to be caught out
in an interview; measuring your own takes ten minutes.

---

## 1. Static: image size

`arm-none-eabi-size` on the linked ELF, FreeRTOS 11.1 + STM32F1 HAL + Bosch
BME68x, all five tasks, DMA enabled:

| Build | text (flash) | data | bss | Flash used | SRAM used |
|---|---|---|---|---|---|
| Debug (`-Og -g3`) | 42 504 B | 32 B | 13 296 B | 42.5 KB / 128 KB (33 %) | 13.3 KB / 20 KB (65 %) |
| Release (`-Os`) | 36 240 B | 32 B | 13 288 B | 36.3 KB / 128 KB (28 %) | 13.0 KB / 20 KB (65 %) |

Reproduce with:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
cmake --build build-release
```

The linker prints the region table on every build (`--print-memory-usage`), so a
change that costs RAM is visible immediately rather than at the next link
failure.

### Where the SRAM goes

Largest statically allocated objects (`arm-none-eabi-nm --size-sort -S`):

| Symbol | Bytes | What |
|---|---|---|
| `ucHeap` | 9 216 | the entire FreeRTOS heap (`configTOTAL_HEAP_SIZE`) |
| `s_timer_stack` | 512 | Timer task stack, statically allocated |
| `s_idle_stack` | 512 | Idle task stack, statically allocated |
| `s_links` | 296 | UART contexts incl. two 128 B DMA ring buffers |
| `s_ctx` | 152 | processing context (four 8-sample windows) |

Everything dynamic — task stacks, TCBs, queues, mutexes, stream buffers — is
carved out of `ucHeap`. Budget at creation time:

| Allocation | Bytes |
|---|---|
| 5 task stacks (256+192+256+192+192 words) | 4 352 |
| 5 TCBs | ~440 |
| `g_sensor_queue` 8 × 28 B | ~304 |
| `g_telemetry_queue` 4 × 60 B | ~320 |
| `g_log_queue` 12 × 92 B | ~1 184 |
| 3 mutexes (I²C + 2 × UART TX) | ~240 |
| 2 stream buffers, 128 B each | ~320 |
| **Total** | **~7 200 of 9 216 (≈ 2.0 KB spare)** |

The health task prints the real figure every 5 s, so this estimate can be
checked rather than trusted.

---

## 2. Runtime: what to record

Fill these in from your own board.

| Metric | Value | How |
|---|---|---|
| Sensor period (commanded) | 1000 ms | `GET_PERIOD` |
| Mean telemetry interval | ___ ms | `uart_test.py bench` |
| Peak jitter | ___ ms | same |
| Sensor task execution time | ___ µs | DWT counter, below |
| Processing task execution time | ___ µs | DWT counter, below |
| Telemetry DMA transfer | ___ µs | DWT counter, below |
| Free heap, steady state | ___ B | health report `heap.free` |
| Free heap, minimum ever | ___ B | health report `heap.minEver` |
| Sensor stack high-water | ___ words | health report `stack.sensor` |
| Comm stack high-water | ___ words | health report `stack.comm` |
| Max sensor queue occupancy | ___ / 8 | health report `q.sensor` |
| Queue drops in 1 h | ___ | health report `qDrops` |
| CPU load | ___ % | see §5 |

### Period and jitter

```bash
python scripts/uart_test.py -p /dev/ttyACM0 bench --period 1000 --seconds 60
```

Reports mean, min, max, standard deviation and peak jitter. Note that host-side
timestamps include USB and OS scheduling latency, so the result is an **upper
bound** on the firmware's own jitter, not a measurement of it. For a tighter
figure, use the DWT method below or a scope on the LED pin.

### Heap, stacks and queues

Just watch the console — the health task prints all of it every 5 s:

```
[15000 ms] --- health ---
[15000 ms]   heap.free=2216
[15000 ms]   heap.minEver=2216
[15000 ms]   q.sensor=0
[15000 ms]   stack.sensor=118
[15000 ms]   stack.comm=131
...
```

`stack.*` is `uxTaskGetStackHighWaterMark()`: the minimum number of **words**
that have ever been left unused. Multiply by 4 for bytes. Useful rules of thumb:

* below ~30 words — increase the stack, you are one deep call from corruption
* above ~120 words — the stack is over-provisioned; that RAM could be heap

Let it run for at least an hour, including a few button presses and commands, so
the worst-case paths are actually taken before you read the numbers.

---

## 3. Timing individual tasks with the DWT counter

`perf.c` exposes the Cortex-M3 cycle counter: 15.6 ns resolution at 64 MHz.
Wrap the section you care about:

```c
#include "perf.h"

const uint32_t t0 = perf_cycles();
/* ... the work being measured ... */
const uint32_t us = perf_elapsed_us(t0);
log_kv_u32("processing.us", us);
```

Worth measuring:

* **sensor task** — should be a few hundred µs of actual work; the ~180 ms
  conversion is `vTaskDelay`, not CPU time, and must not be counted
* **processing task** — moving averages plus classification, soft-float on a
  Cortex-M3
* **`telemetry_format_line()`** — the fixed-point formatter that exists to avoid
  linking `printf`'s float support
* **`uart_dma_send()`** — call-to-return with DMA should be far below the
  ~5.2 ms a 60-byte line takes on the wire at 115200 baud; that gap is the whole
  point of using DMA

---

## 4. Demonstrating that DMA does something

Build both ways and compare `uart_dma_send()` call-to-return:

```bash
cmake -B build-dma    -DUSE_UART_DMA=ON  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
cmake -B build-nodma  -DUSE_UART_DMA=OFF -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
```

(The `OFF` build has no receive path, so drive it from the telemetry side rather
than by sending commands.) The blocking build occupies the calling task for the
full transmission time. The
DMA build returns as soon as the transfer is started and the task blocks, so the
CPU is available to everything else. At 115200 baud, 8N1, a 60-byte line is
60 × 10 / 115200 ≈ **5.2 ms** — per sample, per second, on a core that is
otherwise ~97 % idle.

| Path | Task blocked | CPU busy |
|---|---|---|
| `HAL_UART_Transmit` | ~5.2 ms | ~5.2 ms |
| DMA + notification | ~5.2 ms | ~___ µs (measure) |

---

## 5. Estimating CPU load

There is no run-time-stats timer configured (it costs RAM and a TIM), so the
simple estimate is:

```
load ≈ (sensor_us + processing_us + comm_us) / period_us
```

With sub-millisecond task bodies at a 1000 ms period this lands in the low single
digits of a percent, which is the expected answer for a node that spends most of
its life in `vTaskDelay` and blocked on queues. If you want a measured figure,
set `configGENERATE_RUN_TIME_STATS 1` and feed it a spare TIM.

---

## 6. Sensor calibration note

`IAQ_GAS_BASELINE_OHM` in `app_config.h` defaults to 50 kΩ. The correct value is
per-sensor and per-environment: run the node in clean air for ~20 minutes, watch
the `G=` field stabilise, and set the baseline to that reading. Until you do,
the air-quality score is a relative indicator, not an absolute one.

The score is a weighted heuristic (75 % gas, 25 % humidity), **not** Bosch's
calibrated BSEC IAQ index. BSEC is a separate closed-source library with its own
licence; this project deliberately uses only the open BME68x driver, and says so
rather than implying a calibration it does not have.
