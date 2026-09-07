/**
 * @file    perf.h
 * @brief   Cycle-accurate timing using the Cortex-M3 DWT counter.
 *
 * The DWT (Data Watchpoint and Trace) unit contains a free-running 32-bit cycle
 * counter. At 64 MHz it ticks every 15.6 ns and wraps every ~67 s, which is far
 * finer than the 1 ms RTOS tick and is what docs/measurements.md uses to report
 * task execution times.
 */
#ifndef PERF_H
#define PERF_H

#include <stdint.h>

/** Enable the trace unit and start the cycle counter. Call once, early. */
void perf_init(void);

/** Raw CPU cycle count. Wraps naturally; differences stay correct across wrap. */
uint32_t perf_cycles(void);

/** Microseconds elapsed since @p start_cycles (captured with perf_cycles()). */
uint32_t perf_elapsed_us(uint32_t start_cycles);

/** Busy-wait. Safe to call from anywhere, including before the scheduler runs. */
void perf_delay_us(uint32_t us);

#endif /* PERF_H */
