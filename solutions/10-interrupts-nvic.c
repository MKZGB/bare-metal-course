/*
 * solutions/10-interrupts-nvic.c  -  REFERENCE SOLUTION (Module 10: Interrupts / NVIC)
 * ===================================================================================
 *
 * Goal: stop POLLING for received bytes (Module 07) and instead let the
 * hardware INTERRUPT us when a byte arrives. We enable the USART1 receive
 * interrupt (RXNEIE), enable USART1's line in the NVIC (IRQ 37), and write a
 * real Interrupt Service Routine - USART1_IRQHandler() - that reads the byte,
 * echoes it, and bumps a counter. main() does no polling at all; it sleeps in
 * WFI and only wakes to report when the counter has changed.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6 (Cortex-M3), HSI = 8 MHz.
 * References: RM0041 sec.10 (interrupts, Table 51 IRQ numbers), sec.27 (USART),
 * Cortex-M3 programming manual PM0056 (NVIC, exception model).
 *
 * HOW A CORTEX-M3 INTERRUPT WORKS (no manual save/restore needed):
 *   1. A peripheral asserts its interrupt request (here: USART1 sets RXNE,
 *      and because RXNEIE is set the request propagates to the NVIC).
 *   2. The NVIC, if that IRQ is enabled and high enough priority, tells the
 *      core to take the exception.
 *   3. HARDWARE automatically pushes 8 registers (R0-R3, R12, LR, PC, xPSR)
 *      onto the current stack - so a plain C function is a valid handler; you
 *      do NOT write assembly to save context.
 *   4. The core loads the handler address from the VECTOR TABLE entry for that
 *      IRQ (our g_vector_table in common/startup.c) into PC and runs it.
 *   5. On return, hardware pops those 8 registers back. Execution resumes
 *      exactly where it was interrupted.
 *
 * THE NVIC SET-ENABLE REGISTERS (PM0056):
 *   NVIC_ISER0 @ 0xE000E100 enables IRQ 0..31  (write 1 to bit n to enable n)
 *   NVIC_ISER1 @ 0xE000E104 enables IRQ 32..63
 *   To enable IRQ n:  ISER[n / 32] = (1U << (n % 32));
 *   USART1 = IRQ 37  ->  index = 37/32 = 1 (ISER1), bit = 37%32 = 5.
 *   Writing a 0 does nothing (these are SET registers), so there is no
 *   read-modify-write and no race - one store enables exactly one line.
 *
 * WHAT IS DEMONSTRABLE IN QEMU:
 *   QEMU fully emulates the USARTs, including raising the USART1 interrupt when
 *   a byte arrives on stdin. So this RX-interrupt demo really fires in QEMU.
 *   Run it interactively and type, or pipe input with a brief delay so the
 *   bytes arrive AFTER the program has armed the interrupt:
 *       (sleep 1; printf 'AB') | qemu-system-arm -M stm32vldiscovery \
 *           -display none -serial stdio -serial null -serial null \
 *           -kernel build/firmware.elf
 *   The handler then echoes 'A' and 'B' and main() reports them.
 *
 *   (The delay matters: QEMU front-loads instantly-piped stdin and delivers it
 *   before the guest finishes reset, so the bytes can be missed - this affects
 *   even the POLLED receiver in Module 07. A small sleep, or interactive
 *   typing, sidesteps it. SysTick - Module 08 - is the other emulated
 *   interrupt; GPIO/EXTI button presses are NOT emulated, so a real button IRQ
 *   cannot be triggered in QEMU even though the setup below is correct.)
 *
 * Registers used:
 *   RCC_APB2ENR (0x40021018)  bit2 IOPAEN, bit14 USART1EN
 *   GPIOA_CRH   (0x40010804)  PA9 = 0xB (AF PP TX), PA10 = 0x4 (input flt RX)
 *   USART1_SR   (0x40013800)  bit5 RXNE, bit7 TXE
 *   USART1_DR   (0x40013804)  data
 *   USART1_BRR  (0x40013808)  0x0341 = 9600 @ 8 MHz
 *   USART1_CR1  (0x4001380C)  bit2 RE, bit3 TE, bit5 RXNEIE, bit13 UE
 *   NVIC_ISER1  (0xE000E104)  set-enable for IRQ 32..63
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width types,
 * no dynamic memory, explicit masks, U-suffixed literals.
 */

#include <stdint.h>
#include <stdio.h>      /* printf, setvbuf, _IONBF */

/* ---- RCC (clock gating) ------------------------------------------------- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define RCC_IOPAEN    (1U << 2)     /* GPIO port A clock enable               */
#define RCC_USART1EN  (1U << 14)    /* USART1 clock enable                    */

