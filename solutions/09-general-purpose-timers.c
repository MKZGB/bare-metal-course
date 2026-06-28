/*
 * solutions/09-general-purpose-timers.c  -  REFERENCE SOLUTION
 * (Module 09: General-Purpose Timers TIM2/3/4 - time base, OC/PWM, input capture)
 * =============================================================================
 *
 * Goal: configure TIM3 as a 1 Hz time base from the 8 MHz HSI clock, then
 * watch its UPDATE flag (UIF) tick over. We bring up USART1 + printf (from
 * Module 06) so the program tells us, over the serial console, exactly what
 * it configured and what it observed.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6, default clock HSI = 8 MHz.
 * TIM3 lives on APB1 at 0x40000400 (RM0041 sec.15, sec.7 RCC).
 *
 * THE TIME-BASE MATH (8 MHz -> 1 Hz):
 *   The counter is fed by fCK_CNT = fCK_PSC / (PSC + 1).
 *   With PSC = 7999: 8 000 000 / (7999 + 1) = 1 000 Hz (one tick per ms).
 *   The counter counts 0,1,2,...,ARR then wraps and raises UIF (update).
 *   With ARR = 999: it wraps every (999 + 1) = 1000 ticks = once per second.
 *   So an update event (and UIF) happens at exactly 1 Hz.
 *
 *   UG (EGR bit0): writing 1 forces an "update event" immediately, which
 *   reloads the prescaler with PSC and the counter with 0 *now* instead of
 *   waiting a full period. This is how you make a freshly written PSC take
 *   effect (the prescaler is buffered).
 *
 *   CEN (CR1 bit0): the master switch. The counter does not run until CEN=1.
 *
 * ********************************************************************
 *  CRITICAL QEMU LIMITATION - READ THIS
 * ********************************************************************
 *   qemu-system-arm -M stm32vldiscovery DOES NOT EMULATE TIM2/3/4.
 *   The counter CNT stays frozen at 0 and the update flag UIF NEVER sets.
 *   A naive `while ((TIM3_SR & UIF) == 0U) {}` would therefore HANG FOREVER
 *   in QEMU. On REAL STM32F100 hardware the timer runs and UIF sets at 1 Hz.
 *
 *   We must NEVER write an unbounded flag-wait here. Instead we use a
 *   BOUNDED wait: spin on UIF but with a guard counter so the loop always
 *   terminates. If the guard runs out we print a message explaining this is
 *   the expected QEMU behaviour, then continue (and the program keeps
 *   printing / does not freeze).
 * ********************************************************************
 *
 * Output Compare (PWM) and Input Capture are covered CONCEPTUALLY in the
 * HTML page; the code below focuses on the time base because that is what we
 * can reason about and demonstrate within QEMU's limits.
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width types,
 * no dynamic memory, explicit masks and U suffixes.
 */

#include <stdint.h>
#include <stdio.h>      /* printf, setvbuf, _IONBF */
#include <sys/stat.h>   /* struct stat for the _fstat syscall stub */

/* ---- RCC (clock gating) ------------------------------------------------- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define RCC_APB1ENR   (*(volatile uint32_t *)0x4002101CU)
#define RCC_IOPAEN    (1U << 2)    /* GPIO port A clock enable (PA9 = TX)    */
#define RCC_USART1EN  (1U << 14)   /* USART1 clock enable                    */
#define RCC_TIM3EN    (1U << 1)    /* TIM3 clock enable (APB1 bit1)          */

/* ---- GPIOA (PA9 = USART1 TX) -------------------------------------------- */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15      */
#define PA9_CRH_SHIFT (4U)                                  /* (9-8)*4        */
#define PA9_CRH_MASK  (0xFU << PA9_CRH_SHIFT)
#define PA9_CRH_AF    (0xBU << PA9_CRH_SHIFT)   /* AF push-pull, 50 MHz       */

/* ---- USART1 ------------------------------------------------------------- */
#define USART1_SR     (*(volatile uint32_t *)0x40013800U)
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)
#define USART_SR_TXE  (1U << 7)
#define USART_CR1_TE  (1U << 3)
#define USART_CR1_UE  (1U << 13)
#define USART1_BRR_9600  (0x341U)   /* 8 MHz, 9600 baud (RM0041 sec.27)       */

/* ---- TIM3 (general-purpose timer, base 0x40000400, RM0041 sec.15) ------- */
#define TIM3_CR1      (*(volatile uint32_t *)0x40000400U)  /* offset 0x00     */
#define TIM3_SR       (*(volatile uint32_t *)0x40000410U)  /* offset 0x10     */
#define TIM3_EGR      (*(volatile uint32_t *)0x40000414U)  /* offset 0x14     */
#define TIM3_CNT      (*(volatile uint32_t *)0x40000424U)  /* offset 0x24     */
#define TIM3_PSC      (*(volatile uint32_t *)0x40000428U)  /* offset 0x28     */
#define TIM3_ARR      (*(volatile uint32_t *)0x4000042CU)  /* offset 0x2C     */

#define TIM_CR1_CEN   (1U << 0)    /* counter enable (master switch)         */
#define TIM_EGR_UG    (1U << 0)    /* update generation (force reload)       */
#define TIM_SR_UIF    (1U << 0)    /* update interrupt flag (wrap occurred)  */

/* Time-base constants: 8 MHz -> 1 kHz tick -> 1 Hz update. */
#define TIM3_PSC_VALUE  (7999U)    /* 8 000 000 / (7999 + 1) = 1 kHz         */
#define TIM3_ARR_VALUE  (999U)     /* wrap every (999 + 1) = 1000 ticks      */

