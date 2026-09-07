/**
 * @file    perf.c
 * @brief   DWT cycle-counter based timing helpers.
 */
#include "perf.h"

#include "main.h"

void perf_init(void)
{
    /* Unlock the debug block, then enable the cycle counter. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t perf_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t perf_elapsed_us(uint32_t start_cycles)
{
    /* Unsigned subtraction gives the right answer even across the 32-bit wrap. */
    const uint32_t delta = DWT->CYCCNT - start_cycles;
    return delta / (APP_SYSCLK_HZ / 1000000UL);
}

void perf_delay_us(uint32_t us)
{
    const uint32_t start  = DWT->CYCCNT;
    const uint32_t target = us * (APP_SYSCLK_HZ / 1000000UL);

    while ((DWT->CYCCNT - start) < target)
    {
        __NOP();
    }
}
