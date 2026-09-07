/**
 * @file    sensor_task.h
 * @brief   Periodic BME680 acquisition, highest-priority task in the system.
 */
#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void sensor_task(void *arg);

/** Change the sampling period at runtime (clamped to the configured limits). */
void     sensor_task_set_period(uint32_t period_ms);
uint32_t sensor_task_get_period(void);

/** Force an immediate acquisition without disturbing the periodic schedule. */
void sensor_task_trigger(void);
void sensor_task_trigger_from_isr(BaseType_t *higher_prio_woken);

/** Counters surfaced by the health task and the STATUS command. */
uint32_t sensor_task_forced_count(void);
uint32_t sensor_task_sample_count(void);
uint32_t sensor_task_error_count(void);
void     sensor_task_reset_stats(void);

#endif /* SENSOR_TASK_H */
