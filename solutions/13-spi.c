/*
 * solutions/13-spi.c  -  REFERENCE SOLUTION (Module 13: SPI Driver)
 * ==================================================================
 *
 * Goal: bring up SPI1 as a software-NSS master and run the classic SPI
 * full-duplex byte exchange: wait TXE -> write DR -> wait RXNE -> read DR.
 * We narrate everything over USART1 (printf retargeted, as in Module 06),
 * then dump CR1 read-back and the bytes "received" during a couple of
 * transfers.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6, default clock HSI = 8 MHz.
 * SPI1 base 0x40013000 (APB2). RM0041 sec.25 (SPI), sec.7 (RCC), sec.8 (GPIO).
 * SPI1 pins: SCK = PA5, MISO = PA6, MOSI = PA7 (all alternate-function
 * push-pull). With software NSS we do not drive a hardware NSS pin.
 *
 * WHAT YOU WILL SEE IN QEMU (important - read this):
 *   The qemu-system-arm -M stm32vldiscovery SPI1 model is PARTIALLY emulated:
 *     - CR1 reads back the value you wrote (so we can prove the register
 *       configuration actually landed - a rare luxury in this course),
 *     - the SR status flags TXE and RXNE are reported set, so the polling
 *       flow makes progress and never hangs,
 *     - BUT there is NO slave device attached (no real ADXL345, no shift
 *       register, nothing drives MISO). So any byte we "receive" by reading
 *       DR is MEANINGLESS - it is not data from a peer. On real hardware,
 *       with a real slave selected, that read returns the byte the slave
 *       clocked back to us.
 *
 *   To stay safe and never freeze (the matrix's hard rule), every flag wait
 *   is BOUNDED with a guard counter. If a flag never sets we print a note and
 *   carry on instead of spinning forever.
 *
 * Registers used (SPI1 base 0x40013000):
 *   SPI1_CR1 (0x40013000)  bit0 CPHA, bit1 CPOL, bit2 MSTR, bits[5:3] BR,
 *                          bit6 SPE, bit7 LSBFIRST, bit8 SSI, bit9 SSM,
 *                          bit11 DFF
 *   SPI1_CR2 (0x40013004)  interrupt / DMA / SS-output enables (unused here)
 *   SPI1_SR  (0x40013008)  bit0 RXNE, bit1 TXE, bit7 BSY
 *   SPI1_DR  (0x4001300C)  data register (write to TX, read to RX)
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
#define RCC_SPI1EN    (1U << 12)   /* SPI1 clock enable                      */

/* ---- GPIOA (PA9 = USART1 TX; PA5/6/7 = SPI1 SCK/MISO/MOSI) -------------- */
#define GPIOA_CRL     (*(volatile uint32_t *)0x40010800U)  /* pins 0-7       */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15      */

/*
 * PA9 (USART1 TX) lives in CRH, nibble at bits [7:4]:
 *   AF push-pull 50 MHz: MODE=0b11, CNF=0b10 -> nibble 0b1011 = 0xB.
 */
#define PA9_CRH_SHIFT (4U)
#define PA9_CRH_MASK  (0xFU << PA9_CRH_SHIFT)
#define PA9_CRH_AF    (0xBU << PA9_CRH_SHIFT)

/*
 * SPI1 pins are all in CRL (pins 0-7), 4 bits per pin:
 *   PA5 SCK  -> shift (5*4)=20
 *   PA6 MISO -> shift (6*4)=24
 *   PA7 MOSI -> shift (7*4)=28
 * For the master, SCK and MOSI are peripheral-driven outputs -> AF push-pull
 * 50 MHz, nibble 0xB. MISO is an input the peripheral samples; configuring it
 * as AF push-pull (0xB) is the conventional choice (the alternate function
 * controls direction). We program all three with 0xB for simplicity.
 */
#define PA5_CRL_SHIFT (20U)
#define PA6_CRL_SHIFT (24U)
#define PA7_CRL_SHIFT (28U)
#define SPI_PINS_MASK (((uint32_t)0xFU << PA5_CRL_SHIFT) | \
                       ((uint32_t)0xFU << PA6_CRL_SHIFT) | \
                       ((uint32_t)0xFU << PA7_CRL_SHIFT))
