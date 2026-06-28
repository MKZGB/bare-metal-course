/*
 * solutions/05-gpio-input.c  -  REFERENCE SOLUTION (Module 05: GPIO Input)
 * =========================================================================
 *
 * Goal: read the user button B1 on the STM32VLDISCOVERY and mirror its
 * state onto the green LED LD3. While the button is pressed, light the LED;
 * when it is released, turn the LED off.
 *
 * Board facts:
 *   B1 (user button) = PA0. On the STM32VLDISCOVERY this button is wired
 *     ACTIVE HIGH: an external pull-down holds PA0 low, and pressing the
 *     button connects PA0 to 3.3 V. So IDR bit 0 reads 1 when pressed.
 *   LD3 (green LED)  = PC9. Driving PC9 high lights the LED.
 *
 * IMPORTANT - what you will see in QEMU:
 *   QEMU's stm32vldiscovery machine does NOT emulate GPIO. Writes to
 *   CRL/CRH/ODR/BSRR are dropped, and reads of IDR/ODR/CRL/CRH return 0.
 *   Therefore PA0 (the button) ALWAYS reads 0 in QEMU -> the program thinks
 *   the button is never pressed -> the LED is never turned on. That is the
 *   expected QEMU behaviour, not a bug. The register code below is correct
 *   for real STM32 hardware, where pressing B1 lights LD3.
 *   The program builds and runs without faulting; the first VISIBLE output
 *   in QEMU arrives in Module 06 (UART).
 *
 * STM32F1 GPIO reminder: this family configures pins with CRL (pins 0-7)
 * and CRH (pins 8-15), 4 bits per pin = MODE[1:0] + CNF[1:0]. There is NO
 * MODER register on the STM32F100 (that is an F0/F3/F4 feature).
 *
 * Registers used (RM0041 sec.7 RCC, sec.8 GPIO):
 *   RCC_APB2ENR (0x40021018)  bit2 IOPAEN, bit4 IOPCEN -> clock to ports A,C
 *   GPIOA_CRL   (0x40010800)  pin0 nibble  -> configure PA0 as input
 *   GPIOA_IDR   (0x40010808)  read PA0 (button) state
 *   GPIOC_CRH   (0x40011004)  pin9 nibble  -> configure PC9 as output
 *   GPIOC_BSRR  (0x40011010)  atomic set (bit9) / reset (bit9+16)
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width
 * types, no dynamic memory, explicit masks, U-suffixed literals.
 */

#include <stdint.h>

/* ---- Register definitions (volatile: the compiler must not cache them) -- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)

#define GPIOA_CRL     (*(volatile uint32_t *)0x40010800U)  /* PA0..PA7 config */
#define GPIOA_IDR     (*(volatile uint32_t *)0x40010808U)  /* PA input data   */

#define GPIOC_CRH     (*(volatile uint32_t *)0x40011004U)  /* PC8..PC15 config*/
#define GPIOC_BSRR    (*(volatile uint32_t *)0x40011010U)  /* PC atomic set/rst */

/* ---- RCC enable bits (RCC_APB2ENR, RM0041 sec.7.3.7) -------------------- */
#define RCC_IOPAEN    (1U << 2)   /* enable clock for GPIO port A            */
#define RCC_IOPCEN    (1U << 4)   /* enable clock for GPIO port C            */

/* ---- Pin numbers -------------------------------------------------------- */
#define BTN_PIN       0U          /* B1 user button = PA0                    */
#define LED_PIN       9U          /* LD3 green LED  = PC9                    */

/*
 * PA0 lives in CRL (pins 0-7). Pin 0 occupies bits [3:0] of CRL:
 *   bit offset = pin * 4 = 0.
 * We want an INPUT pin. For input pins the encoding is:
 *   MODE = 0b00  -> input mode
 *   CNF  = 0b01  -> floating input (the reset default for most pins)
 * Nibble value = (CNF << 2) | MODE = (0b01 << 2) | 0b00 = 0b0100 = 0x4.
 *
 * Why floating? The board already provides an external pull-down resistor
 * on PA0, so we do not need (and must not fight it with) an internal pull.
 * Other CNF input options are: 00 = analog, 10 = input with pull-up/down
 * (direction chosen by the ODR bit), 11 = reserved.
 */
