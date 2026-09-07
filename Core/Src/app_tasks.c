/**
 * @file    app_tasks.c
 * @brief   Creates the RTOS objects and starts the application tasks.
 *
 * PHASE 5 -- the full data path, plus bus arbitration:
 *
 *   sensor_task(3) --[g_sensor_queue]--> processing_task(2)
 *                                              |
 *                                     [g_telemetry_queue]
 *                                              v
 *                                        comm_task(2) --> ESP32
 *
 *   any task ------------[g_log_queue]--> logging_task(1) --> PC console
 *
 *   sensor_task(3) and health_task(1) both need I2C1, arbitrated by
 *   g_i2c_mutex with priority inheritance.
 *
 *   A software timer blinks LD2 as a liveness indicator -- 1 Hz healthy,
 *   5 Hz once the sensor stops returning usable data.
 *
 * Priorities encode deadlines, not importance: the sensor task is highest
 * because it is the only one with a hard timing requirement, and logging is
 * lowest because a late log line costs nothing.
 */
#include "app_tasks.h"

#include <string.h>

#include "comm_task.h"
#include "bme680_driver.h"
#include "health_task.h"
#include "logging_task.h"
#include "main.h"
#include "processing_task.h"
#include "sensor_task.h"
#include "uart_dma.h"

QueueHandle_t g_sensor_queue    = NULL;
QueueHandle_t g_telemetry_queue = NULL;
QueueHandle_t g_log_queue       = NULL;

TaskHandle_t g_sensor_task     = NULL;
TaskHandle_t g_processing_task = NULL;
TaskHandle_t g_comm_task       = NULL;
TaskHandle_t g_logging_task    = NULL;
TaskHandle_t g_health_task     = NULL;

SemaphoreHandle_t g_i2c_mutex  = NULL;

static TimerHandle_t s_heartbeat_timer = NULL;
static bool          s_heartbeat_fault = false;

#define HEARTBEAT_OK_MS      500U   /* 1 Hz blink: everything is fine       */
#define HEARTBEAT_FAULT_MS   100U   /* 5 Hz blink: sensor data is unusable  */

static volatile uint32_t s_queue_drops = 0U;

void     app_note_queue_drop(void)  { s_queue_drops++; }
uint32_t app_queue_drop_count(void) { return s_queue_drops; }

/* --------------------------------------------------------------------------
 *  Sensor bus ownership
 *
 *  Strong overrides of the weak hooks in bme680_driver.c. The driver still has
 *  no idea FreeRTOS exists; it just calls lock/unlock/delay.
 * ----------------------------------------------------------------------- */
bool bme680_port_lock(uint32_t timeout_ms)
{
    if (g_i2c_mutex == NULL || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return true;                 /* single-threaded during bring-up */
    }
    return xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bme680_port_unlock(void)
{
    if (g_i2c_mutex != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        (void)xSemaphoreGive(g_i2c_mutex);
    }
}

void bme680_port_delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        HAL_Delay(ms);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* --------------------------------------------------------------------------
 *  Heartbeat LED
 *
 *  A software timer rather than a sixth task. The callback is one GPIO toggle,
 *  and every timer callback shares the single timer-service task, so this costs
 *  no extra stack or TCB -- the right trade for periodic work that never blocks.
 *
 *  Timer callbacks run in the timer task's context, so they must never block:
 *  no vTaskDelay, no waiting on a mutex. A toggle qualifies.
 * ----------------------------------------------------------------------- */
static void heartbeat_callback(TimerHandle_t timer)
{
    (void)timer;
    HAL_GPIO_TogglePin(APP_LED_PORT, APP_LED_PIN);
}

void app_heartbeat_set_fault(bool fault)
{
    /* Only act on a change, so a steady stream of good (or bad) samples does
     * not re-arm the timer a thousand times and reset its phase. */
    if (s_heartbeat_timer == NULL || fault == s_heartbeat_fault)
    {
        return;
    }
    s_heartbeat_fault = fault;

    const TickType_t period =
        pdMS_TO_TICKS(fault ? HEARTBEAT_FAULT_MS : HEARTBEAT_OK_MS);

    /* Zero block time: if the timer command queue is full, the blink rate is
     * simply not worth blocking a task over. */
    (void)xTimerChangePeriod(s_heartbeat_timer, period, 0U);
}

/* --------------------------------------------------------------------------
 *  Button interrupt -> sensor task
 *
 *  The ISR does the minimum defensible amount of work: debounce, signal, ask
 *  for a context switch. Everything else -- reading the sensor, formatting,
 *  transmitting -- happens in task context where it can block safely.
 *
 *  Why a direct-to-task notification instead of a binary semaphore or a queue?
 *  There is exactly one receiving task, so the notification value already
 *  living in that task's TCB is enough. No separate kernel object is allocated
 *  (saving ~80 bytes of heap) and the give/take path is shorter. A semaphore
 *  would be the right answer for several waiters; a queue for carrying data.
 * ----------------------------------------------------------------------- */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin != APP_BUTTON_PIN)
    {
        return;
    }

    /* Contact bounce on a tactile switch lasts a few milliseconds and would
     * otherwise queue a burst of samples from one press. Suppressing in the ISR
     * by timestamp is cheaper than a debounce timer and needs no extra object. */
    static uint32_t s_last_press_tick = 0U;

    const uint32_t now = xTaskGetTickCountFromISR();
    if ((now - s_last_press_tick) < pdMS_TO_TICKS(APP_BUTTON_DEBOUNCE_MS))
    {
        return;
    }
    s_last_press_tick = now;

    BaseType_t higher_prio_woken = pdFALSE;
    sensor_task_trigger_from_isr(&higher_prio_woken);

    /* If the notification made a higher-priority task ready, switch to it on
     * exit from this ISR instead of waiting for the next tick. */
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* --------------------------------------------------------------------------
 *  Result fan-out
 * ----------------------------------------------------------------------- */