#define SPI_PINS_AF   (((uint32_t)0xBU << PA5_CRL_SHIFT) | \
                       ((uint32_t)0xBU << PA6_CRL_SHIFT) | \
                       ((uint32_t)0xBU << PA7_CRL_SHIFT))

/* ---- USART1 (for printf narration) -------------------------------------- */
#define USART1_SR     (*(volatile uint32_t *)0x40013800U)
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)
#define USART_SR_TXE  (1U << 7)
#define USART_CR1_TE  (1U << 3)
#define USART_CR1_UE  (1U << 13)
#define USART1_BRR_9600  (0x341U)   /* 8 MHz / (16 * 9600) -> 0x341          */

/* ---- SPI1 --------------------------------------------------------------- */
#define SPI1_CR1      (*(volatile uint32_t *)0x40013000U)
#define SPI1_CR2      (*(volatile uint32_t *)0x40013004U)
#define SPI1_SR       (*(volatile uint32_t *)0x40013008U)
#define SPI1_DR       (*(volatile uint32_t *)0x4001300CU)

/* CR1 bit positions (RM0041 sec.25.5.1) */
#define SPI_CR1_CPHA      (1U << 0)   /* clock phase                          */
#define SPI_CR1_CPOL      (1U << 1)   /* clock polarity                       */
#define SPI_CR1_MSTR      (1U << 2)   /* 1 = master                           */
#define SPI_CR1_BR_SHIFT  (3U)        /* BR[2:0] baud prescaler at bits [5:3] */
#define SPI_CR1_BR_DIV16  (0x3U << SPI_CR1_BR_SHIFT) /* fPCLK / 16            */
#define SPI_CR1_SPE       (1U << 6)   /* SPI peripheral enable                */
#define SPI_CR1_LSBFIRST  (1U << 7)   /* 1 = LSB first (we leave 0 = MSB)     */
#define SPI_CR1_SSI       (1U << 8)   /* internal slave-select level          */
#define SPI_CR1_SSM       (1U << 9)   /* software slave management            */
#define SPI_CR1_DFF       (1U << 11)  /* 0 = 8-bit frame, 1 = 16-bit frame    */

/* SR status flags (RM0041 sec.25.5.3) */
#define SPI_SR_RXNE   (1U << 0)   /* receive buffer not empty (data to read)  */
#define SPI_SR_TXE    (1U << 1)   /* transmit buffer empty (room to write)    */
#define SPI_SR_BSY    (1U << 7)   /* SPI bus busy                             */

/* Bounded-wait guard. On real hardware the flag sets quickly and the loop
 * exits early; the guard only matters if a flag is never satisfied. */
#define WAIT_GUARD    (100000U)

/* ------------------------------------------------------------------------ */
/* 1. Minimal USART1 transmitter + printf retarget (see Module 06)          */
/* ------------------------------------------------------------------------ */
static void uart_init(void)
{
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);
    GPIOA_CRH = (GPIOA_CRH & ~PA9_CRH_MASK) | PA9_CRH_AF;
    USART1_BRR = USART1_BRR_9600;
    USART1_CR1 = (USART_CR1_TE | USART_CR1_UE);
}

static void uart_putc(uint8_t c)
{
    /* QEMU sets USART TXE, so this unbounded wait always makes progress. */
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* wait for TX data register empty */
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

/* newlib stubs to keep the build warning-clean (nosys.specs). */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)     { (void)fd; if (st != NULL) { st->st_mode = S_IFCHR; } return 0; }

/* ------------------------------------------------------------------------ */
/* 2. SPI1 master initialisation (software NSS)                             */
/* ------------------------------------------------------------------------ */
/*
 * Software-NSS master bring-up sequence (RM0041 sec.25.3.3):
 *   - enable the SPI1 and GPIOA clocks,
 *   - configure SCK/MISO/MOSI as AF push-pull,
 *   - in CR1: set MSTR (master), SSM (software slave management) and SSI
 *     (drive the internal NSS high). Without SSM+SSI a master sees its NSS
 *     pulled low and immediately drops out of master mode (MODF fault), so
 *     these two bits are mandatory when you are not using a hardware NSS.
 *   - select the baud prescaler in BR[5:3] (here fPCLK/16),
 *   - leave CPOL=CPHA=0 (SPI mode 0), DFF=0 (8-bit), LSBFIRST=0 (MSB first),
 *   - finally set SPE to enable the peripheral. SPE must be set LAST.
 */
