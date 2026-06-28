/*
 * solutions/08-systick-timer.c  -  REFERENCE SOLUTION (Module 08: SysTick Timer)
 * =============================================================================
 *
 * Goal: build a real millisecond time base using the Cortex-M3 SysTick timer,
 * then print an uptime counter once per second over USART1. Unlike the crude
 * software delay() of Module 04, this gives an accurate, hardware-driven tick.
 *
 * IMPORTANT - what you will see in QEMU:
 *   SysTick is a CORE peripheral of the Cortex-M3, and QEMU's stm32vldiscovery
 *   machine emulates it FULLY. The counter counts down, COUNTFLAG sets, and the
 *   SysTick exception fires. So this module is genuinely observable: you will
 *   see "Uptime: 1 s", "Uptime: 2 s", ... print on the terminal. (USART1 is
 *   also fully emulated, so the printf output is real.)
 *
 * The SysTick timer (PM0056, Cortex-M3 core peripheral) @ 0xE000E010:
 *   SYST_CSR  (0xE000E010)  control/status
 *       bit0  ENABLE    : 1 = counter running
 *       bit1  TICKINT   : 1 = assert SysTick exception when count reaches 0
 *       bit2  CLKSOURCE : 1 = processor clock (HCLK), 0 = external ref clock
 *       bit16 COUNTFLAG : reads 1 if the timer counted to 0 since last read
 *                         (auto-clears on read of CSR)
 *   SYST_RVR  (0xE000E014)  reload value, 24-bit. Loaded into the counter the
 *                           tick AFTER it reaches 0. For a period of N ticks of
 *                           the input clock, write RVR = N - 1.
 *   SYST_CVR  (0xE000E018)  current value. WRITING ANY value clears it (and
 *                           clears COUNTFLAG); the written data itself is
 *                           ignored.
 *
 * Timing math: the default clock after reset is HSI = 8 MHz, and with
 * CLKSOURCE=1 SysTick is fed by that 8 MHz processor clock. We want one
 * interrupt every 1 ms = every 8000 clock ticks, so:
 *       RVR = 8000 - 1 = 7999.
 * The counter counts 7999, 7998, ..., 1, 0 (that is 8000 states) then reloads.
 *
 * Style: no HAL, no CMSIS. Direct register access, fixed-width types, U
 * suffixes and explicit masks (MISRA-aligned).
 */

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>   /* struct stat for the _fstat syscall stub */

/* ====================================================================== */
/* SysTick registers (Cortex-M3 core, PM0056)                             */
/* ====================================================================== */
#define SYST_CSR   (*(volatile uint32_t *)0xE000E010U)  /* control/status   */
#define SYST_RVR   (*(volatile uint32_t *)0xE000E014U)  /* reload value     */
#define SYST_CVR   (*(volatile uint32_t *)0xE000E018U)  /* current value    */

#define CSR_ENABLE     (1U << 0)   /* counter enable                        */
#define CSR_TICKINT    (1U << 1)   /* exception request on count-to-zero    */
#define CSR_CLKSOURCE  (1U << 2)   /* 1 = processor clock                   */
#define CSR_COUNTFLAG  (1U << 16)  /* counted to zero since last CSR read   */

/* One tick = 1 ms at 8 MHz HSI: 8000 input clocks, so reload = 8000 - 1.   */
#define SYSTICK_1MS_RELOAD  7999U

/* ====================================================================== */
/* USART1 registers (RM0041 sec.27) - used only to print results          */
/* ====================================================================== */
#define RCC_APB2ENR  (*(volatile uint32_t *)0x40021018U)
#define GPIOA_CRH    (*(volatile uint32_t *)0x40010804U)
#define USART1_SR    (*(volatile uint32_t *)0x40013800U)
#define USART1_DR    (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR   (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1   (*(volatile uint32_t *)0x4001380CU)

#define RCC_IOPAEN    (1U << 2)    /* GPIO port A clock                     */
#define RCC_USART1EN  (1U << 14)   /* USART1 clock                          */
#define USART_SR_TXE  (1U << 7)    /* TX data register empty                */
#define USART_CR1_TE  (1U << 3)    /* transmitter enable                    */
#define USART_CR1_UE  (1U << 13)   /* USART enable                          */