#define PA0_CRL_SHIFT (BTN_PIN * 4U)                 /* = 0                   */
#define PA0_CRL_MASK  (0xFU << PA0_CRL_SHIFT)        /* clear pin 0 nibble    */
#define PA0_CRL_IN    (0x4U << PA0_CRL_SHIFT)        /* input floating        */

/*
 * PC9 lives in CRH (pins 8-15). Pin 9 occupies bits [7:4] of CRH:
 *   bit offset = (pin - 8) * 4 = 4.
 * We want: MODE = 0b10 (output, 2 MHz), CNF = 0b00 (push-pull).
 * Nibble value = (CNF << 2) | MODE = (0b00 << 2) | 0b10 = 0b0010 = 0x2.
 */
#define PC9_CRH_SHIFT (((LED_PIN) - 8U) * 4U)        /* = 4                   */
#define PC9_CRH_MASK  (0xFU << PC9_CRH_SHIFT)        /* clear pin 9 nibble    */
#define PC9_CRH_OUT   (0x2U << PC9_CRH_SHIFT)        /* output PP 2 MHz       */

int main(void)
{
    /* 1. Enable the clocks for GPIO ports A (button) and C (LED). A
     *    peripheral with no clock ignores all register reads/writes, so this
     *    must come first. A single read-modify-write OR sets both bits. */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_IOPCEN);

    /* 2. Configure PA0 as a floating input. Read-modify-write: clear the
     *    pin's 4-bit field, then OR in 0x4 (MODE=00 input, CNF=01 floating).
     *    Input floating IS the reset default, but we set it explicitly so the
     *    intent is clear and the code is robust to a non-default start. */
    GPIOA_CRL = (GPIOA_CRL & ~PA0_CRL_MASK) | PA0_CRL_IN;

    /* 3. Configure PC9 as a push-pull output (same as Module 04). */
    GPIOC_CRH = (GPIOC_CRH & ~PC9_CRH_MASK) | PC9_CRH_OUT;

    /* 4. Poll the button forever and mirror it onto the LED.
     *
     *    IDR (Input Data Register) holds the live state of every pin in the
     *    port: bit n = the level on pin n. We do not write IDR; we read it.
     *    (ODR is the OUTPUT register - what WE drive. Do not confuse them.)
     *
     *    B1 is active HIGH on this board, so a set IDR bit 0 means "pressed".
     *    We drive the LED with BSRR, which is atomic: writing bit n sets the
     *    pin, writing bit (n+16) clears it - no read-modify-write race. */
    for (;;)
    {
        uint32_t pressed = (GPIOA_IDR >> BTN_PIN) & 1U;  /* 1 = pressed */

        if (pressed != 0U)
        {
            GPIOC_BSRR = (1U << LED_PIN);          /* PC9 = 1 (LED on)  */
        }
        else
        {
            GPIOC_BSRR = (1U << (LED_PIN + 16U));  /* PC9 = 0 (LED off) */
        }

        /* NOTE on the QEMU bounded-wait rule: this loop has NO blocking
         * "wait until a flag sets" inside it. It reads IDR once per
         * iteration and always makes progress, so it can never hang - even
         * though IDR is frozen at 0 in QEMU. That keeps us inside the course
         * rule of never spinning forever on a flag QEMU will not set.
         *
         * A real driver would also DEBOUNCE the button here: a mechanical
         * switch "bounces" for a few milliseconds when pressed/released,
         * producing a burst of fake 0/1 transitions. Typical fixes: sample
         * the pin, then re-sample after a short delay (or via a timer) and
         * only accept the new state if it is stable. We mention it as a
         * concept; timers arrive in Module 08. */
    }

    /* not reached */
    return 0;
}