void app_publish_processed(const processed_data_t *data)
{
    if (data == NULL || g_telemetry_queue == NULL)
    {
        return;
    }

    /* Overwrite-oldest semantics would be nicer here, but a plain queue with a
     * zero timeout keeps the failure visible: telemetry that cannot be sent is
     * counted rather than silently replaced. */
    if (xQueueSend(g_telemetry_queue, data, 0U) != pdPASS)
    {
        app_note_queue_drop();
        return;
    }

    comm_task_notify_telemetry();
}

/* --------------------------------------------------------------------------
 *  Creation
 *
 *  Order matters: every queue must exist before any task that might touch it is
 *  allowed to run. Since no task runs until vTaskStartScheduler(), creating all
 *  queues first is sufficient.
 * ----------------------------------------------------------------------- */
bool app_tasks_create(void)
{
    if (!uart_dma_init())
    {
        return false;
    }

    g_i2c_mutex = xSemaphoreCreateMutex();
    if (g_i2c_mutex == NULL)
    {
        return false;
    }

    s_heartbeat_timer = xTimerCreate("Heartbeat",
                                     pdMS_TO_TICKS(HEARTBEAT_OK_MS),
                                     pdTRUE,          /* auto-reload        */
                                     NULL,
                                     heartbeat_callback);
    if (s_heartbeat_timer == NULL)
    {
        return false;
    }

    g_sensor_queue    = xQueueCreate(SENSOR_QUEUE_LENGTH,    sizeof(sensor_data_t));
    g_telemetry_queue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(processed_data_t));
    g_log_queue       = xQueueCreate(LOG_QUEUE_LENGTH,       sizeof(log_msg_t));

    if (g_sensor_queue == NULL || g_telemetry_queue == NULL || g_log_queue == NULL)
    {
        return false;
    }

    vQueueAddToRegistry(g_sensor_queue,    "sensorQ");
    vQueueAddToRegistry(g_telemetry_queue, "telemetryQ");
    vQueueAddToRegistry(g_log_queue,       "logQ");

    if (xTaskCreate(sensor_task, "Sensor", TASK_STACK_SENSOR, NULL,
                    TASK_PRIO_SENSOR, &g_sensor_task) != pdPASS)
    {
        return false;
    }
    if (xTaskCreate(processing_task, "Process", TASK_STACK_PROCESSING, NULL,
                    TASK_PRIO_PROCESSING, &g_processing_task) != pdPASS)
    {
        return false;
    }
    if (xTaskCreate(comm_task, "Comm", TASK_STACK_COMM, NULL,
                    TASK_PRIO_COMM, &g_comm_task) != pdPASS)
    {
        return false;
    }
    if (xTaskCreate(logging_task, "Log", TASK_STACK_LOGGING, NULL,
                    TASK_PRIO_LOGGING, &g_logging_task) != pdPASS)
    {
        return false;
    }
    if (xTaskCreate(health_task, "Health", TASK_STACK_HEALTH, NULL,
                    TASK_PRIO_HEALTH, &g_health_task) != pdPASS)
    {
        return false;
    }

    /* Starts pending; the timer service task arms it once the scheduler runs. */
    if (xTimerStart(s_heartbeat_timer, 0U) != pdPASS)
    {
        return false;
    }

    return true;
}
