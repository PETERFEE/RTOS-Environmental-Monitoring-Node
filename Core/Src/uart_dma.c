/**
 * @file    uart_dma.c
 * @brief   DMA transmit + circular DMA receive with IDLE-line framing.
 *
 * The receive side is the interesting half. A plain interrupt-per-byte driver
 * costs an exception entry for every character (about 3.5 us of overhead at
 * 115200 baud, ~4 % of the CPU on a continuous stream) and still needs the
 * application to guess where a message ends. Running the DMA in circular mode
 * and letting the USART's IDLE-line detector tell us when the peer stopped
 * talking removes both problems: zero interrupts during a burst, and exactly
 * one at the end of it, whatever length it was.
 */
#include "uart_dma.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"

#include <string.h>

#include "main.h"

typedef struct
{
    UART_HandleTypeDef *huart;
    SemaphoreHandle_t   tx_mutex;
    volatile TaskHandle_t tx_waiter;

    uint8_t            rx_dma_buf[UART_RX_BUFFER_SIZE];
    volatile uint16_t  rx_read_idx;
    StreamBufferHandle_t rx_stream;
} uart_link_ctx_t;

static uart_link_ctx_t s_links[UART_LINK_COUNT];

static TaskHandle_t s_rx_listener      = NULL;
static uint32_t     s_rx_notify_bits   = 0U;

static volatile uint32_t s_tx_errors    = 0U;
static volatile uint32_t s_rx_overflows = 0U;

uint32_t uart_dma_tx_error_count(void)     { return s_tx_errors; }
uint32_t uart_dma_rx_overflow_count(void)  { return s_rx_overflows; }

void uart_dma_set_rx_listener(TaskHandle_t task, uint32_t notify_bits)
{
    s_rx_listener    = task;
    s_rx_notify_bits = notify_bits;
}

static uart_link_ctx_t *link_for(UART_HandleTypeDef *huart)
{
    for (int i = 0; i < UART_LINK_COUNT; i++)
    {
        if (s_links[i].huart == huart)
        {
            return &s_links[i];
        }
    }
    return NULL;
}

/* ==========================================================================
 *  Init
 * ======================================================================= */
bool uart_dma_init(void)
{
    s_links[UART_LINK_LOG].huart  = &huart_log;
    s_links[UART_LINK_COMM].huart = &huart_comm;

    for (int i = 0; i < UART_LINK_COUNT; i++)
    {
        uart_link_ctx_t *const ctx = &s_links[i];

        ctx->tx_mutex = xSemaphoreCreateMutex();
        if (ctx->tx_mutex == NULL)
        {
            return false;
        }

        ctx->tx_waiter  = NULL;
        ctx->rx_read_idx = 0U;

#if APP_USE_UART_DMA
        /* Trigger level 1: wake the reader as soon as anything is available. */
        ctx->rx_stream = xStreamBufferCreate(UART_RX_BUFFER_SIZE, 1U);
        if (ctx->rx_stream == NULL)
        {
            return false;
        }

        /* Circular reception runs forever; it is never restarted per message. */
        if (HAL_UART_Receive_DMA(ctx->huart, ctx->rx_dma_buf, UART_RX_BUFFER_SIZE) != HAL_OK)
        {
            return false;
        }

        /* IDLE fires one frame time after the peer stops sending -- the event
         * that says "a complete message just arrived", regardless of length. */
        __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);
        __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);
#else
        ctx->rx_stream = NULL;
#endif
    }

    return true;
}

/* ==========================================================================
 *  Transmit
 * ======================================================================= */
bool uart_dma_send(uart_link_t link, const char *data, uint16_t len, uint32_t timeout_ms)
{
    if (link >= UART_LINK_COUNT || data == NULL || len == 0U)
    {
        return false;
    }

    uart_link_ctx_t *const ctx = &s_links[link];

    /* Before the scheduler exists there is nothing to block on and no mutex to
     * take, so fall back to a plain blocking transfer. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED || ctx->tx_mutex == NULL)
    {
        return HAL_UART_Transmit(ctx->huart, (uint8_t *)data, len, timeout_ms) == HAL_OK;
    }

    /* One writer at a time per link: two tasks interleaving bytes into the same
     * data register would produce unparseable output on the wire. */
    if (xSemaphoreTake(ctx->tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        s_tx_errors++;
        return false;
    }

    bool ok;

#if APP_USE_UART_DMA
    /* Clear any stale notification before arming, so a notification left over
     * from a previous timed-out transfer cannot satisfy this wait. */
    (void)ulTaskNotifyTake(pdTRUE, 0U);
    ctx->tx_waiter = xTaskGetCurrentTaskHandle();

    if (HAL_UART_Transmit_DMA(ctx->huart, (uint8_t *)data, len) != HAL_OK)
    {
        ctx->tx_waiter = NULL;
        s_tx_errors++;
        xSemaphoreGive(ctx->tx_mutex);
        return false;
    }

    /* Blocked here, this task costs nothing while the DMA controller moves the
     * bytes. Everything else in the system keeps running. */
    ok = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) > 0U);
    ctx->tx_waiter = NULL;

    if (!ok)
    {
        /* Timed out: abort so the peripheral is not left mid-transfer. */
        (void)HAL_UART_AbortTransmit(ctx->huart);
        s_tx_errors++;
    }
