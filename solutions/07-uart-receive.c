/*
 * solutions/07-uart-receive.c  -  REFERENCE SOLUTION (Module 07: UART Receive)
 * ===========================================================================
 *
 * Goal: receive characters over USART1 and echo them straight back. This is
 * the classic serial "echo" loop: whatever you type in the terminal is sent
 * back out, so you SEE it appear. It proves that both the receive path (RX)
 * and the transmit path (TX, reused from Module 06) work.
 *
 * Wiring on the STM32VLDISCOVERY:
 *   USART1 TX = PA9  (alternate-function push-pull, CRH nibble 0xB)
 *   USART1 RX = PA10 (input floating,                CRH nibble 0x4)
 *
 * IMPORTANT - what you will see in QEMU (this one IS testable!):
 *   QEMU's stm32vldiscovery FULLY emulates the USARTs. With the run command
 *   "-serial stdio -serial null -serial null", USART1 is connected to your
 *   terminal:
 *     - bytes the program writes to DR appear on stdout (you read them);
 *     - bytes you type on the keyboard (stdin) arrive at USART1 RX and set
 *       the RXNE flag, so the program can read them from DR.
 *   So echo really works in QEMU. Run interactively and type, or pipe input:
 *       printf 'hello\r' | qemu-system-arm ... -kernel build/firmware.elf
 *   (Unlike GPIO/TIM/I2C/DMA, no QEMU bounded-wait workaround is needed here:
 *    QEMU genuinely sets TXE and RXNE, so plain flag waits terminate.)
 *
 * RXNE (Read Data Register Not Empty), SR bit 5:
 *   The USART receiver assembles incoming bits into a byte. When a complete
 *   byte is ready in the data register, hardware sets RXNE. READING DR clears
 *   RXNE automatically - that single read is the acknowledgement. We poll
 *   RXNE in a loop; this is "polled" (programmed I/O) receive. The
 *   interrupt-driven alternative (RXNEIE + the USART1 IRQ) arrives in
 *   Module 10.
 *
 * Registers used (RM0041 sec.7 RCC, sec.8 GPIO, sec.27 USART):
 *   RCC_APB2ENR (0x40021018)  bit2 IOPAEN, bit14 USART1EN -> clocks
 *   GPIOA_CRH   (0x40010804)  PA9 nibble 0xB (AF PP), PA10 nibble 0x4 (in flt)
 *   USART1_SR   (0x40013800)  bit5 RXNE, bit7 TXE
 *   USART1_DR   (0x40013804)  write to send, read to receive
 *   USART1_BRR  (0x40013808)  baud divisor (0x0341 = 9600 @ 8 MHz)
 *   USART1_CR1  (0x4001380C)  bit2 RE, bit3 TE, bit13 UE
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width types,
 * no dynamic memory, explicit masks, U-suffixed literals.
 */

#include <stdint.h>

/* ---- Register definitions (volatile: the compiler must not cache them) -- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)

#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* PA8..PA15 config */

#define USART1_SR     (*(volatile uint32_t *)0x40013800U)  /* status           */
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)  /* data             */
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)  /* baud rate divisor*/
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)  /* control 1        */

/* ---- RCC enable bits (RCC_APB2ENR, RM0041 sec.7.3.7) -------------------- */
#define RCC_IOPAEN    (1U << 2)    /* enable clock for GPIO port A            */
#define RCC_USART1EN  (1U << 14)   /* enable clock for USART1                 */

/* ---- USART_SR flags (RM0041 sec.27.6.1) --------------------------------- */
#define USART_SR_RXNE (1U << 5)    /* read data register not empty            */
#define USART_SR_TXE  (1U << 7)    /* transmit data register empty            */

/* ---- USART_CR1 control bits (RM0041 sec.27.6.4) ------------------------- */
#define USART_CR1_RE  (1U << 2)    /* receiver enable                         */
#define USART_CR1_TE  (1U << 3)    /* transmitter enable                      */
#define USART_CR1_UE  (1U << 13)   /* USART enable                            */

/*
 * PA9 (TX) and PA10 (RX) both live in CRH (pins 8-15). Their 4-bit fields:
 *   PA9  -> bits [7:4]   = (9 - 8)  * 4 = 4
 *   PA10 -> bits [11:8]  = (10 - 8) * 4 = 8
 * TX nibble = 0xB = 0b1011 -> MODE=11 (output 50 MHz), CNF=10 (AF push-pull).
 * RX nibble = 0x4 = 0b0100 -> MODE=00 (input),         CNF=01 (floating).
 */
