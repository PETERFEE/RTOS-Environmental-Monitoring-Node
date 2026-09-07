/**
 * @file    logging_task.c
 * @brief   Task 4 -- drains the log queue onto the console UART.
 */
#include "logging_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <string.h>

#include "app_tasks.h"
#include "main.h"
#include "telemetry.h"
#include "uart_dma.h"

static volatile uint32_t s_dropped = 0U;

uint32_t log_dropped_count(void) { return s_dropped; }

/* --------------------------------------------------------------------------
 *  Producers
 * ----------------------------------------------------------------------- */
static void enqueue(const char *text, uint32_t tick)
{
    if (text == NULL || g_log_queue == NULL)
    {
        return;
    }

    log_msg_t msg;
    msg.tick = tick;
    strncpy(msg.text, text, LOG_LINE_MAX - 1U);
    msg.text[LOG_LINE_MAX - 1U] = '\0';

    /* Zero timeout on purpose: logging must never delay the task doing the real
     * work. A lost log line is visible in the drop counter. */
    if (xQueueSend(g_log_queue, &msg, 0U) != pdPASS)
    {
        s_dropped++;
    }
}

void log_line(const char *text)
{
    enqueue(text, xTaskGetTickCount());
}

void log_line_from_isr(const char *text, BaseType_t *higher_prio_woken)
{
    if (text == NULL || g_log_queue == NULL)
    {
        return;
    }

    log_msg_t msg;
    msg.tick = xTaskGetTickCountFromISR();
    strncpy(msg.text, text, LOG_LINE_MAX - 1U);
    msg.text[LOG_LINE_MAX - 1U] = '\0';

    if (xQueueSendFromISR(g_log_queue, &msg, higher_prio_woken) != pdPASS)
    {
        s_dropped++;
    }
}

void log_kv_u32(const char *key, uint32_t value)
{
    char line[LOG_LINE_MAX];
    char num[12];

    (void)telemetry_ftoa((float)value, 0U, num, sizeof(num));

    line[0] = '\0';
    strncat(line, key, LOG_LINE_MAX - 2U);
    strncat(line, "=", LOG_LINE_MAX - strlen(line) - 1U);
    strncat(line, num, LOG_LINE_MAX - strlen(line) - 1U);

    log_line(line);
}

void log_kv_f(const char *key, float value, uint8_t decimals)
{
    char line[LOG_LINE_MAX];
    char num[24];

    (void)telemetry_ftoa(value, decimals, num, sizeof(num));

    line[0] = '\0';
    strncat(line, key, LOG_LINE_MAX - 2U);
    strncat(line, "=", LOG_LINE_MAX - strlen(line) - 1U);
    strncat(line, num, LOG_LINE_MAX - strlen(line) - 1U);

    log_line(line);
}

/* --------------------------------------------------------------------------
 *  Consumer
 * ----------------------------------------------------------------------- */
void logging_task(void *arg)
{
    (void)arg;

    log_line("logging task started");

    for (;;)
    {
        log_msg_t msg;
        if (xQueueReceive(g_log_queue, &msg, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        /* "[<tick> ms] <text>" -- the timestamp is the enqueue time, so the log
         * shows when the event happened rather than when the UART got round to
         * it. That distinction matters as soon as the queue backs up. */
        char out[LOG_LINE_MAX + 24U];
        char tick[12];

        (void)telemetry_ftoa((float)msg.tick, 0U, tick, sizeof(tick));

        out[0] = '\0';
        strcat(out, "[");
        strcat(out, tick);
        strcat(out, " ms] ");
        strncat(out, msg.text, LOG_LINE_MAX - 1U);
        strcat(out, "\r\n");

        (void)uart_dma_send(UART_LINK_LOG, out, (uint16_t)strlen(out), 100U);
    }
}
