/**
 * @file    comm_task.h
 * @brief   Task 3 -- the ESP32 link and the command interpreter.
 *
 * Fully event driven. The task blocks on xTaskNotifyWait() and is woken by
 * exactly two things:
 *   COMM_EVT_TELEMETRY -- the processing task queued a result to transmit
 *   COMM_EVT_RX        -- the UART IDLE-line ISR delivered received bytes
 *
 * Using notification BITS here (rather than the counting notification the
 * sensor task uses) lets one wait serve two independent event sources without
 * a queue set and without polling.
 */
#ifndef COMM_TASK_H
#define COMM_TASK_H

#include <stdint.h>

#define COMM_EVT_TELEMETRY  (1UL << 0)
#define COMM_EVT_RX         (1UL << 1)

void comm_task(void *arg);

/** Wake the task because a result is waiting on the telemetry queue. */
void comm_task_notify_telemetry(void);

uint32_t comm_task_tx_count(void);
uint32_t comm_task_tx_error_count(void);
uint32_t comm_task_command_count(void);

#endif /* COMM_TASK_H */
