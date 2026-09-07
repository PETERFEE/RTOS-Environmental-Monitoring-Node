/**
 * @file    logging_task.h
 * @brief   Task 4 -- asynchronous, non-blocking debug logging.
 *
 * Producers copy a short line into a queue and return immediately; only the
 * logging task ever touches the UART. That keeps a 5 ms 115200-baud transmit
 * out of the sensor task's timing budget, and means logging can never introduce
 * a priority inversion on the console.
 *
 * Nothing here uses printf: the formatting helpers below cover what the firmware
 * actually needs, without linking a 7 KB vararg formatter into a 128 KB part.
 */
#ifndef LOGGING_TASK_H
#define LOGGING_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "app_config.h"

typedef struct
{
    uint32_t tick;                  /**< when the event happened, not when printed */
    char     text[LOG_LINE_MAX];
} log_msg_t;

void logging_task(void *arg);

/** Queue a line. Never blocks; drops (and counts) if the queue is full. */
void log_line(const char *text);

/** "<key>=<value>" */
void log_kv_u32(const char *key, uint32_t value);

/** "<key>=<value>" with a fixed number of decimals. */
void log_kv_f(const char *key, float value, uint8_t decimals);

/** ISR-safe variant. Sets *higher_prio_woken if a context switch is due. */
void log_line_from_isr(const char *text, BaseType_t *higher_prio_woken);

uint32_t log_dropped_count(void);

#endif /* LOGGING_TASK_H */
