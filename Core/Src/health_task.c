/**
 * @file    health_task.c
 * @brief   Task 5 -- system health monitor.
 *
 * Every HEALTH_PERIOD_MS it reports:
 *   - free heap now, and the low-water mark since boot
 *   - occupancy of each queue
 *   - stack high-water mark of every task, in words still unused
 *   - sample / error / drop counters
 *
 * The stack figures are the useful ones: they turn stack sizing from guesswork
 * into measurement. Anything that never drops below ~40 words of headroom is
 * over-provisioned; anything approaching zero is one deep call away from
 * corrupting its neighbour.
 *
 * The periodic chip-ID read is deliberate. It gives the system a SECOND task
 * that needs the I2C bus, which is what makes the mutex in app_tasks.c load
 * bearing rather than decorative.
 */
#include "health_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include <string.h>

#include "app_tasks.h"
#include "bme680_driver.h"
#include "comm_task.h"
#include "logging_task.h"
#include "main.h"
#include "processing_task.h"
#include "sensor_task.h"

static volatile uint32_t s_min_free_heap = 0xFFFFFFFFU;
static volatile uint32_t s_bus_errors    = 0U;

uint32_t health_min_free_heap(void)  { return s_min_free_heap; }
uint32_t health_bus_error_count(void){ return s_bus_errors; }

static void report_stack(const char *name, TaskHandle_t task)
{
    if (task == NULL)
    {
        return;
    }

    char key[LOG_LINE_MAX];
    key[0] = '\0';
    strncat(key, "  stack.", sizeof(key) - 1U);
    strncat(key, name, sizeof(key) - strlen(key) - 1U);

    /* uxTaskGetStackHighWaterMark returns the minimum number of WORDS that have
     * ever remained unused -- multiply by 4 for bytes. */
    log_kv_u32(key, (uint32_t)uxTaskGetStackHighWaterMark(task));
}

void health_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEALTH_PERIOD_MS));

        const uint32_t free_heap = (uint32_t)xPortGetFreeHeapSize();
        if (free_heap < s_min_free_heap)
        {
            s_min_free_heap = free_heap;
        }

        log_line("--- health ---");
        log_kv_u32("  heap.free",       free_heap);
        log_kv_u32("  heap.min",        s_min_free_heap);
        log_kv_u32("  heap.minEver",    (uint32_t)xPortGetMinimumEverFreeHeapSize());

        log_kv_u32("  q.sensor",        (uint32_t)uxQueueMessagesWaiting(g_sensor_queue));
        log_kv_u32("  q.telemetry",     (uint32_t)uxQueueMessagesWaiting(g_telemetry_queue));
        log_kv_u32("  q.log",           (uint32_t)uxQueueMessagesWaiting(g_log_queue));

        report_stack("sensor",  g_sensor_task);
        report_stack("process", g_processing_task);
        report_stack("comm",    g_comm_task);
        report_stack("log",     g_logging_task);
        report_stack("health",  g_health_task);

        log_kv_u32("  samples",   sensor_task_sample_count());
        log_kv_u32("  forced",    sensor_task_forced_count());
        log_kv_u32("  sensorErr", sensor_task_error_count());
        log_kv_u32("  rejected",  processing_task_rejected_count());
        log_kv_u32("  qDrops",    app_queue_drop_count());
        log_kv_u32("  logDrops",  log_dropped_count());
        log_kv_u32("  txCount",   comm_task_tx_count());
        log_kv_u32("  txErr",     comm_task_tx_error_count());

        /* Second I2C user: proves the bus mutex works under real contention. */
        uint8_t chip_id = 0U;
        if (bme680_driver_read_chip_id(&chip_id))
        {
            log_kv_u32("  bus.chipId", (uint32_t)chip_id);
        }
        else
        {
            s_bus_errors++;
            log_line("  bus.chipId=FAILED");
        }
    }
}
