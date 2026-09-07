/**
 * @file    uart_dma.h
 * @brief   DMA-driven UART links shared by the logging and communication tasks.
 *
 * Two links exist:
 *   UART_LINK_LOG   -- console for the PC (ST-LINK virtual COM port)
 *   UART_LINK_COMM  -- telemetry / command channel to the ESP32
 *
 * Transmit
 *   The calling task hands the buffer to the DMA controller and blocks on a
 *   task notification until the transfer-complete interrupt fires. The CPU is
 *   free for other tasks for the whole ~5 ms a 60-byte line takes at 115200,
 *   instead of spinning in HAL_UART_Transmit().
 *
 * Receive
 *   A circular DMA buffer runs continuously, and the USART's IDLE-line
 *   interrupt fires whenever the peer stops transmitting. That combination
 *   handles variable-length messages without knowing their length in advance
 *   and without an interrupt per byte. New bytes are copied into a stream
 *   buffer and the communication task is notified.
 *
 * Set -DUSE_UART_DMA=OFF to fall back to blocking transmit and no reception,
 * which is the phase-4 behaviour; the API is unchanged either way.
 */
#ifndef UART_DMA_H
#define UART_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

typedef enum
{
    UART_LINK_LOG = 0,
    UART_LINK_COMM,
    UART_LINK_COUNT
} uart_link_t;

/** Create the per-link mutexes and stream buffers, and arm circular reception. */
bool uart_dma_init(void);

/** Send @p len bytes on @p link. Serialised per link; any task may call it. */
bool uart_dma_send(uart_link_t link, const char *data, uint16_t len, uint32_t timeout_ms);

/** Convenience wrapper for NUL-terminated strings. */
bool uart_dma_send_str(uart_link_t link, const char *str, uint32_t timeout_ms);

/**
 * Pull up to @p max_len received bytes out of @p link's stream buffer.
 * Non-blocking: returns 0 when nothing is pending.
 */
size_t uart_dma_read(uart_link_t link, uint8_t *out, size_t max_len);

/** Task to notify when bytes arrive. Set once, before reception is armed. */
void uart_dma_set_rx_listener(TaskHandle_t task, uint32_t notify_bits);

uint32_t uart_dma_tx_error_count(void);
uint32_t uart_dma_rx_overflow_count(void);

#endif /* UART_DMA_H */
