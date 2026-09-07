/**
 * @file    comm_task.c
 * @brief   Task 3 -- telemetry out, commands in.
 *
 * Outbound: each processed result becomes the compact key=value line the ESP32
 * gateway parses and republishes over MQTT.
 *
 * Inbound: bytes arrive by circular DMA and are handed over by the IDLE-line
 * ISR. This task reassembles them into lines, parses them with cmd_parser.c and
 * answers on the same link the request came in on -- so the ESP32 and a PC
 * terminal can both drive the node, and the Python tests can talk to it over the
 * ST-LINK virtual COM port without any extra hardware.
 */
#include "comm_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <string.h>

#include "app_tasks.h"
#include "cmd_parser.h"
#include "health_task.h"
#include "logging_task.h"
#include "main.h"
#include "processing_task.h"
#include "sensor_task.h"
#include "telemetry.h"
#include "uart_dma.h"

static volatile uint32_t s_tx_count  = 0U;
static volatile uint32_t s_tx_errors = 0U;
static volatile uint32_t s_cmd_count = 0U;

/* One reassembly buffer per link: a partial line on the console must not be
 * mixed with a partial line from the ESP32. */
static char   s_line[UART_LINK_COUNT][UART_CMD_MAX_LEN];
static size_t s_line_len[UART_LINK_COUNT];
static bool   s_line_overflow[UART_LINK_COUNT];

uint32_t comm_task_tx_count(void)       { return s_tx_count; }
uint32_t comm_task_tx_error_count(void) { return s_tx_errors; }
uint32_t comm_task_command_count(void)  { return s_cmd_count; }

void comm_task_notify_telemetry(void)
{
    if (g_comm_task != NULL)
    {
        (void)xTaskNotify(g_comm_task, COMM_EVT_TELEMETRY, eSetBits);
    }
}

static void reply(uart_link_t link, const char *text)
{
    (void)uart_dma_send_str(link, text, 100U);
}

/* --------------------------------------------------------------------------
 *  Command execution
 * ----------------------------------------------------------------------- */
static void execute(uart_link_t link, const cmd_t *cmd)
{
    char out[UART_TX_BUFFER_SIZE];

    switch (cmd->type)
    {
        case CMD_STATUS:
        {
            processed_data_t last;
            const bool have = processing_task_get_last(&last);

            const size_t len = telemetry_format_status(
                have ? &last : NULL,
                sensor_task_get_period(),
                (uint32_t)xPortGetFreeHeapSize(),
                (uint32_t)xTaskGetTickCount(),
                sensor_task_sample_count(),
                sensor_task_error_count(),
                out, sizeof(out));

            if (len > 0U)
            {
                (void)uart_dma_send(link, out, (uint16_t)len, 100U);
            }
            break;
        }

        case CMD_SAMPLE_NOW:
            sensor_task_trigger();
            reply(link, "OK SAMPLE_NOW\n");
            break;

        case CMD_SET_PERIOD:
        {
            sensor_task_set_period(cmd->arg);

            char num[12];
            (void)telemetry_ftoa((float)sensor_task_get_period(), 0U, num, sizeof(num));
            out[0] = '\0';
            strcat(out, "OK PERIOD=");
            strcat(out, num);
            strcat(out, "\n");
            reply(link, out);
            break;
        }

        case CMD_GET_PERIOD:
        {
            char num[12];
            (void)telemetry_ftoa((float)sensor_task_get_period(), 0U, num, sizeof(num));
            out[0] = '\0';
            strcat(out, "PERIOD=");
            strcat(out, num);
            strcat(out, "\n");
            reply(link, out);
            break;
        }

        case CMD_RESET_STATS:
            sensor_task_reset_stats();
            processing_task_reset_stats();
            reply(link, "OK RESET_STATS\n");
            break;

        case CMD_PING:
            reply(link, "PONG\n");
            break;

        case CMD_HELP:
            reply(link,
                  "COMMANDS: STATUS SAMPLE_NOW SET_PERIOD=<ms> GET_PERIOD "
                  "RESET_STATS PING HELP\n");
            break;

        case CMD_BAD_ARGUMENT:
            reply(link, "ERR BAD_ARGUMENT\n");
            break;

        case CMD_NONE:
            break;                       /* blank line: say nothing */

        case CMD_UNKNOWN:
        default:
            reply(link, "ERR UNKNOWN_COMMAND\n");
            break;
    }
}

