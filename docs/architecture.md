# Architecture

## System

```
   ┌──────────────────────────────────────────────┐
   │            STM32F103RB @ 64 MHz              │
   │                  FreeRTOS 11.1               │
   │                                              │
   │   sensor ─▶ processing ─▶ comm ──────────────┼──▶ ESP32 ──▶ Wi-Fi ──▶ MQTT
   │      │           │          ▲                │
   │      │           └─ logging ┘                │
   │      │                 │                     │
   │   health ──────────────┘                     │
   └───────┬──────────────────────────┬───────────┘
           │ I²C                      │ USART2
        BME680                   ST-LINK VCP ──▶ PC console + tests
```

The two-processor split is the central design decision. The STM32 does
deterministic sensing and RTOS scheduling; the ESP32 does Wi-Fi and MQTT.
Association, DHCP, TLS and broker reconnection all have unbounded latency, and
none of it is allowed to touch the sampling loop.

---

## Tasks

| Task | Prio | Stack (words) | Blocks on | Responsibility |
|---|---|---|---|---|
| `Sensor` | 3 | 256 | notification + timeout | BME680 forced-mode reads on a fixed period |
| `Process` | 2 | 192 | `g_sensor_queue` | moving average, validation, thresholds, IAQ |
| `Comm` | 2 | 256 | task notification (2 bits) | telemetry TX, command RX and execution |
| `Log` | 1 | 192 | `g_log_queue` | drains log lines to the console UART |
| `Health` | 1 | 192 | periodic delay | heap / stack / queue reporting, I²C sanity read |

Priorities encode **deadlines, not importance**. The sensor task is highest
because it is the only task whose lateness changes the meaning of the data: a
sample taken 200 ms late is a measurement of a different moment. Logging is
lowest because a late log line costs nothing.

The two priority-2 tasks are equal on purpose: they never compete, because each
spends essentially all of its time blocked on a different object.

---

## RTOS objects

| Object | Type | Size | Purpose |
|---|---|---|---|
| `g_sensor_queue` | queue | 8 × 28 B | raw samples, sensor → processing |
| `g_telemetry_queue` | queue | 4 × 60 B | results, processing → comm |
| `g_log_queue` | queue | 12 × 92 B | log lines, anyone → logging |
| `g_i2c_mutex` | mutex | — | arbitrates I²C1 between sensor and health |
| per-link TX mutex | mutex ×2 | — | one writer at a time per UART |
| RX stream buffer | stream ×2 | 128 B | ISR → comm task byte transport |
| sensor notification | task notify | — | button / `SAMPLE_NOW` → sensor task |
| comm notification | task notify | — | telemetry-ready and RX-ready event bits |
| `Heartbeat` | software timer | ~44 B | blinks LD2: 1 Hz healthy, 5 Hz sensor fault |

Queue depths are chosen so a transient stall is absorbed but a persistent one is
*visible*: every queue send uses a zero timeout and increments a drop counter on
failure, which the health task prints. Silent backpressure is worse than a
reported loss.

---

## Design decisions

### Mutex, not binary semaphore, for the I²C bus

FreeRTOS mutexes implement **priority inheritance**; binary semaphores do not.
The low-priority health task and the high-priority sensor task both need I²C1.
Without inheritance, this sequence is possible:

1. `Health` (prio 1) takes the bus.
2. `Sensor` (prio 3) wakes, wants the bus, blocks.
3. `Process` / `Comm` (prio 2) become runnable and pre-empt `Health`.
4. `Sensor` now waits for as long as the prio-2 work chooses to run.

That is unbounded priority inversion, and the sensor task misses its deadline
for reasons that have nothing to do with the sensor. With a mutex, step 2
temporarily promotes `Health` to priority 3, so it finishes and releases the bus
promptly.

Inheritance bounds the damage; it does not eliminate it. The driver also
**releases the bus across the ~180 ms gas-heater conversion** rather than
holding it, so the longest anyone waits is one short register burst.

### Direct-to-task notification, not a semaphore, for the button

There is exactly **one** receiving task. A notification uses the value already
present in that task's TCB, so no separate kernel object is allocated (~80 bytes
of heap saved) and the signalling path is shorter than a semaphore give/take.

A binary semaphore would be the right choice if several tasks could wait on the
event, and a queue would be right if the event carried data. Neither applies.

### Notification *bits* for the comm task

The comm task has two independent wake sources: a queued telemetry result and
received UART bytes. Using `xTaskNotifyWait()` with one bit per source serves
both from a single blocking wait — no polling, no queue set, no timeout tuning.

