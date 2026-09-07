/**
 * @file    main.c
 * @brief   Board bring-up only: clocks, peripherals, then hand over.
 *
 * The scheduler owns the CPU: main() brings the hardware up, creates the RTOS
 * objects and calls vTaskStartScheduler(), then never returns. All application
 * behaviour lives in the task modules.
 */
#include "main.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_tasks.h"
#include "bme680_driver.h"
#include "perf.h"
#include "telemetry.h"

/* --------------------------------------------------------------------------
 *  Peripheral handles (declared extern in main.h)
 * ----------------------------------------------------------------------- */
I2C_HandleTypeDef  hi2c1;
UART_HandleTypeDef huart_log;
UART_HandleTypeDef huart_comm;

DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart2_tx;
DMA_HandleTypeDef hdma_usart2_rx;

static void dma_init(void);
static void gpio_init(void);
static void i2c1_init(void);
static void uart_init(void);

/* ======================================================================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    perf_init();

    gpio_init();
    dma_init();
    uart_init();
    i2c1_init();

    board_log_line("\r\n=== rtos-iot-sensor-node v1.0 ===");

    const bme680_status_t st = bme680_driver_init();
    if (st != BME680_OK)
    {
        board_log_line("BME680 init FAILED: ");
        board_log_line(bme680_status_name(st));
        board_log_line("  -> check wiring: PB6=SCL PB7=SDA, 3V3, GND, pull-ups");
        Error_Handler();
    }

    {
        /* Two nibbles of hex is not worth linking printf for. */
        static const char k_hex[] = "0123456789ABCDEF";
        const uint8_t a = bme680_driver_address();
        char msg[32] = "BME680 found at 0x";
        const char h[3] = { k_hex[(a >> 4) & 0x0FU], k_hex[a & 0x0FU], '\0' };
        strcat(msg, h);
        board_log_line(msg);
    }

    if (!app_tasks_create())
    {
        board_log_line("task creation FAILED (out of heap)");
        Error_Handler();
    }

    board_log_line("starting scheduler");
    vTaskStartScheduler();

    /* Only reached if the kernel could not start the idle task. */
    board_log_line("scheduler did not start");
    Error_Handler();

    for (;;) { }
}

/* ========================================================================
 *  Clock tree
 *
 *  SYSCLK 64 MHz from the internal HSI (8 MHz / 2 * 16).
 *
 *  Deliberately NOT the 72 MHz HSE configuration: on a Nucleo-64 the 8 MHz
 *  HSE reference comes from the ST-LINK's MCO through solder bridges SB16/SB18,
 *  so an HSE build silently fails to start on boards where those are open.
 *  HSI costs 8 MHz of headroom and buys "works on any board". See
 *  docs/wiring.md for the HSE variant of this function.
 * ===================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;   /* 8 MHz / 2 = 4 MHz  */
    osc.PLL.PLLMUL          = RCC_PLL_MUL16;            /* 4 MHz * 16 = 64 MHz */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        Error_Handler();
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;               /* HCLK  = 64 MHz     */
    clk.APB1CLKDivider = RCC_HCLK_DIV2;                 /* PCLK1 = 32 MHz max */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;                 /* PCLK2 = 64 MHz     */

    /* 64 MHz needs 2 flash wait states on the F1. */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ========================================================================
 *  Peripheral init
 * ===================================================================== */
static void gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    /* LD2 on PA5 */
    HAL_GPIO_WritePin(APP_LED_PORT, APP_LED_PIN, GPIO_PIN_RESET);
    g.Pin   = APP_LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(APP_LED_PORT, &g);

    /* B1 on PC13. The Nucleo wires the button to ground with an external
     * pull-up fitted, so the line idles high and a press pulls it low:
     * falling-edge trigger, internal pull-up as a belt-and-braces default for
     * anyone wiring their own button to a bare GPIO. */
    g.Pin  = APP_BUTTON_PIN;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(APP_BUTTON_PORT, &g);

    /* Priority 7 is numerically above configMAX_SYSCALL_INTERRUPT_PRIORITY (5),
     * which is what makes it legal for this ISR to call ...FromISR() APIs. */
    HAL_NVIC_SetPriority(APP_BUTTON_IRQn, APP_IRQ_PRIO_BUTTON, 0U);
    HAL_NVIC_EnableIRQ(APP_BUTTON_IRQn);
}

/**
 * The DMA controller's clock must be running before HAL_UART_Init() reaches
 * HAL_UART_MspInit(), which is where the channels are configured and linked.
 */
static void dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* USART1: TX = DMA1_Channel4, RX = DMA1_Channel5
     * USART2: RX = DMA1_Channel6, TX = DMA1_Channel7   (fixed by the F1 map) */
    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, APP_IRQ_PRIO_DMA, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, APP_IRQ_PRIO_DMA, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, APP_IRQ_PRIO_DMA, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel7_IRQn, APP_IRQ_PRIO_DMA, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);
}

static void i2c1_init(void)
{
    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = BME680_I2C_SPEED_HZ;
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2     = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
}

static void uart_config(UART_HandleTypeDef *h, USART_TypeDef *instance)
{
    h->Instance          = instance;
    h->Init.BaudRate     = APP_UART_BAUDRATE;
    h->Init.WordLength   = UART_WORDLENGTH_8B;
    h->Init.StopBits     = UART_STOPBITS_1;
    h->Init.Parity       = UART_PARITY_NONE;
    h->Init.Mode         = UART_MODE_TX_RX;
    h->Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    h->Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(h) != HAL_OK)
    {
        Error_Handler();
    }
}

static void uart_init(void)
{
    uart_config(&huart_log,  APP_LOG_USART);
    uart_config(&huart_comm, APP_COMM_USART);
}

/* ========================================================================
 *  Low-level output + fault handling
 * ===================================================================== */
void board_uart_write_blocking(const char *data, uint16_t len)
{
    if (data == NULL || len == 0U)
    {
        return;
    }
    (void)HAL_UART_Transmit(&huart_log, (uint8_t *)data, len, 200U);
}

void board_log_line(const char *s)
{
    board_uart_write_blocking(s, (uint16_t)strlen(s));
    board_uart_write_blocking("\r\n", 2U);
}

void Error_Handler(void)
{
    __disable_irq();

    board_uart_write_blocking("\r\n!! Error_Handler: halted\r\n", 27U);

    /* Fast blink so a halted board is obvious without a terminal attached. */
    for (;;)
    {
        APP_LED_PORT->ODR ^= APP_LED_PIN;
        for (volatile uint32_t i = 0U; i < 400000U; i++) { __NOP(); }
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file; (void)line;
    Error_Handler();
}
#endif
