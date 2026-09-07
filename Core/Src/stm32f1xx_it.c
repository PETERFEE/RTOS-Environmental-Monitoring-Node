/**
 * @file    stm32f1xx_it.c
 * @brief   Exception and interrupt handlers.
 *
 * SVC_Handler and PendSV_Handler are supplied by the FreeRTOS Cortex-M3 port
 * (FreeRTOSConfig.h maps vPortSVCHandler/xPortPendSVHandler onto those names),
 * so they deliberately do not appear here.
 */
#include "stm32f1xx_it.h"

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"

/* Defined by the FreeRTOS Cortex-M3 port (port.c). The port header does not
 * declare it, because CubeMX projects normally map SysTick_Handler straight
 * onto it; this project shares the interrupt with the HAL instead. */
extern void xPortSysTickHandler(void);

#if APP_USE_UART_DMA
extern void uart_dma_idle_isr(UART_HandleTypeDef *huart);
#endif

/* --------------------------------------------------------------------------
 *  Fault reporting
 *
 *  On a fault the CPU has already stacked R0-R3, R12, LR, PC and xPSR. The
 *  naked wrapper works out which stack pointer was in use and hands the frame
 *  to a C function, so a crash prints the faulting PC instead of silently
 *  spinning -- the single most useful five minutes of debugging setup on any
 *  Cortex-M project.
 * ----------------------------------------------------------------------- */
static void put_hex32(uint32_t v)
{
    static const char k_hex[] = "0123456789ABCDEF";
    char out[11] = "0x";

    for (int i = 0; i < 8; i++)
    {
        out[2 + i] = k_hex[(v >> (28 - (4 * i))) & 0x0FU];
    }
    out[10] = '\0';
    board_uart_write_blocking(out, 10U);
}

void hardfault_report(uint32_t *stack_frame) __attribute__((used));
void hardfault_report(uint32_t *stack_frame)
{
    __disable_irq();

    board_uart_write_blocking("\r\n!! HARD FAULT\r\n", 17U);

    static const char *const k_names[8] = {
        "R0  =", "R1  =", "R2  =", "R3  =", "R12 =", "LR  =", "PC  =", "xPSR="
    };
    for (int i = 0; i < 8; i++)
    {
        board_uart_write_blocking(k_names[i], 5U);
        board_uart_write_blocking(" ", 1U);
        put_hex32(stack_frame[i]);
        board_uart_write_blocking("\r\n", 2U);
    }

    board_uart_write_blocking("CFSR= ", 6U);
    put_hex32(SCB->CFSR);
    board_uart_write_blocking("  HFSR= ", 8U);
    put_hex32(SCB->HFSR);
    board_uart_write_blocking("\r\n", 2U);

    for (;;) { __NOP(); }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile
    (
        "tst   lr, #4            \n"   /* EXC_RETURN bit 2: which stack?      */
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "b     hardfault_report  \n"
    );
}

/* --------------------------------------------------------------------------
 *  Cortex-M3 system exceptions
 * ----------------------------------------------------------------------- */
void NMI_Handler(void)
{
    for (;;) { __NOP(); }
}

void MemManage_Handler(void)
{
    for (;;) { __NOP(); }
}

void BusFault_Handler(void)
{
    for (;;) { __NOP(); }
}

void UsageFault_Handler(void)
{
    for (;;) { __NOP(); }
}

void DebugMon_Handler(void)
{
}

/* --------------------------------------------------------------------------
 *  Peripheral interrupts
 * ----------------------------------------------------------------------- */

/** User button B1 on PC13 -> EXTI line 13, which shares the 10..15 vector. */
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(APP_BUTTON_PIN);
}

/* --- DMA channels ------------------------------------------------------
 * The F1's channel assignments are fixed in silicon:
 *   USART1_TX = ch4, USART1_RX = ch5, USART2_RX = ch6, USART2_TX = ch7
 * ---------------------------------------------------------------------- */
void DMA1_Channel4_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart1_tx); }
void DMA1_Channel5_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart1_rx); }
void DMA1_Channel6_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_rx); }
void DMA1_Channel7_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_tx); }

/**
 * USART interrupts.
 *
 * The IDLE flag is checked and cleared FIRST, before HAL_UART_IRQHandler() is
 * given the chance to look at the peripheral: the HAL has no notion of
 * idle-line reception and would otherwise leave the flag set, re-entering this
 * handler forever.
 */
static void usart_common_isr(UART_HandleTypeDef *huart)
{
#if APP_USE_UART_DMA
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE) &&
        __HAL_UART_GET_IT_SOURCE(huart, UART_IT_IDLE))
    {
        __HAL_UART_CLEAR_IDLEFLAG(huart);
        uart_dma_idle_isr(huart);
    }
#endif
    HAL_UART_IRQHandler(huart);
}

void USART1_IRQHandler(void)
{
    usart_common_isr((huart_log.Instance == USART1) ? &huart_log : &huart_comm);
}

void USART2_IRQHandler(void)
{
    usart_common_isr((huart_log.Instance == USART2) ? &huart_log : &huart_comm);
}

/**
 * One 1 kHz interrupt serves both time bases.
 *
 * HAL_IncTick() keeps HAL_GetTick()/HAL_Delay() working (the HAL drivers use
 * them for their timeouts), and the kernel tick is only forwarded once the
 * scheduler is actually running -- before that, xPortSysTickHandler() would
 * touch a task list that does not exist yet.
 *
 * The usual CubeMX alternative is to give the HAL its own TIM. Sharing SysTick
 * costs nothing here because both want exactly 1 ms.
 */
void SysTick_Handler(void)
{
    HAL_IncTick();

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        xPortSysTickHandler();
    }
}