/* ---- GPIOA (PA9 = USART1 TX, PA10 = USART1 RX) -------------------------- */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15      */
#define PA9_CRH_SHIFT   (4U)
#define PA10_CRH_SHIFT  (8U)
#define PA9_CRH_MASK    (0xFU << PA9_CRH_SHIFT)
#define PA10_CRH_MASK   (0xFU << PA10_CRH_SHIFT)
#define PA9_CRH_AF_PP   (0xBU << PA9_CRH_SHIFT)   /* AF push-pull 50 MHz (TX) */
#define PA10_CRH_IN_FLT (0x4U << PA10_CRH_SHIFT)  /* input floating     (RX)  */

/* ---- USART1 ------------------------------------------------------------- */
#define USART1_SR     (*(volatile uint32_t *)0x40013800U)
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)

#define USART_SR_RXNE   (1U << 5)   /* read data register not empty           */
#define USART_SR_TXE    (1U << 7)   /* transmit data register empty           */
#define USART_CR1_RE    (1U << 2)   /* receiver enable                        */
#define USART_CR1_TE    (1U << 3)   /* transmitter enable                     */
#define USART_CR1_RXNEIE (1U << 5)  /* RXNE interrupt enable (the new bit!)   */
#define USART_CR1_UE    (1U << 13)  /* USART enable                           */

#define USART1_BRR_9600 (0x0341U)   /* 9600 baud @ 8 MHz                      */

/* ---- NVIC set-enable registers (Cortex-M3 core, PM0056) ----------------- */
/*
 * NVIC_ISER is an array of 32-bit registers starting at 0xE000E100. Index k
 * covers IRQs (32*k) .. (32*k + 31). We model it as a volatile array so the
 * index/bit math reads naturally.
 */
#define NVIC_ISER ((volatile uint32_t *)0xE000E100U)

/*
 * NVIC_ISPR (Interrupt Set-Pending Register) @ 0xE000E200, same indexing as
 * ISER. Writing 1 to bit n marks IRQ n as pending in software, exactly as if
 * the peripheral had asserted it. We use it once below to cover an edge case
 * in QEMU (see uart_init). On real hardware it is also handy for software-
 * triggered interrupts and self-test.
 */
#define NVIC_ISPR ((volatile uint32_t *)0xE000E200U)

#define USART1_IRQn   (37U)         /* RM0041 Table 51: USART1 = IRQ 37       */

/*
 * Shared state between the ISR and main(). It MUST be volatile: the handler
 * runs "asynchronously" from main()'s point of view, so without volatile the
 * compiler is free to cache g_rx_count in a register inside main()'s loop and
 * never see the handler's updates. volatile forces a fresh memory read each
 * time. (g_last_char is informational only.)
 */
static volatile uint32_t g_rx_count   = 0U;
static volatile uint8_t  g_last_char  = 0U;

/* ------------------------------------------------------------------------ */
/* Small UART helpers (TX path reused from Modules 06/07)                   */
/* ------------------------------------------------------------------------ */
static void uart_putc(uint8_t c)
{
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* QEMU sets TXE, so this terminates */
    }
    USART1_DR = (uint32_t)c;
}

/* ------------------------------------------------------------------------ */
/* nvic_enable_irq - the generic "turn on IRQ n" routine                    */
/* ------------------------------------------------------------------------ */
/*
 * Enable interrupt number 'irqn' in the NVIC. The set-enable registers are
 * indexed by irqn/32, and the bit within that register is irqn%32. Because
 * these are write-1-to-set registers, a single store enables exactly one line
 * with no read-modify-write and no race against an interrupt.
 *   e.g. nvic_enable_irq(37):  index = 1, bit = 5  ->  ISER1 = (1 << 5).
 */
static void nvic_enable_irq(uint32_t irqn)
{
    NVIC_ISER[irqn >> 5U] = (1U << (irqn & 0x1FU));   /* /32 and %32 */
}

/* ------------------------------------------------------------------------ */
/* uart_init - USART1 for TX + RX, with the RX interrupt armed              */
/* ------------------------------------------------------------------------ */
static void uart_init(void)
{
    /* 1. Clock GPIOA (for PA9/PA10) and USART1. */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* 2. PA9 = AF push-pull (TX), PA10 = input floating (RX). */
    GPIOA_CRH = (GPIOA_CRH & ~(PA9_CRH_MASK | PA10_CRH_MASK))
              | PA9_CRH_AF_PP | PA10_CRH_IN_FLT;

    /* 3. Baud rate (written while USART disabled). */
    USART1_BRR = USART1_BRR_9600;

    /*
     * 4. Enable TX, RX, the RXNE interrupt source, and the USART.
     *    RXNEIE is the key step for THIS module: it tells the USART to raise
     *    its interrupt request line whenever RXNE becomes 1. Without it, RXNE
     *    still sets but no interrupt is generated (that was Module 07's poll).
     */
    USART1_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    /*
     * 5. Enable USART1's line in the NVIC. Up to now the request would reach
     *    the NVIC but be masked. IRQ 37 -> ISER1 bit 5.
     */
    nvic_enable_irq(USART1_IRQn);

    /* Interrupts are enabled globally out of reset (PRIMASK = 0), so nothing
     * else is needed; a byte on RX now triggers USART1_IRQHandler(). */

    /*
     * QEMU edge case (and a real-hardware corner case too): if a byte is
     * ALREADY waiting (RXNE = 1) at the moment we arm RXNEIE/NVIC, no fresh
     * "RXNE became set" edge occurs, so the interrupt may not be raised for
     * that already-present byte. QEMU front-loads piped stdin, so this is the
     * normal situation under `printf 'AB' | qemu...`. We cover it by software-
     * pending the USART1 IRQ via NVIC_ISPR: the handler then runs, reads DR,
     * and drains the byte. After that, each NEW byte raises the interrupt
     * normally. (Harmless on real hardware: if RXNE is clear the handler's
     * RXNE check simply does nothing.)
     */
    if ((USART1_SR & USART_SR_RXNE) != 0U)
    {
        NVIC_ISPR[USART1_IRQn >> 5U] = (1U << (USART1_IRQn & 0x1FU));
    }
}