static void spi1_init(void)
{
    /* Clocks: SPI1 (APB2 bit12) and GPIO port A (for the three pins). */
    RCC_APB2ENR |= (RCC_SPI1EN | RCC_IOPAEN);

    /* PA5/PA6/PA7 -> alternate-function push-pull so SPI1 owns the pins. */
    GPIOA_CRL = (GPIOA_CRL & ~SPI_PINS_MASK) | SPI_PINS_AF;

    /* Build CR1 in one write. MSTR + SSM + SSI + BR=div16 + SPE.
     * CPOL=CPHA=0 (mode 0), DFF=0 (8-bit), LSBFIRST=0 (MSB first). */
    SPI1_CR1 = SPI_CR1_MSTR
             | SPI_CR1_SSM
             | SPI_CR1_SSI
             | SPI_CR1_BR_DIV16
             | SPI_CR1_SPE;
}

/* ------------------------------------------------------------------------ */
/* 3. Full-duplex single-byte transfer                                      */
/* ------------------------------------------------------------------------ */
/*
 * SPI is inherently full-duplex: one clocked frame shifts a TX byte out on
 * MOSI while simultaneously shifting an RX byte in on MISO. So every transfer
 * both sends and receives. The polled flow:
 *   1. wait until TXE = 1 (transmit buffer has room),
 *   2. write the outgoing byte to DR -> this starts the clock,
 *   3. wait until RXNE = 1 (a full frame has been shifted in),
 *   4. read DR to get the received byte and clear RXNE.
 *
 * Both waits are BOUNDED so we never hang if a flag never sets. The returned
 * byte is real data on hardware; in QEMU (no slave) it is meaningless.
 */
static uint8_t spi_transfer(uint8_t tx)
{
    uint32_t guard;

    /* 1. Wait for room in the transmit buffer (TXE). */
    guard = WAIT_GUARD;
    while (((SPI1_SR & SPI_SR_TXE) == 0U) && (guard > 0U))
    {
        guard--;
    }
    if (guard == 0U)
    {
        printf("  (QEMU: TXE never set - peripheral limitation)\n");
    }

    /* 2. Writing DR launches the clocked exchange. */
    SPI1_DR = (uint32_t)tx;

    /* 3. Wait for a complete frame to arrive (RXNE). */
    guard = WAIT_GUARD;
    while (((SPI1_SR & SPI_SR_RXNE) == 0U) && (guard > 0U))
    {
        guard--;
    }
    if (guard == 0U)
    {
        printf("  (QEMU: RXNE never set - peripheral limitation)\n");
    }

    /* 4. Read DR: returns the received byte and clears RXNE. */
    return (uint8_t)SPI1_DR;
}

/* ------------------------------------------------------------------------ */
/* 4. Application                                                           */
/* ------------------------------------------------------------------------ */
int main(void)
{
    uint8_t rx;

    uart_init();
    (void)setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: no heap needed */

    printf("\n=== Module 13: SPI Driver ===\n");

    spi1_init();

    /* Prove the configuration landed: unlike GPIO/I2C, QEMU lets SPI1_CR1
     * read back. We expect MSTR|SSM|SSI|BR(div16)|SPE set. */
    printf("SPI1_CR1 read-back = 0x%08X\n", (unsigned int)SPI1_CR1);
    printf("  expected bits: MSTR(2) BR=div16(5:3) SPE(6) SSI(8) SSM(9)\n");
    printf("  mode 0 (CPOL=0,CPHA=0), 8-bit frame (DFF=0), MSB first\n");

    /* Run a couple of full-duplex transfers. The bytes we get back are
     * MEANINGLESS in QEMU (no slave drives MISO); they would be the slave's
     * reply on real hardware. */
    printf("Transferring bytes (RX is meaningless in QEMU - no slave):\n");

    rx = spi_transfer(0x9FU);   /* e.g. a "read ID" command to some slave */
    printf("  sent 0x9F -> received 0x%02X\n", (unsigned int)rx);

    rx = spi_transfer(0xABU);
    printf("  sent 0xAB -> received 0x%02X\n", (unsigned int)rx);

    printf("=== done; SPI idle ===\n");

    for (;;)
    {
        /* nothing more to transfer */
    }

    /* not reached */
    return 0;
}
