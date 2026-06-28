/*
 * solutions/06-uart-transmit.c  -  REFERENCE SOLUTION (Module 06: UART Transmit)
 * =============================================================================
 *
 * Goal: bring up USART1 as a transmitter and get the FIRST VISIBLE OUTPUT of
 * the whole course. We build a tiny polled TX driver (uart_putc / uart_puts),
 * then retarget the C library's printf() so it sends characters to USART1.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6, default clock HSI = 8 MHz.
 * USART1 TX is on PA9 (alternate-function push-pull). RM0041 sec.27 (USART),
 * sec.7 (RCC), sec.8 (GPIO).
 *
 * WHAT YOU WILL SEE IN QEMU (good news for once):
 *   USART1 *is* fully emulated by qemu-system-arm -M stm32vldiscovery. Bytes
 *   written to USART1_DR appear on the host terminal (the -serial stdio
 *   backend). So unlike the GPIO modules, this program produces REAL,
 *   visible output. QEMU ignores BRR (no actual baud timing) but we still
 *   program it correctly for real hardware.
 *
 *   QEMU SERIAL GOTCHA: the machine has THREE USARTs, and QEMU requires a
 *   backend for each. A bare "-serial stdio" fails with
 *       "cannot use stdio by multiple character devices"
 *   so the run command gives all three a backend (the shared Makefile's
 *   `run` target already does this):
 *       qemu-system-arm -M stm32vldiscovery -display none \
 *           -serial stdio -serial null -serial null -kernel build/firmware.elf
 *   USART1 is the first -serial (stdio). Quit QEMU with Ctrl-A then X.
 *
 * Registers used (USART1 base 0x40013800):
 *   USART1_SR  (0x40013800)  bit7 TXE (TX data reg empty), bit6 TC (complete)
 *   USART1_DR  (0x40013804)  data register (write a byte to transmit)
 *   USART1_BRR (0x40013808)  baud rate: 8 MHz / (16 * USARTDIV) = 9600 -> 0x341
 *   USART1_CR1 (0x4001380C)  bit13 UE (USART enable), bit3 TE (transmit enable)
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width types,
 * no dynamic memory, explicit masks and U suffixes.
 */

#include <stdint.h>
#include <stdio.h>      /* printf, setvbuf, _IONBF */
#include <sys/stat.h>   /* struct stat for the _fstat syscall stub */

/* ---- RCC (clock gating) ------------------------------------------------- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define RCC_IOPAEN    (1U << 2)    /* GPIO port A clock enable               */
#define RCC_USART1EN  (1U << 14)   /* USART1 clock enable                    */

/* ---- GPIOA (PA9 = USART1 TX) -------------------------------------------- */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15      */

/*
 * PA9 lives in CRH (pins 8-15). Pin 9 occupies bits [7:4]:
 *   shift = (9 - 8) * 4 = 4.
 * We want alternate-function push-pull output at 50 MHz so the USART
 * peripheral (not the GPIO ODR) drives the pin:
 *   MODE = 0b11 (output, 50 MHz), CNF = 0b10 (AF push-pull).
 *   nibble = (CNF << 2) | MODE = (0b10 << 2) | 0b11 = 0b1011 = 0xB.
 */
#define PA9_CRH_SHIFT (4U)
#define PA9_CRH_MASK  (0xFU << PA9_CRH_SHIFT)   /* clear pin-9 nibble        */
#define PA9_CRH_AF    (0xBU << PA9_CRH_SHIFT)   /* AF push-pull, 50 MHz      */

/* ---- USART1 ------------------------------------------------------------- */
#define USART1_SR     (*(volatile uint32_t *)0x40013800U)
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)

#define USART_SR_TXE  (1U << 7)    /* transmit data register empty           */
#define USART_SR_TC   (1U << 6)    /* transmission complete                  */
#define USART_CR1_TE  (1U << 3)    /* transmitter enable                     */
#define USART_CR1_UE  (1U << 13)   /* USART enable                           */

/*
 * Baud rate divisor for 9600 baud from an 8 MHz peripheral clock:
 *   USARTDIV = fck / (16 * baud) = 8000000 / (16 * 9600) = 52.0833...
 *   mantissa = 52 = 0x34  -> BRR[15:4]
 *   fraction = round(0.0833 * 16) = 1 = 0x1 -> BRR[3:0]
 *   BRR = 0x341.
 */
