/**
 * @file    processing_task.c
 * @brief   Task 2 -- blocks on the sensor queue, folds each sample into the
 *          running statistics, classifies it and forwards the result.
 *
 * The whole task is one blocking xQueueReceive(): with no data it consumes
 * exactly zero CPU, and the kernel moves it to the ready list the instant the
 * sensor task posts. That is the reason to use a queue instead of a polled
 * flag, and it is what keeps the idle task free to do power management later.
 *
 * All of the actual mathematics lives in processing.c, which has no RTOS or HAL
 * dependencies and is unit-tested on the host (tests/test_processing.py).
 */
#include "processing_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_tasks.h"
#include "main.h"
#include "processing.h"

static processing_ctx_t  s_ctx;
static processed_data_t  s_last;
static bool              s_have_last = false;
static volatile uint32_t s_rejected  = 0U;

bool processing_task_get_last(processed_data_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    bool have;

    /* processed_data_t is ~60 bytes, far too big for an atomic load. A short
     * critical section is cheaper and simpler than a second mutex here. */
    taskENTER_CRITICAL();
    have = s_have_last;
    if (have)
    {
        *out = s_last;
    }
    taskEXIT_CRITICAL();

    return have;
}

uint32_t processing_task_processed_count(void) { return s_ctx.samples_processed; }
uint32_t processing_task_rejected_count(void)  { return s_rejected; }

void processing_task_reset_stats(void)
{
    taskENTER_CRITICAL();
    processing_init(&s_ctx);
    s_rejected  = 0U;
    s_have_last = false;
    taskEXIT_CRITICAL();
}

void processing_task(void *arg)
{
    (void)arg;

    processing_init(&s_ctx);

    for (;;)
    {
        sensor_data_t sample;

        /* portMAX_DELAY: block forever. No timeout is wanted -- a missing
         * sample is the sensor task's problem to report, not this task's. */
        if (xQueueReceive(g_sensor_queue, &sample, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        processed_data_t result;
        const bool ok = processing_update(&s_ctx, &sample, &result);

        if (!ok)
        {
            s_rejected++;
        }

        /* Drive the heartbeat LED from the one place that knows whether the
         * data is actually usable. */
        app_heartbeat_set_fault(!ok);

        taskENTER_CRITICAL();
        s_last      = result;
        s_have_last = true;
        taskEXIT_CRITICAL();

        app_publish_processed(&result);
    }
}
