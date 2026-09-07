/**
 * @file    sensor_task.c
 * @brief   Task 1 -- periodic acquisition from the BME680.
 *
 * Runs at the highest application priority because it is the only task with a
 * hard timing requirement: a sample that arrives late is a sample taken at the
 * wrong time, and no amount of downstream buffering fixes that.
 *
 * PHASE 6 -- the task now waits on a direct-to-task notification with a timeout
 * instead of a plain delay, so a button press (or a SAMPLE_NOW command) can
 * force an immediate acquisition without disturbing the periodic schedule.
 */
#include "sensor_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_tasks.h"
#include "bme680_driver.h"
#include "logging_task.h"
#include "main.h"
#include "telemetry.h"

static volatile uint32_t s_period_ms    = SENSOR_PERIOD_MS_DEFAULT;
static volatile uint32_t s_sample_count = 0U;
static volatile uint32_t s_error_count  = 0U;
static volatile uint32_t s_forced_count = 0U;

/* --------------------------------------------------------------------------
 *  Runtime controls
 * ----------------------------------------------------------------------- */
void sensor_task_set_period(uint32_t period_ms)
{
    if (period_ms < SENSOR_PERIOD_MS_MIN) { period_ms = SENSOR_PERIOD_MS_MIN; }
    if (period_ms > SENSOR_PERIOD_MS_MAX) { period_ms = SENSOR_PERIOD_MS_MAX; }

    /* A single aligned 32-bit store is atomic on Cortex-M3, so no critical
     * section is needed: the task picks the new value up next iteration. */
    s_period_ms = period_ms;
}

uint32_t sensor_task_get_period(void) { return s_period_ms; }

void sensor_task_trigger(void)
{
    if (g_sensor_task != NULL)
    {
        xTaskNotifyGive(g_sensor_task);
    }
}

void sensor_task_trigger_from_isr(BaseType_t *higher_prio_woken)
{
    if (g_sensor_task != NULL)
    {
        vTaskNotifyGiveFromISR(g_sensor_task, higher_prio_woken);
    }
}

uint32_t sensor_task_forced_count(void) { return s_forced_count; }
uint32_t sensor_task_sample_count(void) { return s_sample_count; }
uint32_t sensor_task_error_count(void)  { return s_error_count; }

void sensor_task_reset_stats(void)
{
    s_sample_count = 0U;
    s_error_count  = 0U;
    s_forced_count = 0U;
}

/* --------------------------------------------------------------------------
 *  Task body
 * ----------------------------------------------------------------------- */
void sensor_task(void *arg)
{
    (void)arg;

    /* next_wake is kept as an ABSOLUTE tick count rather than a delay, so the
     * period stays anchored to the schedule even though a BME680 conversion
     * takes a variable ~180 ms and forced samples arrive at arbitrary times.
     * This is xTaskDelayUntil()'s trick, done by hand because the task also has
     * to be interruptible by a notification. */
    TickType_t next_wake = xTaskGetTickCount() + pdMS_TO_TICKS(s_period_ms);

    for (;;)
    {
        const TickType_t now = xTaskGetTickCount();

        /* Signed comparison handles tick-counter wrap correctly. */
        TickType_t remaining = 0U;
        if ((int32_t)(next_wake - now) > 0)
        {
            remaining = next_wake - now;
        }

        /* Block until the period expires OR someone asks for a sample now.
         *
         * A direct-to-task notification is used rather than a binary semaphore
         * or a queue because there is exactly ONE receiving task. The kernel
         * stores the notification in the TCB the task already has, so no
         * separate object is allocated and the signalling path is measurably
         * faster than a semaphore give/take. */
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, remaining);

        const bool forced = (notified > 0U);
        if (forced)
        {
            s_forced_count++;
            log_line("sensor: forced sample");
        }
        else
        {
            /* Periodic wake: advance the schedule. If the system fell so far
             * behind that the next slot has already passed, resynchronise
             * rather than trying to catch up with a burst of samples. */
            next_wake += pdMS_TO_TICKS(s_period_ms);
            if ((int32_t)(next_wake - xTaskGetTickCount()) <= 0)
            {
                next_wake = xTaskGetTickCount() + pdMS_TO_TICKS(s_period_ms);
            }
        }

        sensor_data_t sample;
        const bme680_status_t rc = bme680_driver_read(&sample);
        sample.forced = forced;

        if (rc == BME680_OK)
        {
            s_sample_count++;
        }
        else
        {
            s_error_count++;
            sample.valid = false;
        }

        /* Non-blocking send. If the processing task has fallen behind, dropping
         * the newest sample is better than stalling the one task in the system
         * with a real deadline. The drop is visible in the health report. */
        if (xQueueSend(g_sensor_queue, &sample, 0U) != pdPASS)
        {
            app_note_queue_drop();
        }
    }
}
