/**
 * @file    app_config.h
 * @brief   Single place for every board pin, task parameter and tunable.
 *
 * Nothing in this file depends on FreeRTOS or the HAL, so the host-side unit
 * tests in tests/native can include it too.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ==========================================================================
 *  Board: NUCLEO-F103RB (STM32F103RBT6, 128 KB flash, 20 KB SRAM)
 * ========================================================================== */

/* --- Clock ---------------------------------------------------------------
 * SYSCLK = 64 MHz from the internal 8 MHz HSI (HSI/2 * 16).
 *
 * Using HSI rather than the 72 MHz HSE configuration means the firmware runs
 * on any F103RB board regardless of whether SB16/SB18 are fitted to route the
 * ST-LINK's 8 MHz MCO into OSC_IN. See docs/wiring.md for the HSE variant.
 * ------------------------------------------------------------------------ */
#define APP_SYSCLK_HZ                   64000000UL

/* --- I2C1: BME680 -------------------------------------------------------- */
#define BME680_I2C_SCL_PORT             GPIOB
#define BME680_I2C_SCL_PIN              GPIO_PIN_6      /* PB6 */
#define BME680_I2C_SDA_PORT             GPIOB
#define BME680_I2C_SDA_PIN              GPIO_PIN_7      /* PB7 */
#define BME680_I2C_SPEED_HZ             100000U         /* standard mode      */

/* 7-bit addresses. SDO tied low -> 0x76, SDO tied high -> 0x77.
 * bme680_driver.c probes primary first, then secondary, so either works. */
#define BME680_I2C_ADDR_PRIMARY         0x76U
#define BME680_I2C_ADDR_SECONDARY       0x77U

/* --- UARTs ---------------------------------------------------------------
 * USART2 (PA2/PA3) is hard-wired to the ST-LINK virtual COM port on every
 * Nucleo-64 board, so it is the natural home for the PC log + command console
 * (no extra USB-serial adapter needed, and the Python tests in tests/ talk to
 * it directly). USART1 (PA9/PA10) is broken out on the headers and carries the
 * telemetry link to the ESP32.
 *
 * Build with -DSWAP_UART_ROLES=ON to follow the original project brief exactly
 * (ESP32 on USART2 / logs on USART1) if you would rather use an external
 * USB-serial adapter for the console.
 * ------------------------------------------------------------------------ */
#define APP_UART_BAUDRATE               115200U

#if APP_SWAP_UART_ROLES
  #define APP_LOG_USART                 USART1          /* PA9 TX / PA10 RX  */
  #define APP_COMM_USART                USART2          /* PA2 TX / PA3 RX   */
#else
  #define APP_LOG_USART                 USART2          /* ST-LINK VCP       */
  #define APP_COMM_USART                USART1          /* ESP32             */
#endif

/* --- User LED (LD2) and user button (B1) --------------------------------- */
#define APP_LED_PORT                    GPIOA
#define APP_LED_PIN                     GPIO_PIN_5      /* PA5  = LD2        */

#define APP_BUTTON_PORT                 GPIOC
#define APP_BUTTON_PIN                  GPIO_PIN_13     /* PC13 = B1, active low */
#define APP_BUTTON_IRQn                 EXTI15_10_IRQn
#define APP_BUTTON_DEBOUNCE_MS          200U

/* ==========================================================================
 *  Interrupt priorities (NVIC_PRIORITYGROUP_4: 4 preempt bits, 0 sub bits)
 *
 *  Anything that calls a FreeRTOS ...FromISR() API must have a numerically
 *  HIGHER (= logically lower) priority than configMAX_SYSCALL_INTERRUPT_PRIORITY,
 *  which this project sets to 5. Everything below therefore sits in 5..15.
 * ========================================================================== */
#define APP_IRQ_PRIO_DMA                6U
#define APP_IRQ_PRIO_USART              6U
#define APP_IRQ_PRIO_BUTTON             7U

/* ==========================================================================
 *  FreeRTOS task configuration
 *
 *  Stack depths are in WORDS (4 bytes each on Cortex-M3). The health task
 *  prints the measured high-water marks at runtime so these can be tuned with
 *  evidence rather than guesswork -- see docs/measurements.md.
 * ========================================================================== */
#define TASK_PRIO_SENSOR                3
#define TASK_PRIO_PROCESSING            2
#define TASK_PRIO_COMM                  2
#define TASK_PRIO_LOGGING               1
#define TASK_PRIO_HEALTH                1

#define TASK_STACK_SENSOR               256
#define TASK_STACK_PROCESSING           192
#define TASK_STACK_COMM                 256
#define TASK_STACK_LOGGING              192
#define TASK_STACK_HEALTH               192

/* --- Periods -------------------------------------------------------------- */
#define SENSOR_PERIOD_MS_DEFAULT        1000U
#define SENSOR_PERIOD_MS_MIN            100U
#define SENSOR_PERIOD_MS_MAX            60000U
#define HEALTH_PERIOD_MS                5000U

/* --- Queues --------------------------------------------------------------- */
#define SENSOR_QUEUE_LENGTH             8U
#define TELEMETRY_QUEUE_LENGTH          4U
#define LOG_QUEUE_LENGTH                12U
/* Sized to hold a complete telemetry line, which the comm task mirrors to the
 * console. Worst case ("S=SENSOR_FAULT" with saturated 32-bit counters) is
 * 85 characters; 88 leaves headroom without another queue-entry growth. */
#define LOG_LINE_MAX                    88U     /* bytes incl. NUL           */

/* --- Processing ----------------------------------------------------------- */
#define PROCESSING_WINDOW_LEN           8U      /* moving-average depth       */

/* Threshold bands used by processing.c to derive the environment status. */
#define THRESH_TEMP_HIGH_C              30.0f
#define THRESH_TEMP_LOW_C               5.0f
#define THRESH_HUMIDITY_HIGH_PCT        70.0f
#define THRESH_HUMIDITY_LOW_PCT         20.0f
#define THRESH_GAS_POOR_OHM             20000.0f    /* low resistance = high VOC */

/* Gas resistance in clean air, used to normalise the IAQ approximation.
 * Calibrate this per sensor: run the node in fresh air for ~20 min and read
 * the stabilised gas value from the log. */
#define IAQ_GAS_BASELINE_OHM            50000.0f
#define IAQ_HUMIDITY_OPTIMAL_PCT        40.0f
#define IAQ_HUMIDITY_WEIGHT             0.25f
#define IAQ_GAS_WEIGHT                  0.75f

/* --- UART DMA buffers ----------------------------------------------------- */
#define UART_TX_BUFFER_SIZE             160U
#define UART_RX_BUFFER_SIZE             128U    /* circular DMA landing zone */
#define UART_CMD_MAX_LEN                48U

#endif /* APP_CONFIG_H */
