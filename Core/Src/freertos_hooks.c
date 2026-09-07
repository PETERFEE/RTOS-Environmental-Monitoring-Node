/**
 * @file    freertos_hooks.c
 * @brief   Kernel callbacks: failure hooks and static memory for Idle/Timer.
 *
 * Every hook here is a "this should never happen" path. On a resource-tight
 * part they are the difference between a board that reboots mysteriously and
 * one that tells you which task blew its stack.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "main.h"

/* --------------------------------------------------------------------------
 *  Failure hooks
 * ----------------------------------------------------------------------- */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;

    __disable_irq();
    board_uart_write_blocking("\r\n!! STACK OVERFLOW in task: ", 29U);
    if (task_name != NULL)
    {
        uint16_t n = 0U;
        while (task_name[n] != '\0' && n < 32U) { n++; }
        board_uart_write_blocking(task_name, n);
    }
    board_uart_write_blocking("\r\n", 2U);

    for (;;) { __NOP(); }
}

void vApplicationMallocFailedHook(void)
{
    __disable_irq();
    board_uart_write_blocking("\r\n!! HEAP EXHAUSTED (pvPortMalloc returned NULL)\r\n", 49U);
    for (;;) { __NOP(); }
}

void vAssertCalled(const char *file, int line)
{
    (void)file;

    taskDISABLE_INTERRUPTS();

    board_uart_write_blocking("\r\n!! configASSERT failed at line ", 32U);

    /* Print the line number without printf. */
    char digits[12];
    int  n = 0;
    if (line <= 0)
    {
        digits[n++] = '0';
    }
    else
    {
        while (line > 0 && n < 11)
        {
            digits[n++] = (char)('0' + (line % 10));
            line /= 10;
        }
    }
    char out[12];
    for (int i = 0; i < n; i++)
    {
        out[i] = digits[n - 1 - i];
    }
    board_uart_write_blocking(out, (uint16_t)n);
    board_uart_write_blocking("\r\n", 2U);

    for (;;) { __NOP(); }
}

/* --------------------------------------------------------------------------
 *  Static allocation for the kernel's own tasks
 *
 *  With configSUPPORT_STATIC_ALLOCATION the kernel asks the application where
 *  to put the Idle and Timer task control blocks and stacks. Placing them in
 *  .bss keeps ~1.5 KB out of the FreeRTOS heap and turns an over-committed RAM
 *  budget into a link-time error rather than a boot-time malloc failure.
 * ----------------------------------------------------------------------- */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb,
                                   StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *stack_size)
{
    *tcb        = &s_idle_tcb;
    *stack      = s_idle_stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb,
                                    StackType_t **stack,
                                    configSTACK_DEPTH_TYPE *stack_size)
{
    *tcb        = &s_timer_tcb;
    *stack      = s_timer_stack;
    *stack_size = configTIMER_TASK_STACK_DEPTH;
}