#define USART1_BRR_9600  (0x341U)

/* ------------------------------------------------------------------------ */
/* 1. Driver: bring USART1 up as a transmitter                              */
/* ------------------------------------------------------------------------ */
static void uart_init(void)
{
    /* Enable clocks: GPIO port A (for PA9) and USART1 itself. A peripheral
     * with no clock ignores every register write, so this comes first. */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* Configure PA9 as alternate-function push-pull (nibble 0xB) so USART1
     * drives the pin. Read-modify-write so other pins in CRH are untouched. */
    GPIOA_CRH = (GPIOA_CRH & ~PA9_CRH_MASK) | PA9_CRH_AF;

    /* Program the baud rate. QEMU ignores this, but real hardware needs it. */
    USART1_BRR = USART1_BRR_9600;

    /* Enable the transmitter (TE) and the USART as a whole (UE). Order:
     * set TE first, then UE; a single combined write is also fine. */
    USART1_CR1 = (USART_CR1_TE | USART_CR1_UE);
}

/*
 * Send one byte. We POLL the TXE flag: TXE = 1 means the data register has
 * room for the next byte, so we may write DR. QEMU sets TXE, so this loop
 * always makes progress (no QEMU hang here, unlike unemulated peripherals).
 */
static void uart_putc(uint8_t c)
{
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* wait until the TX data register is empty */
    }
    USART1_DR = (uint32_t)c;   /* writing DR launches the transmission */
}

/* Send a NUL-terminated string, byte by byte. */
static void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        uart_putc((uint8_t)*s);
        s++;
    }
}

/* ------------------------------------------------------------------------ */
/* 2. Retarget printf() to USART1                                           */
/* ------------------------------------------------------------------------ */
/*
 * newlib's printf() ultimately calls the low-level syscall _write() to emit
 * characters. The Makefile links --specs=nosys.specs, which provides a stub
 * _write() that does nothing. By defining our OWN _write() we override that
 * stub and route every printf byte to the UART.
 *
 * We also translate '\n' into "\r\n" so a plain terminal advances to the
 * start of the next line (a bare '\n' on a serial console moves down but not
 * left). Return value must be the number of bytes "written".
 *
 * The signature must match newlib's expectation exactly.
 */
int _write(int fd, const char *buf, int len)
{
    int i;

    (void)fd;   /* we send everything to USART1 regardless of fd */

    for (i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
        {
            uart_putc((uint8_t)'\r');   /* carriage return before newline */
        }
        uart_putc((uint8_t)buf[i]);
    }

    return len;
}

/*
 * Minimal stubs for the other low-level syscalls newlib references when it
 * pulls in printf. We never read files, seek, or query a TTY on bare metal,
 * so these just fail safely. Defining them here silences the linker's
 * "X is not implemented and will always fail" warnings from nosys.specs and
 * keeps the build warning-clean. (errno handling omitted on purpose.)
 */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)     { (void)fd; if (st != NULL) { st->st_mode = S_IFCHR; } return 0; }

/* ------------------------------------------------------------------------ */
/* 3. Application                                                           */
/* ------------------------------------------------------------------------ */
int main(void)
{
    uart_init();

    /* Make stdout UNBUFFERED. Two reasons:
     *  - characters appear immediately instead of waiting for a full buffer,
     *  - newlib then needs no heap buffer for stdout, so no malloc/_sbrk is
     *    pulled in. That keeps the build MISRA-clean (no dynamic memory). */
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    /* First, prove the raw driver works without any libc. */
    uart_puts("\r\n=== Module 06: UART Transmit ===\r\n");
    uart_puts("Hello from USART1 (uart_puts)!\r\n");

    /* Now the retargeted printf. newlib-nano printf is INTEGER-ONLY
     * (no %f), which is exactly what we need for register/embedded work. */
    printf("printf works: line on a fresh line.\n");

    int   answer = 42;
    uint32_t reg = 0x40013800U;
    printf("decimal: %d\n", answer);
    printf("hex:     0x%08X\n", (unsigned int)reg);
    printf("both:    %d == 0x%X\n", answer, (unsigned int)answer);

    uart_puts("=== done; UART idle ===\r\n");

    /* Park here. On real hardware you might sleep; in QEMU we simply loop. */
    for (;;)
    {
        /* nothing more to transmit */
    }

    /* not reached */
    return 0;
}