#define PA9_CRH_SHIFT   (4U)
#define PA10_CRH_SHIFT  (8U)
#define PA9_CRH_MASK    (0xFU << PA9_CRH_SHIFT)
#define PA10_CRH_MASK   (0xFU << PA10_CRH_SHIFT)
#define PA9_CRH_AF_PP   (0xBU << PA9_CRH_SHIFT)   /* AF push-pull 50 MHz       */
#define PA10_CRH_IN_FLT (0x4U << PA10_CRH_SHIFT)  /* input floating            */

/*
 * Baud-rate divisor for 9600 baud at the 8 MHz reset clock (HSI):
 *   USARTDIV = fck / (16 * baud) = 8e6 / (16 * 9600) = 52.0833...
 *   mantissa = 52 = 0x34 -> BRR[15:4]
 *   fraction = round(0.0833 * 16) = 1 -> BRR[3:0]
 *   BRR = 0x341.
 * QEMU ignores BRR (it has no real bit timing), but we set it correctly so
 * the program is right on real hardware too.
 */
#define USART1_BRR_9600 (0x0341U)

/*
 * uart_init - bring up USART1 for both transmit AND receive.
 * Order matters: clocks first, then pin config, then peripheral config.
 */
static void uart_init(void)
{
    /* 1. Clock GPIO port A (for PA9/PA10) and USART1. A peripheral with no
     *    clock ignores every register access, so this is always step one. */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* 2. Configure the two pins in CRH with a single read-modify-write:
     *    clear both 4-bit fields, then OR in TX (AF PP) and RX (input flt). */
    GPIOA_CRH = (GPIOA_CRH & ~(PA9_CRH_MASK | PA10_CRH_MASK))
              | PA9_CRH_AF_PP | PA10_CRH_IN_FLT;

    /* 3. Set the baud rate. Must be written while the USART is disabled. */
    USART1_BRR = USART1_BRR_9600;

    /* 4. Enable transmitter, receiver, and the USART itself. Setting RE in
     *    addition to TE is the whole point of this module - it switches the
     *    receive hardware on so incoming bytes set RXNE. */
    USART1_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

/*
 * uart_putc - send one byte (blocking). Wait until the transmit data register
 * is empty (TXE = 1), then write the byte to DR. (Reused from Module 06.)
 */
static void uart_putc(uint8_t c)
{
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* spin: QEMU sets TXE, so this terminates */
    }
    USART1_DR = (uint32_t)c;
}

/*
 * uart_puts - send a NUL-terminated string. Translate '\n' into "\r\n" so
 * terminals advance to the start of the next line. (Reused from Module 06.)
 */
static void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            uart_putc((uint8_t)'\r');
        }
        uart_putc((uint8_t)*s);
        s++;
    }
}

/*
 * uart_getc - receive one byte (blocking). Poll RXNE until a byte has been
 * received, then READ DR. That read both returns the byte AND clears RXNE,
 * so the receiver is ready for the next byte. This is "polled" receive.
 *
 * QEMU sets RXNE when a character arrives on stdin, so this loop terminates
 * as soon as you type (or pipe in) a byte.
 */
static uint8_t uart_getc(void)
{
    while ((USART1_SR & USART_SR_RXNE) == 0U)
    {
        /* spin until a byte has been received */
    }
    return (uint8_t)(USART1_DR & 0xFFU);
}

int main(void)
{
    uint8_t c;

    uart_init();

    /* Greet the user once, then enter the echo loop. */
    uart_puts("USART1 echo ready. Type something:\n");

    /*
     * Echo loop: read a byte, send it straight back. Because the terminal
     * does not normally echo what QEMU receives, this is what makes your
     * typing visible. When the byte is a carriage return ('\r', what the
     * Enter key sends), also emit a newline so the cursor drops to a fresh
     * line - a tiny "line reader" courtesy.
     */
    for (;;)
    {
        c = uart_getc();        /* blocks until RXNE; reading DR clears it */
        uart_putc(c);           /* echo the received byte back            */

        if (c == (uint8_t)'\r')
        {
            uart_putc((uint8_t)'\n');
        }
    }

    /* not reached */
    return 0;
}

/*
 * _write - retarget for newlib so printf()/puts() send to USART1. Not strictly
 * needed by the echo loop above (which uses uart_putc directly), but provided
 * for consistency with Module 06 and so students can add printf() debugging.
 * setvbuf(stdout, NULL, _IONBF, 0) in Module 06 keeps stdout unbuffered so no
 * heap is required.
 */
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