/* ====================================================================== */
/* Shared tick counter                                                    */
/* ====================================================================== */
/*
 * g_msticks is written by the SysTick_Handler interrupt and read by main().
 * It MUST be 'volatile': it is modified by a context (the ISR) the compiler
 * cannot see from main(). Without volatile, the optimiser is free to cache
 * the value in a register and a delay loop reading it would spin forever on
 * a stale copy. 'volatile' forces a fresh memory load every time.
 */
static volatile uint32_t g_msticks = 0U;

/*
 * SysTick exception handler. Its name matches the WEAK symbol in
 * common/startup.c, so the linker uses THIS strong definition. It fires once
 * per millisecond (every time the counter reaches 0) and just advances the
 * tick count. Reading/clearing COUNTFLAG is not required here - entering the
 * exception already acknowledges it.
 */
void SysTick_Handler(void)
{
    g_msticks++;
}

/* ---------------------------------------------------------------------- */
/* SysTick setup                                                          */
/* ---------------------------------------------------------------------- */
static void systick_init(uint32_t reload)
{
    SYST_RVR = reload & 0x00FFFFFFU;   /* 24-bit reload; mask to be safe    */
    SYST_CVR = 0U;                     /* any write clears the counter      */
    /* Enable: processor clock + interrupt + run. */
    SYST_CSR = CSR_CLKSOURCE | CSR_TICKINT | CSR_ENABLE;
}

/*
 * Block for 'ms' milliseconds by watching the tick count advance. Because
 * g_msticks is volatile, each comparison re-reads it from memory, so the
 * loop exits as soon as the ISR has incremented it enough times.
 */
static void delay_ms(uint32_t ms)
{
    uint32_t start = g_msticks;
    while ((g_msticks - start) < ms)
    {
        /* spin - the SysTick ISR advances g_msticks in the background */
    }
}

/* ---------------------------------------------------------------------- */
/* Minimal UART + printf retarget (see Module 06 for the full treatment)  */
/* ---------------------------------------------------------------------- */
static void uart_init(void)
{
    /* Clock the GPIO port A and USART1 peripherals. */
    RCC_APB2ENR |= RCC_IOPAEN | RCC_USART1EN;

    /* PA9 = USART1 TX: alternate-function push-pull, 50 MHz -> nibble 0xB.
     * PA9 lives in CRH at bits [(9-8)*4 .. ] = bits [7:4]. */
    GPIOA_CRH = (GPIOA_CRH & ~(0xFU << 4)) | (0xBU << 4);

    /* 9600 baud @ 8 MHz: USARTDIV = 8e6 / (16*9600) = 52.0833 -> BRR 0x0341.
     * (QEMU ignores BRR, but set it correctly for real hardware.) */
    USART1_BRR = 0x0341U;

    /* Enable transmitter, then the USART as a whole. */
    USART1_CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void uart_putc(char c)
{
    /* Wait until the TX data register is empty, then write the byte.
     * QEMU sets TXE, so this unbounded wait is safe here. */
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* wait */
    }
    USART1_DR = (uint32_t)((uint8_t)c);
}

/*
 * newlib calls _write() to flush stdout; we route it to USART1. Translate
 * '\n' to CR+LF so terminals show clean line breaks. Returning 'len' tells
 * newlib all bytes were consumed.
 */
int _write(int fd, const char *buf, int len)
{
    int i;
    (void)fd;
    for (i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
        {
            uart_putc('\r');
        }
        uart_putc(buf[i]);
    }
    return len;
}

/*
 * Minimal stubs for the other low-level syscalls newlib references when it
 * pulls in printf. We never read files, seek, or query a TTY on bare metal,
 * so these just fail safely. Defining them here silences the linker's
 * "X is not implemented and will always fail" warnings from nosys.specs.
 */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)     { (void)fd; if (st != NULL) { st->st_mode = S_IFCHR; } return 0; }

int main(void)
{
    uint32_t seconds = 0U;

    uart_init();
    /* Unbuffered stdout: printf goes straight to _write with no heap/malloc. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Start the 1 ms tick. From here g_msticks advances in the background. */
    systick_init(SYSTICK_1MS_RELOAD);

    printf("SysTick started: 1 ms tick (RVR=%u @ 8 MHz)\n",
           (unsigned)SYSTICK_1MS_RELOAD);

    for (;;)
    {
        delay_ms(1000U);   /* wait one second, measured by SysTick */
        seconds++;
        printf("Uptime: %u s\n", (unsigned)seconds);
    }

    /* not reached */
    return 0;
}
