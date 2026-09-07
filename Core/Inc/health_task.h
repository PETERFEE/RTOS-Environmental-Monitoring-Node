/**
 * @file    health_task.h
 * @brief   Task 5 -- periodic resource and liveness reporting.
 *
 * Reports what an embedded system actually runs out of: heap, task stacks and
 * queue space. It also performs a periodic I2C sanity read, which makes it the
 * second task contending for the sensor bus and therefore the reason the I2C
 * mutex exists at all.
 */
#ifndef HEALTH_TASK_H
#define HEALTH_TASK_H

#include <stdint.h>

void health_task(void *arg);

/** Smallest free-heap figure seen since boot. */
uint32_t health_min_free_heap(void);

/** Number of periodic I2C sanity checks that failed. */
uint32_t health_bus_error_count(void);

#endif /* HEALTH_TASK_H */
