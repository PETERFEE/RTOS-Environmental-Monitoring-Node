/**
 * @file    app_tasks.h
 * @brief   Ownership of every RTOS object in the system, in one place.
 *
 * Keeping the queue and mutex handles here (rather than scattered through the
 * task modules) means the data flow can be read off a single header, and there
 * is exactly one function that decides creation order.
 *
 * PHASE 5 -- five tasks, plus the I2C mutex:
 *
 *   sensor --[sensorQ]--> processing --[telemetryQ]--> comm --> ESP32
 *      |                       |
 *      |                   [logQ] --> logging --> PC console
 *      |                       ^
 *      +--[i2cMutex]--+        |
 *                     |        |
 *                  health -----+
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#include "telemetry.h"

/* --------------------------------------------------------------------------
 *  Shared RTOS objects
 * ----------------------------------------------------------------------- */
extern QueueHandle_t g_sensor_queue;     /**< sensor_data_t,    sensor -> processing */
extern QueueHandle_t g_telemetry_queue;  /**< processed_data_t, processing -> comm   */
extern QueueHandle_t g_log_queue;        /**< log_msg_t,        anyone -> logging    */

/** Guards the I2C1 bus. A MUTEX, not a binary semaphore: FreeRTOS mutexes
 *  implement priority inheritance, so while the low-priority health task holds
 *  the bus it is temporarily promoted to the priority of the highest-priority
 *  task waiting for it. Without that, the sensor task (prio 3) could be made to
 *  wait on the health task (prio 1) for as long as any middle-priority task
 *  wanted to run -- unbounded priority inversion. */
extern SemaphoreHandle_t g_i2c_mutex;

extern TaskHandle_t  g_sensor_task;
extern TaskHandle_t  g_processing_task;
extern TaskHandle_t  g_comm_task;
extern TaskHandle_t  g_logging_task;
extern TaskHandle_t  g_health_task;

/** Create every RTOS object and task. Call before vTaskStartScheduler(). */
bool app_tasks_create(void);

/** Called when a queue send is dropped because the consumer fell behind. */
void     app_note_queue_drop(void);
uint32_t app_queue_drop_count(void);

/** Hand a finished result to the communication task. */
void app_publish_processed(const processed_data_t *data);

/**
 * Set the heartbeat LED's meaning.
 *
 * LD2 blinks at 1 Hz while readings are good and at 5 Hz once the sensor starts
 * returning unusable data, so the board's state is readable across the room with
 * no terminal attached. Driven by a FreeRTOS software timer, not a task: the
 * work is a single GPIO toggle and does not justify a TCB and a stack.
 */
void app_heartbeat_set_fault(bool fault);

#endif /* APP_TASKS_H */
