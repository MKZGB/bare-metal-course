/*
 * startup.c - Reset handler and interrupt vector table for STM32F100RBT6
 * =====================================================================
 *
 * On a Cortex-M3 there is no BIOS and no bootloader doing setup for you.
 * When the chip leaves reset the hardware does only two things:
 *
 *   1. Loads the Main Stack Pointer (MSP) from the 32-bit word at address
 *      0x08000000 (vector-table entry 0).
 *   2. Loads the Program Counter (PC) from the word at 0x08000004
 *      (vector-table entry 1) and starts executing there - that is our
 *      Reset_Handler below.
 *
 * Everything else - copying initialised data to RAM, zeroing .bss, and
 * eventually calling main() - is OUR job. That is what this file does.
 *
 * This single startup file is shared by every module in the course.
 *
 * MISRA notes: fixed-width types only, no dynamic allocation, every
 * symbol is explicitly typed, and the table is fully populated.
 *
 * Reference: RM0041 sec. 10 (Interrupts and events / vector table) and
 * the Cortex-M3 programming manual PM0056.
 */

#include <stdint.h>

/* ---- Symbols provided by the linker script (linker.ld) -------------- */
extern uint32_t _sidata;   /* .data init image, start address (in Flash) */
extern uint32_t _sdata;    /* .data start (in RAM)                       */
extern uint32_t _edata;    /* .data end   (in RAM)                       */
extern uint32_t _sbss;     /* .bss  start (in RAM)                       */
extern uint32_t _ebss;     /* .bss  end   (in RAM)                       */
extern uint32_t _estack;   /* initial stack pointer (top of RAM)         */

/* ---- The student's entry point -------------------------------------- */
extern int main(void);

/* ---- Forward declaration of the reset handler ----------------------- */
void Reset_Handler(void);

/*
 * Default_Handler: a safe trap for any interrupt we have not written a
 * handler for. An infinite loop means "we got an unexpected exception" -
 * easy to catch in the debugger. Every handler below is a WEAK alias to
 * this, so if a module defines e.g. SysTick_Handler, the linker uses the
 * module's version instead of this default.
 */
void Default_Handler(void);

/* Each handler is declared 'weak' and aliased to Default_Handler.
 * 'weak' = "use this unless someone defines a stronger (normal) symbol
 * with the same name elsewhere". This is how a module supplies its own
 * ISR just by defining a function with the matching name.            */
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))

/* Cortex-M3 system exceptions ---------------------------------------- */
void NMI_Handler(void)        WEAK_ALIAS;
void HardFault_Handler(void)  WEAK_ALIAS;
void MemManage_Handler(void)  WEAK_ALIAS;
void BusFault_Handler(void)   WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void)        WEAK_ALIAS;
void DebugMon_Handler(void)   WEAK_ALIAS;
void PendSV_Handler(void)     WEAK_ALIAS;
void SysTick_Handler(void)    WEAK_ALIAS;