Telemetry is drained unconditionally on every wake, because notifications
coalesce: two events arriving before the task runs produce one wake. The queue,
not the wake count, is the authority on outstanding work.

### Absolute wake times, not relative delays

`vTaskDelay(1000)` means "sleep 1000 ms **after I finish**", so every cycle is
`period + execution_time` and the sampling rate drifts. A BME680 conversion
takes a variable ~180 ms, so the drift is neither small nor constant.

The sensor task keeps an absolute next-wake tick and blocks for the *remaining*
time, which is what `xTaskDelayUntil()` does internally. It is done by hand here
because the task must also be interruptible by a notification without losing its
schedule. Tick arithmetic uses signed comparison so counter wrap is handled, and
a badly overrun deadline resynchronises instead of firing a catch-up burst.

### Circular DMA + IDLE line for UART reception

Three options for receiving variable-length messages:

| Approach | Cost | Framing |
|---|---|---|
| Interrupt per byte | ~3.5 µs of ISR entry/exit per char at 115200 (~4 % CPU on a continuous stream) | application must scan for a terminator |
| DMA with a fixed length | ~0 | only works if every message is the same size |
| **Circular DMA + IDLE** | ~0 during a burst, one interrupt at the end | the hardware detects the end of transmission |

The third is the only one that is both cheap and length-agnostic. The DMA's
remaining-count register acts as the write pointer into a ring buffer; the ISR
copies whatever is new into a stream buffer and notifies the comm task.

One trap: `HAL_UART_IRQHandler()` has no concept of idle-line reception, so the
IDLE flag must be checked and cleared **before** calling it, or the handler
re-enters forever.

### SysTick shared between the HAL and the kernel

CubeMX's default is to give the HAL its own timer so FreeRTOS can own SysTick.
Both want exactly 1 kHz, so this project has `SysTick_Handler` call
`HAL_IncTick()` and then forward to `xPortSysTickHandler()` — but only once the
scheduler is running, since before that the kernel's task lists do not exist. A
whole TIM peripheral stays free.

### Static allocation for Idle and Timer tasks

With 20 KB of SRAM, the FreeRTOS heap is the biggest single RAM decision.
Allocating the Idle and Timer task stacks statically puts ~1.5 KB in `.bss`,
where over-commitment is a **link error** rather than a boot-time
`pvPortMalloc()` failure that only shows up on hardware.

### A software timer for the heartbeat, not a sixth task

Blinking an LED is one GPIO write on a fixed period. A task would cost a TCB and
a stack (~150 bytes minimum) to spend all its life in `vTaskDelay`. Every
software timer instead shares the single timer-service task, so the marginal cost
of this one is a ~44-byte timer object.

The constraint that comes with it: timer callbacks run in the timer task's
context and must never block — no `vTaskDelay`, no waiting on a mutex — because
doing so stalls every other timer in the system. A GPIO toggle qualifies; reading
a sensor would not.

### Portable logic separated from RTOS glue

`processing.c`, `telemetry.c` and `cmd_parser.c` contain no HAL and no FreeRTOS
calls. They compile for the host, so `tests/` exercises the code that actually
ships rather than a Python model of it. The RTOS glue that *cannot* be tested on
a host is correspondingly thin.

---

## Data flow, one cycle

```
  t=0     sensor task wakes (period elapsed, or button/SAMPLE_NOW notification)
          ├─ take I²C mutex, trigger forced-mode conversion, release
          ├─ vTaskDelay(~180 ms)          <- CPU free for every other task
          ├─ take I²C mutex, read result, release
          └─ xQueueSend(sensor_queue, 0)  <- never blocks

          processing task wakes (was blocked on the queue)
          ├─ plausibility check
          ├─ fold into 8-sample moving averages
          ├─ classify + air-quality score
          ├─ snapshot for the STATUS command
          └─ xQueueSend(telemetry_queue) + notify comm

          comm task wakes (COMM_EVT_TELEMETRY)
          ├─ format "T=..,H=..,..\n" and start a DMA transfer to the ESP32,
          │  block on notification until transfer-complete
          └─ mirror the same reading to the console via the log queue

  every 5 s   health task reports heap, stacks, queues, counters and does an
              I²C chip-ID read (the second bus user)

  on RX idle  ISR copies bytes into a stream buffer, notifies comm (COMM_EVT_RX)
              comm task reassembles a line, parses it, answers on the same link
```
