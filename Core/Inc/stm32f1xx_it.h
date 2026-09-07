/**
 * @file    stm32f1xx_it.h
 * @brief   Cortex-M3 and peripheral interrupt handler prototypes.
 */
#ifndef STM32F1xx_IT_H
#define STM32F1xx_IT_H

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
void SysTick_Handler(void);

#ifdef __cplusplus
}
#endif
#endif /* STM32F1xx_IT_H */