/* STM32F100 peripheral interrupts (RM0041 Table 51, vector positions). */
void WWDG_IRQHandler(void)            WEAK_ALIAS;
void PVD_IRQHandler(void)             WEAK_ALIAS;
void TAMPER_IRQHandler(void)          WEAK_ALIAS;
void RTC_IRQHandler(void)             WEAK_ALIAS;
void FLASH_IRQHandler(void)           WEAK_ALIAS;
void RCC_IRQHandler(void)             WEAK_ALIAS;
void EXTI0_IRQHandler(void)           WEAK_ALIAS;
void EXTI1_IRQHandler(void)           WEAK_ALIAS;
void EXTI2_IRQHandler(void)           WEAK_ALIAS;
void EXTI3_IRQHandler(void)           WEAK_ALIAS;
void EXTI4_IRQHandler(void)           WEAK_ALIAS;
void DMA1_Channel1_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel2_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel3_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel4_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel5_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel6_IRQHandler(void)   WEAK_ALIAS;
void DMA1_Channel7_IRQHandler(void)   WEAK_ALIAS;
void ADC1_IRQHandler(void)            WEAK_ALIAS;
void EXTI9_5_IRQHandler(void)         WEAK_ALIAS;
void TIM1_BRK_IRQHandler(void)        WEAK_ALIAS;
void TIM1_UP_IRQHandler(void)         WEAK_ALIAS;
void TIM1_TRG_COM_IRQHandler(void)    WEAK_ALIAS;
void TIM1_CC_IRQHandler(void)         WEAK_ALIAS;
void TIM2_IRQHandler(void)            WEAK_ALIAS;
void TIM3_IRQHandler(void)            WEAK_ALIAS;
void TIM4_IRQHandler(void)            WEAK_ALIAS;
void I2C1_EV_IRQHandler(void)         WEAK_ALIAS;
void I2C1_ER_IRQHandler(void)         WEAK_ALIAS;
void I2C2_EV_IRQHandler(void)         WEAK_ALIAS;
void I2C2_ER_IRQHandler(void)         WEAK_ALIAS;
void SPI1_IRQHandler(void)            WEAK_ALIAS;
void SPI2_IRQHandler(void)            WEAK_ALIAS;
void USART1_IRQHandler(void)          WEAK_ALIAS;
void USART2_IRQHandler(void)          WEAK_ALIAS;
void USART3_IRQHandler(void)          WEAK_ALIAS;
void EXTI15_10_IRQHandler(void)       WEAK_ALIAS;
void RTCAlarm_IRQHandler(void)        WEAK_ALIAS;
void CEC_IRQHandler(void)             WEAK_ALIAS;
void TIM6_IRQHandler(void)            WEAK_ALIAS;
void TIM7_IRQHandler(void)            WEAK_ALIAS;

/*
 * The vector table itself.
 *
 * It is an array of "pointers", but entry 0 is actually the initial stack
 * pointer VALUE, not a function. We therefore store everything as a
 * uint32_t-compatible pointer. The cast (void (*)(void)) for _estack just
 * reuses the slot; the CPU reads it as a raw 32-bit value.
 *
 * The section attribute ".isr_vector" matches the KEEP() rule in
 * linker.ld that forces this table to address 0x08000000.
 */
