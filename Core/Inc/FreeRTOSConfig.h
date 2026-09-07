/**
 * @file    FreeRTOSConfig.h
 * @brief   Kernel configuration for STM32F103RB (Cortex-M3, 64 MHz, 20 KB SRAM).
 *
 * Notes on the choices that matter on this part:
 *
 *  - configTOTAL_HEAP_SIZE is the single biggest RAM decision. The F103RB has
 *    20 KB of SRAM total, so the heap is sized from the measured task stacks
 *    and queue storage rather than picked optimistically. health_task.c prints
 *    the live free-heap and stack high-water marks so this number can be tuned
 *    with evidence -- see docs/measurements.md.
 *
 *  - Static allocation is enabled and used for the Idle and Timer tasks so
 *    their stacks land in .bss. That takes ~1.5 KB of guesswork out of the
 *    heap budget and makes over-commitment a link error instead of a runtime
 *    failure.
 *
 *  - Interrupt priorities: the Cortex-M3 on the F1 implements 4 priority bits.
 *    Any ISR that calls a FreeRTOS ...FromISR() API must have a numerically
 *    higher (logically lower) priority value than
 *    configMAX_SYSCALL_INTERRUPT_PRIORITY. This project puts every such ISR at
 *    6 or 7; see app_config.h.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

/* ------------------------------------------------------------------------ */
/*  Scheduler                                                                */
/* ------------------------------------------------------------------------ */
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1   /* CM3 CLZ instruction  */
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     5
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configMAX_TASK_NAME_LEN                  12
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0

/* ------------------------------------------------------------------------ */
/*  Synchronisation primitives                                               */
/* ------------------------------------------------------------------------ */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1
#define configQUEUE_REGISTRY_SIZE                6   /* named queues in the debugger */
#define configUSE_QUEUE_SETS                     0
#define configUSE_STREAM_BUFFERS                 1
#define configUSE_APPLICATION_TASK_TAG           0

/* ------------------------------------------------------------------------ */
/*  Memory                                                                   */
/* ------------------------------------------------------------------------ */
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configSUPPORT_STATIC_ALLOCATION          1
#define configTOTAL_HEAP_SIZE                    ((size_t)(9 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP         0

/* ------------------------------------------------------------------------ */
/*  Hooks and run-time checks
 *
 *  Stack-overflow checking is left at method 2 (pattern fill + check on every
 *  context switch) deliberately: on a 20 KB part an overflow corrupts the
 *  neighbouring task's TCB and produces a fault that is very hard to read
 *  backwards. The few cycles per switch are worth it.
 * ------------------------------------------------------------------------ */
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

/* ------------------------------------------------------------------------ */
/*  Software timers                                                          */
/* ------------------------------------------------------------------------ */
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                 4
#define configTIMER_TASK_STACK_DEPTH             128

/* ------------------------------------------------------------------------ */
/*  Statistics / introspection                                               */
/* ------------------------------------------------------------------------ */
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configGENERATE_RUN_TIME_STATS            0
#define configRECORD_STACK_HIGH_ADDRESS          1

/* ------------------------------------------------------------------------ */
/*  Optional API                                                             */
/* ------------------------------------------------------------------------ */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      0
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           0
#define INCLUDE_eTaskGetState                    0
#define INCLUDE_xTimerPendFunctionCall           0
#define INCLUDE_xQueueGetMutexHolder             1

/* ------------------------------------------------------------------------ */
/*  Cortex-M interrupt priorities                                            */
/* ------------------------------------------------------------------------ */
#define configPRIO_BITS                          4

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY        15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY   5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Catches the classic bug of calling a FromISR API from an interrupt whose
 * priority is above configMAX_SYSCALL_INTERRUPT_PRIORITY. */
#define configASSERT_DEFINED                     1
extern void vAssertCalled(const char *file, int line);
#define configASSERT(x)  if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); }

/* ------------------------------------------------------------------------ */
/*  Handler names
 *
 *  SVC and PendSV go straight to the port. SysTick is NOT mapped here: the
 *  handler in stm32f1xx_it.c drives the HAL time base as well as the kernel
 *  tick, so the two share one 1 kHz interrupt instead of burning a TIM.
 * ------------------------------------------------------------------------ */
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler

#endif /* FREERTOS_CONFIG_H */
