# Wiring

Board: **NUCLEO-F103RB** (STM32F103RBT6 — Cortex-M3, 128 KB flash, 20 KB SRAM).

Everything below is 3.3 V logic. No level shifters are needed anywhere in this
project.

---

## 1. BME680 over I²C

| BME680 breakout | Nucleo pin | Nucleo header | Note |
|---|---|---|---|
| VIN / VCC | **3V3** | CN6-4 | see the supply warning below |
| GND | **GND** | CN6-6 | |
| SCL | **PB6** | CN10-17 | I2C1_SCL |
| SDA | **PB7** | CN7-21 | I2C1_SDA |

**Supply voltage.** The BME680 die runs at 1.71–3.6 V, but breakout boards differ:
some (Adafruit, Pimoroni) include a regulator and level shifters and accept 5 V
on `VIN`; bare modules do not and will be damaged. Check your specific board
before connecting anything. 3.3 V is safe on every variant.

**Pull-ups.** Almost every BME680 breakout already fits 4.7 kΩ pull-ups on SDA
and SCL, so the firmware configures PB6/PB7 as alternate-function **open-drain
with no internal pull-up**. If you are using a bare module with no pull-ups,
add 4.7 kΩ from each line to 3V3 — I²C cannot work without them, and the
symptom is a clean `NOT_FOUND` at boot.

**Address.** 0x76 or 0x77 depending on how the breakout ties SDO. The firmware
probes 0x76 first, then 0x77, and prints which one answered, so either works
with no configuration.

---

## 2. ESP32 over UART

The STM32 talks to the ESP32 on **USART1**.

| STM32 | Nucleo header | ESP32 DevKit | Direction |
|---|---|---|---|
| **PA9** — USART1_TX | CN5-1 (D8) | **GPIO16** (RX2) | STM32 → ESP32 |
| **PA10** — USART1_RX | CN9-3 (D2) | **GPIO17** (TX2) | ESP32 → STM32 |
| **GND** | CN6-6 | **GND** | — |

```
STM32 PA9  TX  ───────────>  ESP32 GPIO16 RX
STM32 PA10 RX  <───────────  ESP32 GPIO17 TX
STM32 GND      ────────────  ESP32 GND
```

**The ground connection is not optional.** Two separately-powered boards with no
shared ground have no common voltage reference, and the receiver sees noise
rather than data. This is the single most common cause of "my UART link doesn't
work".

Power each board from its own USB port. Do not feed the Nucleo's 3V3 pin from
the ESP32.

### Why USART1 and not USART2?

The original project brief puts the ESP32 on USART2 (PA2/PA3). On a Nucleo-64
those two pins are wired to the **ST-LINK virtual COM port** by default
(SB13/SB14 fitted, SB62/SB63 open). Connecting an ESP32 there means fighting the
on-board debugger for the same pins, and gives up the free USB console.

So this project assigns:

| Link | USART | Pins | Carries |
|---|---|---|---|
| Console | USART2 | PA2 / PA3 → ST-LINK VCP | logs + command interface |
| Telemetry | USART1 | PA9 / PA10 → ESP32 | telemetry + command interface |

Both links accept commands, so the node can be driven from the PC *or* from
MQTT. The console link is what `tests/` and `scripts/uart_test.py` use, which is
why the Python test suite needs no hardware beyond the flashing cable.

To follow the brief exactly instead, build with `-DSWAP_UART_ROLES=ON` and move
the connections accordingly. You will then need a USB-serial adapter on PA9/PA10
for the console.

---

## 3. Button and LED

Both are already on the Nucleo — no external parts needed.

| Function | Pin | Detail |
|---|---|---|
| User button B1 | **PC13** | active low, external pull-up fitted on the board |
| User LED LD2 | **PA5** | active high |

The firmware configures PC13 as a falling-edge EXTI source with the internal
pull-up also enabled, so wiring your own button to a bare GPIO works the same
way:

```
STM32 GPIO ──── button ──── GND
     (internal pull-up: idle HIGH, pressed LOW)
```

Pressing the button forces an immediate sample. Contact bounce is suppressed in
the ISR by timestamp (200 ms), so one press produces one sample.

---

## 4. Clock configuration

The firmware runs **SYSCLK at 64 MHz from the internal HSI** (8 MHz ÷ 2 × 16),
not the more common 72 MHz HSE configuration.

The reason is portability. On a Nucleo-64 there is no crystal fitted at X3; the
8 MHz HSE reference comes from the ST-LINK's MCO output through solder bridges
SB16/SB18. If those are open — which varies by board revision and is invisible
without a magnifier — an HSE build simply never starts, with no diagnostic. HSI
costs 8 MHz of headroom (the node uses ~3 % of the CPU) and always works.

To switch to 72 MHz HSE, replace the oscillator block in `SystemClock_Config()`
(`Core/Src/main.c`) with:

```c
osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
osc.HSEState       = RCC_HSE_ON;
osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
osc.PLL.PLLState   = RCC_PLL_ON;
osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;   /* 8 MHz      */
osc.PLL.PLLMUL     = RCC_PLL_MUL9;        /* 8 * 9 = 72 */
```

and set `APP_SYSCLK_HZ` to `72000000UL` in `Core/Inc/app_config.h` — `perf.c`
uses it to convert cycles to microseconds.

---

## 5. Peripheral and DMA map

| Peripheral | Channel | Use |
|---|---|---|
| I2C1 | polled | BME680, guarded by `g_i2c_mutex` |
| USART1 TX | DMA1_Channel4 | telemetry to ESP32 |
| USART1 RX | DMA1_Channel5 | circular + IDLE line |
| USART2 RX | DMA1_Channel6 | circular + IDLE line |
| USART2 TX | DMA1_Channel7 | console output |
| EXTI13 | — | user button |
| SysTick | — | HAL tick **and** FreeRTOS tick |

Channel assignments are fixed in F1 silicon and cannot be remapped.

Interrupt priorities (4 priority bits, group 4):

| Interrupt | Priority | Reason |
|---|---|---|
| SysTick / PendSV / SVC | 15 (lowest) | set by the FreeRTOS port |
| DMA1 channels 4–7 | 6 | above `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5) |
| USART1 / USART2 | 6 | same |
| EXTI15_10 (button) | 7 | same |

Anything calling a `...FromISR()` API **must** be numerically ≥ 5. Getting this
wrong produces corruption that only appears under load; `configASSERT` catches
it at the call site instead.
