/**
 * @file    processing_task.h
 * @brief   Task 2 -- consumes raw samples, derives statistics and status.
 */
#ifndef PROCESSING_TASK_H
#define PROCESSING_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "telemetry.h"

void processing_task(void *arg);

/**
 * Copy the most recent processed result.
 *
 * The snapshot is taken inside a critical section so the caller can never
 * observe a half-updated structure -- the STATUS command handler and the health
 * task both read it from other task contexts.
 *
 * @return false if nothing has been processed yet.
 */
bool processing_task_get_last(processed_data_t *out);

uint32_t processing_task_processed_count(void);
uint32_t processing_task_rejected_count(void);
void     processing_task_reset_stats(void);

#endif /* PROCESSING_TASK_H */