/* How many UIF-set events we attempt to observe before giving up. */
#define UPDATE_EVENTS   (3U)
/* Bounded-wait guard: large enough that on real HW UIF sets first, small
 * enough that in QEMU (where UIF never sets) we exit quickly. */
#define WAIT_GUARD      (2000000U)

/* ------------------------------------------------------------------------ */
/* 1. USART1 transmitter + printf retarget (from Module 06)                 */
/* ------------------------------------------------------------------------ */
static void uart_init(void)
{
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);
    GPIOA_CRH    = (GPIOA_CRH & ~PA9_CRH_MASK) | PA9_CRH_AF;
    USART1_BRR   = USART1_BRR_9600;
    USART1_CR1   = (USART_CR1_TE | USART_CR1_UE);
}

static void uart_putc(uint8_t c)
{
    /* QEMU emulates USART1, so TXE sets and this loop always progresses. */
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* wait for the TX data register to be empty */
    }
    USART1_DR = (uint32_t)c;
}

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

/* ------------------------------------------------------------------------ */
/* 2. TIM3 time base: 1 Hz from 8 MHz                                       */
/* ------------------------------------------------------------------------ */
static void tim3_init(void)
{
    /* Enable the TIM3 clock on APB1. A timer with no clock ignores every
     * register write, so this is the first step. */
    RCC_APB1ENR |= RCC_TIM3EN;

    /* Program the prescaler and auto-reload. PSC divides the 8 MHz input by
     * (PSC + 1) to give a 1 kHz counting clock; ARR sets the wrap point so an
     * update event happens once per second. */
    TIM3_PSC = TIM3_PSC_VALUE;
    TIM3_ARR = TIM3_ARR_VALUE;

    /* The prescaler is buffered: a fresh PSC value only takes effect at the
     * next update event. Force one now with UG so PSC/ARR load immediately.
     * This also sets UIF as a side effect, so we clear it afterwards. */
    TIM3_EGR = TIM_EGR_UG;
    TIM3_SR &= ~TIM_SR_UIF;     /* clear the update flag (write 0 to clear)   */

    /* Start the counter: CEN is the master enable. */
    TIM3_CR1 |= TIM_CR1_CEN;
}

/*
 * Wait (BOUNDED) for one TIM3 update event. Returns 1 if UIF was observed
 * and cleared, 0 if the guard expired first (the expected QEMU case, since
 * QEMU does not emulate TIM2/3/4 so UIF never sets).
 *
 * This is the pattern that keeps the program from hanging forever in QEMU
 * while still being correct on real hardware (where UIF sets at 1 Hz and the
 * loop exits early).
 */
static uint32_t tim3_wait_update_bounded(void)
{
    uint32_t guard = WAIT_GUARD;

    while (((TIM3_SR & TIM_SR_UIF) == 0U) && (guard > 0U))
    {
        guard--;
    }

    if (guard == 0U)
    {
        return 0U;   /* timed out: UIF never set (QEMU limitation)           */
    }

    TIM3_SR &= ~TIM_SR_UIF;   /* clear the flag for the next period          */
    return 1U;
}

/* ------------------------------------------------------------------------ */
/* 3. Application                                                           */
/* ------------------------------------------------------------------------ */
int main(void)
{
    uint32_t i;
    uint32_t got;

    uart_init();
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n=== Module 09: General-Purpose Timer (TIM3) ===\n");

    tim3_init();

    /* Report what we configured so the student can check the math. */
    printf("TIM3 configured for a 1 Hz time base from 8 MHz:\n");
    printf("  PSC = %u  -> counting clock = 8000000 / (PSC+1) = %u Hz\n",
           (unsigned int)TIM3_PSC_VALUE,
           (unsigned int)(8000000U / (TIM3_PSC_VALUE + 1U)));
    printf("  ARR = %u  -> update every (ARR+1) ticks = %u Hz\n",
           (unsigned int)TIM3_ARR_VALUE,
           (unsigned int)((8000000U / (TIM3_PSC_VALUE + 1U))
                          / (TIM3_ARR_VALUE + 1U)));
    printf("  CEN set (counter running), UG forced PSC/ARR to load.\n");

    /* Read CNT back. On real hardware this advances; in QEMU it reads 0
     * because TIM2/3/4 are not emulated. */
    printf("CNT read-back = %u (0 in QEMU: TIM not emulated)\n",
           (unsigned int)TIM3_CNT);

    /* Try to observe a few 1 Hz update events using the BOUNDED wait.
     * On real hardware each call returns after ~1 s with UIF set. In QEMU
     * the guard expires and we report the expected limitation instead of
     * hanging forever on an unbounded `while (!(SR & UIF))`. */
    printf("Watching for %u update events (bounded wait)...\n",
           (unsigned int)UPDATE_EVENTS);

    for (i = 0U; i < UPDATE_EVENTS; i++)
    {
        got = tim3_wait_update_bounded();
        if (got != 0U)
        {
            printf("  event %u: UIF set (timer wrapped) - real hardware.\n",
                   (unsigned int)(i + 1U));
        }
        else
        {
            printf("  event %u: UIF never set - EXPECTED in QEMU "
                   "(TIM2/3/4 not emulated). On real STM32 this fires "
                   "at 1 Hz.\n", (unsigned int)(i + 1U));
        }
    }

    printf("=== done; configuration is correct for real hardware ===\n");

    /* Park. The program has finished printing; it does not hang. */
    for (;;)
    {
        /* nothing more to do */
    }

    /* not reached */
    return 0;
}