#else
    ok = (HAL_UART_Transmit(ctx->huart, (uint8_t *)data, len, timeout_ms) == HAL_OK);
    if (!ok)
    {
        s_tx_errors++;
    }
#endif

    xSemaphoreGive(ctx->tx_mutex);
    return ok;
}

bool uart_dma_send_str(uart_link_t link, const char *str, uint32_t timeout_ms)
{
    if (str == NULL)
    {
        return false;
    }
    return uart_dma_send(link, str, (uint16_t)strnlen(str, 1024U), timeout_ms);
}

/* ==========================================================================
 *  Receive
 * ======================================================================= */
size_t uart_dma_read(uart_link_t link, uint8_t *out, size_t max_len)
{
    if (link >= UART_LINK_COUNT || out == NULL || max_len == 0U)
    {
        return 0U;
    }

    StreamBufferHandle_t sb = s_links[link].rx_stream;
    if (sb == NULL)
    {
        return 0U;
    }

    return xStreamBufferReceive(sb, out, max_len, 0U);
}

#if APP_USE_UART_DMA
/**
 * Move everything the DMA has landed since last time into the stream buffer.
 *
 * Called from the IDLE, half-transfer and transfer-complete interrupts. The
 * DMA's remaining-count register is the write pointer; rx_read_idx is ours.
 */
static void rx_drain_from_isr(uart_link_ctx_t *ctx, BaseType_t *woken)
{
    if (ctx == NULL || ctx->rx_stream == NULL)
    {
        return;
    }

    const uint16_t remaining = (uint16_t)__HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);
    const uint16_t write_idx = (uint16_t)(UART_RX_BUFFER_SIZE - remaining);
    uint16_t       read_idx  = ctx->rx_read_idx;

    if (write_idx == read_idx)
    {
        return;                                  /* nothing new */
    }

    size_t sent;

    if (write_idx > read_idx)
    {
        const size_t len = (size_t)(write_idx - read_idx);
        sent = xStreamBufferSendFromISR(ctx->rx_stream, &ctx->rx_dma_buf[read_idx], len, woken);
        if (sent < len) { s_rx_overflows++; }
    }
    else
    {
        /* The write pointer wrapped: copy the tail, then the head. */
        const size_t tail = (size_t)(UART_RX_BUFFER_SIZE - read_idx);
        sent = xStreamBufferSendFromISR(ctx->rx_stream, &ctx->rx_dma_buf[read_idx], tail, woken);
        if (sent < tail) { s_rx_overflows++; }

        if (write_idx > 0U)
        {
            sent = xStreamBufferSendFromISR(ctx->rx_stream, &ctx->rx_dma_buf[0], write_idx, woken);
            if (sent < write_idx) { s_rx_overflows++; }
        }
    }

    ctx->rx_read_idx = write_idx;

    if (s_rx_listener != NULL)
    {
        (void)xTaskNotifyFromISR(s_rx_listener, s_rx_notify_bits, eSetBits, woken);
    }
}

/** Called from USARTx_IRQHandler when the IDLE-line flag is set. */
void uart_dma_idle_isr(UART_HandleTypeDef *huart)
{
    uart_link_ctx_t *const ctx = link_for(huart);
    if (ctx == NULL)
    {
        return;
    }

    BaseType_t woken = pdFALSE;
    rx_drain_from_isr(ctx, &woken);
    portYIELD_FROM_ISR(woken);
}
#endif /* APP_USE_UART_DMA */

/* ==========================================================================
 *  HAL callbacks
 * ======================================================================= */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    uart_link_ctx_t *const ctx = link_for(huart);
    if (ctx == NULL || ctx->tx_waiter == NULL)
    {
        return;
    }

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->tx_waiter, &woken);
    portYIELD_FROM_ISR(woken);
}

#if APP_USE_UART_DMA
/* Half- and full-buffer callbacks keep a fast continuous stream flowing even if
 * the peer never goes idle long enough to raise IDLE. */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t woken = pdFALSE;
    rx_drain_from_isr(link_for(huart), &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t woken = pdFALSE;
    rx_drain_from_isr(link_for(huart), &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    /* A framing/noise/overrun error leaves the DMA armed but the error flags
     * latched. Clear them and keep going -- a garbled byte must not kill the
     * link permanently. */
    uart_link_ctx_t *const ctx = link_for(huart);
    if (ctx == NULL)
    {
        return;
    }

    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);

    if (huart->RxState != HAL_UART_STATE_BUSY_RX)
    {
        ctx->rx_read_idx = 0U;
        (void)HAL_UART_Receive_DMA(huart, ctx->rx_dma_buf, UART_RX_BUFFER_SIZE);
    }
}
#endif /* APP_USE_UART_DMA */
