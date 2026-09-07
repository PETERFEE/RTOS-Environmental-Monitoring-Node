/**
 * @file    main.h
 * @brief   Board bring-up shared between main.c, the HAL MSP and the ISRs.
 */
#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"
#include "app_config.h"

/* Peripheral handles owned by main.c, used by the drivers and ISRs. */
extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart_log;
extern UART_HandleTypeDef huart_comm;
/* DMA handles are named by peripheral rather than by role, because
 * SWAP_UART_ROLES changes which USART is the console and which is the ESP32
 * link, but never which DMA channel serves which USART. */
extern DMA_HandleTypeDef  hdma_usart1_tx;   /**< DMA1_Channel4 */
extern DMA_HandleTypeDef  hdma_usart1_rx;   /**< DMA1_Channel5 */
extern DMA_HandleTypeDef  hdma_usart2_tx;   /**< DMA1_Channel7 */
extern DMA_HandleTypeDef  hdma_usart2_rx;   /**< DMA1_Channel6 */

void SystemClock_Config(void);
void Error_Handler(void);

/** Blocking, interrupt-safe write straight to the log UART.
 *  Used before the scheduler starts and from fault handlers, where the
 *  RTOS logging path is not available. */
void board_uart_write_blocking(const char *data, uint16_t len);

/** Blocking "print a line" helper built on the above. Used during bring-up and
 *  by any code that must report before the logging task exists. */
void board_log_line(const char *s);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_H */
