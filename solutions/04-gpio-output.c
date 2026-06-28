/*
 * solutions/04-gpio-output.c  -  REFERENCE SOLUTION (Module 04: GPIO Output)
 * =========================================================================
 *
 * Goal: blink the green LED LD3 on the STM32VLDISCOVERY. LD3 is wired to
 * port C, pin 9 (PC9). We toggle it forever with a crude software delay.
 *
 * Board fact: LD3 (green) = PC9, LD4 (blue) = PC8.
 *
 * IMPORTANT - what you will see in QEMU:
 *   QEMU's stm32vldiscovery machine does NOT emulate GPIO. The writes below
 *   are accepted but nothing lights up, and reading the registers back
 *   returns 0. So this program is verified by REASONING about the bit math
 *   (and by running on real hardware). It will build and run in QEMU
 *   without faulting; visible output begins in Module 06 (UART).
 *
 * STM32F1 GPIO reminder: this family configures pins with CRL (pins 0-7)
 * and CRH (pins 8-15), 4 bits per pin = MODE[1:0] + CNF[1:0]. There is NO
 * MODER register on STM32F100 (that is an F0/F3/F4 feature).
 *
 * Registers used (RM0041 sec.7 RCC, sec.8 GPIO):
 *   RCC_APB2ENR (0x40021018)  bit4 IOPCEN  -> clock to GPIO port C
 *   GPIOC_CRH   (0x40011004)  pin9 nibble  -> configure PC9 as output
 *   GPIOC_BSRR  (0x40011010)  atomic set (bit9) / reset (bit9+16)
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width
 * types, no dynamic memory, explicit masks.
 */

#include <stdint.h>

/* ---- Register definitions (volatile: the compiler must not cache them) -- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define GPIOC_CRH     (*(volatile uint32_t *)0x40011004U)
#define GPIOC_BSRR    (*(volatile uint32_t *)0x40011010U)

/* ---- Bit positions ------------------------------------------------------ */
#define RCC_IOPCEN    (1U << 4)   /* enable clock for GPIO port C            */
#define LED_PIN       9U          /* LD3 green LED is on PC9                 */

/*
 * PC9 lives in CRH (pins 8-15). Pin 9 occupies bits [7:4] of CRH:
 *   bit offset = (pin - 8) * 4 = (9 - 8) * 4 = 4.
 * We want: MODE = 0b10 (output, 2 MHz), CNF = 0b00 (push-pull).
 * Nibble value = (CNF << 2) | MODE = (0b00 << 2) | 0b10 = 0b0010 = 0x2.
 */
#define PC9_CRH_SHIFT (((LED_PIN) - 8U) * 4U)        /* = 4                  */
#define PC9_CRH_MASK  (0xFU << PC9_CRH_SHIFT)        /* clear pin 9 nibble   */
#define PC9_CRH_OUT   (0x2U << PC9_CRH_SHIFT)        /* output PP 2 MHz      */

/* Crude busy-wait. 'volatile' on the counter stops the optimiser deleting
 * the empty loop. Not accurate timing - we get real timing in Module 08. */
static void delay(volatile uint32_t count)
{
    while (count > 0U)
    {
        count--;
    }
}

int main(void)
{
    /* 1. Enable the clock to GPIO port C. A peripheral with no clock
     *    ignores all register writes, so this must come first. */
    RCC_APB2ENR |= RCC_IOPCEN;

    /* 2. Configure PC9 as a push-pull output. Read-modify-write: clear the
     *    pin's 4-bit field, then OR in our configuration. */
    GPIOC_CRH = (GPIOC_CRH & ~PC9_CRH_MASK) | PC9_CRH_OUT;

    /* 3. Blink forever using BSRR (Bit Set/Reset Register), which is atomic:
     *    writing bit n sets the pin, writing bit (n+16) clears it. Using
     *    BSRR avoids a read-modify-write race on ODR. */
    for (;;)
    {
        GPIOC_BSRR = (1U << LED_PIN);          /* PC9 = 1 (LED on)  */
        delay(200000U);
        GPIOC_BSRR = (1U << (LED_PIN + 16U));  /* PC9 = 0 (LED off) */
        delay(200000U);
    }

    /* not reached */
    return 0;
}
