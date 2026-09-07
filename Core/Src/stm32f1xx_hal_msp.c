/**
 * @file    stm32f1xx_hal_msp.c
 * @brief   MCU Support Package: the board-specific half of peripheral init.
 *
 * The HAL calls into these hooks from HAL_I2C_Init() / HAL_UART_Init(), which
 * keeps clock gating, pin muxing, DMA wiring and NVIC setup out of main.c.
 */
#include "main.h"

void HAL_MspInit(void)
{
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* 4 preemption-priority bits, no sub-priority. FreeRTOS assumes this
     * grouping when it validates configMAX_SYSCALL_INTERRUPT_PRIORITY. */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1)
    {
        return;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB6 = I2C1_SCL, PB7 = I2C1_SDA.
     * Alternate-function OPEN DRAIN is mandatory on I2C: the bus is wired-AND
     * and any push-pull driver would fight the pull-ups and the slave's ACK. */
    GPIO_InitTypeDef g = {0};
    g.Pin   = BME680_I2C_SCL_PIN | BME680_I2C_SDA_PIN;
    g.Mode  = GPIO_MODE_AF_OD;
    g.Pull  = GPIO_NOPULL;      /* rely on the breakout's 4k7 pull-ups */
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);

    __HAL_RCC_I2C1_CLK_ENABLE();
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1)
    {
        return;
    }
    __HAL_RCC_I2C1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOB, BME680_I2C_SCL_PIN | BME680_I2C_SDA_PIN);
}

#if APP_USE_UART_DMA
/**
 * Configure one DMA channel for a USART.
 *
 * TX is normal mode: one transfer per message, complete interrupt wakes the
 * sending task. RX is CIRCULAR: the controller wraps forever without CPU
 * involvement, and the IDLE-line interrupt is what frames messages inside it.
 */
static void uart_dma_channel_init(DMA_HandleTypeDef *hdma,
                                  DMA_Channel_TypeDef *channel,
                                  uint32_t direction,
                                  uint32_t mode)
{
    hdma->Instance                 = channel;
    hdma->Init.Direction           = direction;
    hdma->Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma->Init.MemInc              = DMA_MINC_ENABLE;
    hdma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma->Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma->Init.Mode                = mode;
    hdma->Init.Priority            = DMA_PRIORITY_LOW;

    if (HAL_DMA_Init(hdma) != HAL_OK)
    {
        Error_Handler();
    }
}
#endif /* APP_USE_UART_DMA */

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    if (huart->Instance == USART1)
    {
        __HAL_RCC_USART1_CLK_ENABLE();

        g.Pin   = GPIO_PIN_9;                 /* PA9  = USART1_TX */
        g.Mode  = GPIO_MODE_AF_PP;
        g.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &g);

        g.Pin  = GPIO_PIN_10;                 /* PA10 = USART1_RX */
        g.Mode = GPIO_MODE_INPUT;
        g.Pull = GPIO_PULLUP;                 /* idle-high line when unplugged */
        HAL_GPIO_Init(GPIOA, &g);

#if APP_USE_UART_DMA
        uart_dma_channel_init(&hdma_usart1_tx, DMA1_Channel4,
                              DMA_MEMORY_TO_PERIPH, DMA_NORMAL);
        __HAL_LINKDMA(huart, hdmatx, hdma_usart1_tx);

        uart_dma_channel_init(&hdma_usart1_rx, DMA1_Channel5,
                              DMA_PERIPH_TO_MEMORY, DMA_CIRCULAR);
        __HAL_LINKDMA(huart, hdmarx, hdma_usart1_rx);
#endif

        HAL_NVIC_SetPriority(USART1_IRQn, APP_IRQ_PRIO_USART, 0U);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
    else if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_ENABLE();

        g.Pin   = GPIO_PIN_2;                 /* PA2 = USART2_TX */
        g.Mode  = GPIO_MODE_AF_PP;
        g.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &g);

        g.Pin  = GPIO_PIN_3;                  /* PA3 = USART2_RX */
        g.Mode = GPIO_MODE_INPUT;
        g.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOA, &g);

#if APP_USE_UART_DMA
        uart_dma_channel_init(&hdma_usart2_tx, DMA1_Channel7,
                              DMA_MEMORY_TO_PERIPH, DMA_NORMAL);
        __HAL_LINKDMA(huart, hdmatx, hdma_usart2_tx);

        uart_dma_channel_init(&hdma_usart2_rx, DMA1_Channel6,
                              DMA_PERIPH_TO_MEMORY, DMA_CIRCULAR);
        __HAL_LINKDMA(huart, hdmarx, hdma_usart2_rx);
#endif

        HAL_NVIC_SetPriority(USART2_IRQn, APP_IRQ_PRIO_USART, 0U);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        __HAL_RCC_USART1_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
        HAL_NVIC_DisableIRQ(USART1_IRQn);
    }
    else if (huart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
        HAL_NVIC_DisableIRQ(USART2_IRQn);
    }

    if (huart->hdmatx != NULL) { (void)HAL_DMA_DeInit(huart->hdmatx); }
    if (huart->hdmarx != NULL) { (void)HAL_DMA_DeInit(huart->hdmarx); }
}