__attribute__((section(".isr_vector"), used))
void (* const g_vector_table[])(void) =
{
    (void (*)(void))(&_estack),  /*   0: Initial Main Stack Pointer       */
    Reset_Handler,               /*   1: Reset                            */
    NMI_Handler,                 /*   2: Non-maskable interrupt           */
    HardFault_Handler,           /*   3: Hard fault                       */
    MemManage_Handler,           /*   4: Memory management fault          */
    BusFault_Handler,            /*   5: Bus fault                        */
    UsageFault_Handler,          /*   6: Usage fault                      */
    0, 0, 0, 0,                  /* 7-10: Reserved                        */
    SVC_Handler,                 /*  11: Supervisor call (SVC)            */
    DebugMon_Handler,            /*  12: Debug monitor                    */
    0,                           /*  13: Reserved                         */
    PendSV_Handler,              /*  14: Pendable service request         */
    SysTick_Handler,             /*  15: System tick timer                */

    /* External interrupts (IRQ0..) - RM0041 Table 51 ------------------ */
    WWDG_IRQHandler,             /*  16: IRQ0  Window watchdog            */
    PVD_IRQHandler,              /*  17: IRQ1  PVD via EXTI16             */
    TAMPER_IRQHandler,           /*  18: IRQ2  Tamper                     */
    RTC_IRQHandler,              /*  19: IRQ3  RTC global                 */
    FLASH_IRQHandler,            /*  20: IRQ4  Flash                      */
    RCC_IRQHandler,              /*  21: IRQ5  RCC                        */
    EXTI0_IRQHandler,            /*  22: IRQ6  EXTI line 0                */
    EXTI1_IRQHandler,            /*  23: IRQ7  EXTI line 1                */
    EXTI2_IRQHandler,            /*  24: IRQ8  EXTI line 2                */
    EXTI3_IRQHandler,            /*  25: IRQ9  EXTI line 3                */
    EXTI4_IRQHandler,            /*  26: IRQ10 EXTI line 4                */
    DMA1_Channel1_IRQHandler,    /*  27: IRQ11 DMA1 channel 1             */
    DMA1_Channel2_IRQHandler,    /*  28: IRQ12 DMA1 channel 2             */
    DMA1_Channel3_IRQHandler,    /*  29: IRQ13 DMA1 channel 3             */
    DMA1_Channel4_IRQHandler,    /*  30: IRQ14 DMA1 channel 4 (USART1_TX) */
    DMA1_Channel5_IRQHandler,    /*  31: IRQ15 DMA1 channel 5             */
    DMA1_Channel6_IRQHandler,    /*  32: IRQ16 DMA1 channel 6             */
    DMA1_Channel7_IRQHandler,    /*  33: IRQ17 DMA1 channel 7             */
    ADC1_IRQHandler,             /*  34: IRQ18 ADC1                       */
    0, 0, 0, 0,                  /*  35-38: IRQ19-22 Reserved (USB/CAN)   */
    EXTI9_5_IRQHandler,          /*  39: IRQ23 EXTI lines 9..5            */
    TIM1_BRK_IRQHandler,         /*  40: IRQ24 TIM1 break                 */
    TIM1_UP_IRQHandler,          /*  41: IRQ25 TIM1 update                */
    TIM1_TRG_COM_IRQHandler,     /*  42: IRQ26 TIM1 trigger/commutation   */
    TIM1_CC_IRQHandler,          /*  43: IRQ27 TIM1 capture/compare       */
    TIM2_IRQHandler,             /*  44: IRQ28 TIM2                       */
    TIM3_IRQHandler,             /*  45: IRQ29 TIM3                       */
    TIM4_IRQHandler,             /*  46: IRQ30 TIM4                       */
    I2C1_EV_IRQHandler,          /*  47: IRQ31 I2C1 event                 */
    I2C1_ER_IRQHandler,          /*  48: IRQ32 I2C1 error                 */
    I2C2_EV_IRQHandler,          /*  49: IRQ33 I2C2 event                 */
    I2C2_ER_IRQHandler,          /*  50: IRQ34 I2C2 error                 */
    SPI1_IRQHandler,             /*  51: IRQ35 SPI1                       */
    SPI2_IRQHandler,             /*  52: IRQ36 SPI2                       */
    USART1_IRQHandler,           /*  53: IRQ37 USART1                     */
    USART2_IRQHandler,           /*  54: IRQ38 USART2                     */
    USART3_IRQHandler,           /*  55: IRQ39 USART3                     */
    EXTI15_10_IRQHandler,        /*  56: IRQ40 EXTI lines 15..10          */
    RTCAlarm_IRQHandler,         /*  57: IRQ41 RTC alarm via EXTI17       */
    CEC_IRQHandler,              /*  58: IRQ42 HDMI-CEC                   */
    0, 0, 0, 0, 0,               /*  59-63: IRQ43-47 Reserved             */
    0, 0, 0, 0, 0,               /*  64-68: IRQ48-52 Reserved             */
    TIM6_IRQHandler,             /*  69: IRQ53 TIM6                       */
    TIM7_IRQHandler              /*  70: IRQ54 TIM7                       */
};

/*
 * Reset_Handler: the first C code to run after reset.
 *
 * Steps (the classic bare-metal "C runtime startup"):
 *   1. Copy the .data initialisation image from Flash to RAM.
 *   2. Zero the .bss section in RAM.
 *   3. Call main().
 *   4. If main() ever returns, trap forever (there is no OS to return to).
 */
void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    /* 1. Copy initialised data: Flash image (_sidata) -> RAM (_sdata.._edata) */
    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; )
    {
        *dst = *src;
        dst++;
        src++;
    }

    /* 2. Zero-initialise the .bss section. */
    for (dst = &_sbss; dst < &_ebss; )
    {
        *dst = 0U;
        dst++;
    }

    /* 3. Hand control to the student's program. */
    (void)main();

    /* 4. Should never get here on bare metal. */
    for (;;)
    {
        /* trap */
    }
}

/*
 * Default_Handler: catch-all for unexpected interrupts. Spin forever so a
 * debugger (or QEMU's -d int trace) shows exactly where execution stuck.
 */
void Default_Handler(void)
{
    for (;;)
    {
        /* unexpected interrupt - trap here */
    }
}