static void handle_line(uart_link_t link, char *line)
{
    cmd_t cmd;
    (void)cmd_parse(line, &cmd);

    if (cmd.type == CMD_NONE)
    {
        return;
    }

    s_cmd_count++;

    char note[LOG_LINE_MAX];
    note[0] = '\0';
    strcat(note, "cmd: ");
    strncat(note, cmd_name(cmd.type), LOG_LINE_MAX - 8U);
    log_line(note);

    execute(link, &cmd);
}

/* --------------------------------------------------------------------------
 *  Byte stream -> lines
 * ----------------------------------------------------------------------- */
static void feed(uart_link_t link, const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len; i++)
    {
        const char c = (char)data[i];

        if (c == '\n' || c == '\r')
        {
            if (s_line_overflow[link])
            {
                /* The line was longer than the buffer: report it rather than
                 * silently acting on a truncated command. */
                reply(link, "ERR LINE_TOO_LONG\n");
            }
            else if (s_line_len[link] > 0U)
            {
                s_line[link][s_line_len[link]] = '\0';
                handle_line(link, s_line[link]);
            }

            s_line_len[link]      = 0U;
            s_line_overflow[link] = false;
            continue;
        }

        if (s_line_len[link] < (UART_CMD_MAX_LEN - 1U))
        {
            s_line[link][s_line_len[link]++] = c;
        }
        else
        {
            s_line_overflow[link] = true;
        }
    }
}

static void drain_rx(void)
{
    for (int link = 0; link < UART_LINK_COUNT; link++)
    {
        uint8_t chunk[32];
        size_t  n;

        while ((n = uart_dma_read((uart_link_t)link, chunk, sizeof(chunk))) > 0U)
        {
            feed((uart_link_t)link, chunk, n);
        }
    }
}

static void drain_telemetry(void)
{
    processed_data_t data;

    while (xQueueReceive(g_telemetry_queue, &data, 0U) == pdPASS)
    {
        char         line[UART_TX_BUFFER_SIZE];
        const size_t len = telemetry_format_line(&data, line, sizeof(line));

        if (len == 0U)
        {
            s_tx_errors++;
            continue;
        }

        if (uart_dma_send(UART_LINK_COMM, line, (uint16_t)len, 100U))
        {
            s_tx_count++;
        }
        else
        {
            s_tx_errors++;
            log_line("comm: telemetry TX failed");
        }

        /* Mirror the reading to the PC console as well.
         *
         * Without this the node is invisible unless an ESP32 is attached: the
         * console would show health reports but never a reading, which looks
         * exactly like a broken sensor. Being observable with nothing but the
         * ST-LINK cable plugged in matters more than the handful of bytes.
         *
         * The trailing '\n' is dropped because the logging task supplies its
         * own line ending. `line` is not used again after this. */
        line[len - 1U] = '\0';
        log_line(line);
    }
}

/* --------------------------------------------------------------------------
 *  Task body
 * ----------------------------------------------------------------------- */
void comm_task(void *arg)
{
    (void)arg;

    memset(s_line_len,      0, sizeof(s_line_len));
    memset(s_line_overflow, 0, sizeof(s_line_overflow));

    /* Route received-byte notifications to this task. */
    uart_dma_set_rx_listener(xTaskGetCurrentTaskHandle(), COMM_EVT_RX);

    (void)uart_dma_send_str(UART_LINK_COMM, "HELLO FROM STM32\n", 100U);
    log_line("comm task started");

    /* Reception was armed in uart_dma_init(), before this task existed, so
     * anything that arrived in the meantime is already sitting in the stream
     * buffer with no notification pending. Drain it once. */
    drain_rx();

    for (;;)
    {
        uint32_t events = 0U;

        /* Clear every bit on exit so each wake reports only new events.
         * portMAX_DELAY: with nothing happening this task uses no CPU at all. */
        (void)xTaskNotifyWait(0x00000000UL, 0xFFFFFFFFUL, &events, portMAX_DELAY);

        if ((events & COMM_EVT_RX) != 0U)
        {
            drain_rx();
        }

        /* Always drain telemetry: a notification can coalesce with an earlier
         * one, so the queue is the authority on how much work is outstanding,
         * not the number of times we were woken. */
        drain_telemetry();
    }
}