/* ------------------------------------------------------------------------ */
/* The Interrupt Service Routine                                            */
/* ------------------------------------------------------------------------ */
/*
 * USART1_IRQHandler - runs when USART1 raises its interrupt. Its name matches
 * the WEAK symbol in common/startup.c, so the linker wires this strong
 * definition into vector-table slot 53 (IRQ 37). The hardware has already
 * saved our context, so this is just an ordinary C function.
 *
 * What it does:
 *   - Check RXNE: confirm a received byte is the cause.
 *   - READ DR: this returns the byte AND clears RXNE, which de-asserts the
 *     interrupt request. (Forgetting to clear the source is the classic
 *     interrupt bug: the handler would re-fire forever.)
 *   - Echo the byte and update the shared, volatile state for main().
 *
 * Keep handlers SHORT: do the minimum (grab the byte, set a flag) and let the
 * main loop do heavier work. We echo here only because it makes the demo
 * visible in QEMU.
 */
void USART1_IRQHandler(void);
void USART1_IRQHandler(void)
{
    /*
     * Drain every byte currently available. Normally one interrupt = one byte,
     * but draining in a `while` is robust: if several bytes are already queued
     * (e.g. QEMU delivered piped input in a burst), we handle them all in one
     * entry rather than relying on one interrupt per byte.
     */
    while ((USART1_SR & USART_SR_RXNE) != 0U)
    {
        uint8_t c = (uint8_t)(USART1_DR & 0xFFU);  /* read DR clears RXNE */

        uart_putc(c);                              /* echo it back        */
        if (c == (uint8_t)'\r')
        {
            uart_putc((uint8_t)'\n');
        }

        g_last_char = c;
        g_rx_count++;                              /* tell main() about it */
    }
}

/* ------------------------------------------------------------------------ */
/* printf retarget (so the banner can use printf)                           */
/* ------------------------------------------------------------------------ */
int _write(int fd, const char *buf, int len);
int _write(int fd, const char *buf, int len)
{
    int i;

    (void)fd;
    for (i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
        {
            uart_putc((uint8_t)'\r');
        }
        uart_putc((uint8_t)buf[i]);
    }
    return len;
}

/* ------------------------------------------------------------------------ */
/* Application                                                              */
/* ------------------------------------------------------------------------ */
int main(void)
{
    uint32_t seen = 0U;

    uart_init();
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n=== Module 10: USART1 RX interrupt (NVIC IRQ 37) ===\n");
    printf("Send characters; the ISR echoes them. main() does NOT poll DR.\n");

    /*
     * main() never touches the USART. The RX interrupt does ALL the receiving
     * in the background; main() merely watches the volatile counter the ISR
     * updates and reports each new byte. This is the whole point of interrupts:
     * the foreground code is fully decoupled from the device.
     *
     * Note the deliberate absence of any flag-WAIT here: main() does not spin
     * on RXNE. It is therefore free to do other work; we just report. (On real
     * hardware you would typically execute `__asm volatile("wfi")` at the top
     * of the loop to sleep between interrupts and save power. We omit WFI under
     * QEMU because, once piped input is exhausted, no further interrupt would
     * arrive to wake the core and the program would appear to freeze.)
     *
     * This loop runs forever, exactly like the parked loops in Modules 06/07.
     * Under the timed test command (`timeout 4 qemu...`) QEMU is simply stopped
     * by the timeout after the bytes have been echoed - that is expected and is
     * not a hang/fault.
     */
    for (;;)
    {
        if (g_rx_count != seen)
        {
            seen = g_rx_count;
            printf("[main] byte #%u handled by ISR: 0x%02X\n",
                   (unsigned int)seen, (unsigned int)g_last_char);
        }
    }

    /* not reached */
}

/* Minimal newlib stubs to keep the link warning-clean (see Module 06). */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, void *st)            { (void)fd; (void)st; return 0; }
