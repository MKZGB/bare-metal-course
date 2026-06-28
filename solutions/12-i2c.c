/*
 * solutions/12-i2c.c  -  REFERENCE SOLUTION (Module 12: I2C Driver)
 * =================================================================
 *
 * Goal: build a register-level I2C master driver for I2C1 and walk the
 * master-transmit state machine step by step, NARRATING each step over
 * USART1 with printf. We attempt to address a fictitious ADXL345
 * accelerometer (7-bit address 0x53) purely as an illustrative target.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6, default clock HSI = 8 MHz
 * (so PCLK1 / APB1 = 8 MHz). I2C1 SCL = PB6, SDA = PB7, both alternate-
 * function OPEN-DRAIN (I2C is an open-drain bus). RM0041 sec.26 (I2C),
 * sec.7 (RCC), sec.8 (GPIO).
 *
 * =====================================================================
 *  ⚠  WHAT YOU WILL SEE IN QEMU  -  READ THIS
 * =====================================================================
 *   qemu-system-arm -M stm32vldiscovery emulates NO I2C peripheral, and
 *   there is NO slave device on the bus (the ADXL345 below is NOT present).
 *   Concretely:
 *     - writes to the I2C registers are dropped,
 *     - reads of SR1/SR2 return 0,
 *     - the SB / ADDR / BTF / TxE flags NEVER set.
 *   Therefore the master sequence CANNOT complete past "set START": it
 *   would wait for SB forever. THIS IS EXPECTED. The point of this module
 *   is to understand the register state machine, not to talk to a real
 *   sensor.
 *
 *   To avoid an infinite hang we use a BOUNDED wait at every flag poll:
 *   a guard counter that decrements, and on timeout we print which step
 *   we reached and continue. On REAL hardware (with a real ADXL345 on the
 *   bus) the flag sets quickly and the loop exits early - the same code
 *   works correctly there.
 *
 * Registers used (I2C1 base 0x40005400, RM0041 sec.26):
 *   CR1   (0x40005400)  bit0 PE, bit8 START, bit9 STOP, bit10 ACK
 *   CR2   (0x40005404)  bits[5:0] FREQ = APB1 clock in MHz (= 8)
 *   OAR1  (0x40005408)  own address (master mode: not strictly needed)
 *   DR    (0x40005410)  data register (write addr/data, read received)
 *   SR1   (0x40005414)  bit0 SB, bit1 ADDR, bit2 BTF, bit6 RxNE, bit7 TxE
 *   SR2   (0x40005418)  bit1 BUSY  (read after SR1 to clear ADDR)
 *   CCR   (0x4000541C)  clock control (sets SCL frequency)
 *   TRISE (0x40005420)  max SCL rise time
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width
 * types, no dynamic memory, explicit masks and U suffixes.
 */

#include <stdint.h>
#include <stdio.h>      /* printf, setvbuf, _IONBF */
#include <sys/stat.h>   /* struct stat for the _fstat syscall stub */

/* ====================================================================== */
/* RCC (clock gating)                                                     */
/* ====================================================================== */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define RCC_APB1ENR   (*(volatile uint32_t *)0x4002101CU)
#define RCC_IOPAEN    (1U << 2)    /* GPIO port A clock (for USART1 TX PA9) */
#define RCC_IOPBEN    (1U << 3)    /* GPIO port B clock (for PB6/PB7)       */
#define RCC_AFIOEN    (1U << 0)    /* alternate-function I/O clock          */
#define RCC_USART1EN  (1U << 14)   /* USART1 clock                          */
#define RCC_I2C1EN    (1U << 21)   /* I2C1 clock (APB1)                     */

/* ====================================================================== */
/* GPIO  (PA9 = USART1 TX; PB6 = I2C1 SCL, PB7 = I2C1 SDA)                 */
/* ====================================================================== */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15    */
#define GPIOB_CRL     (*(volatile uint32_t *)0x40010C00U)  /* pins 0-7     */

/*
 * PA9 (USART1 TX): AF push-pull 50 MHz -> nibble 0xB at bits [7:4] of CRH.
 */
#define PA9_CRH_MASK  (0xFU << 4)
#define PA9_CRH_AF    (0xBU << 4)

/*
 * PB6 (SCL) and PB7 (SDA) both live in CRL (pins 0-7).
 *   PB6 nibble at bits [27:24], PB7 nibble at bits [31:28].
 * I2C is an OPEN-DRAIN bus (lines are pulled up; devices only pull low),
 * so the pins must be ALTERNATE-FUNCTION OPEN-DRAIN at 50 MHz:
 *   MODE = 0b11 (output 50 MHz), CNF = 0b11 (AF open-drain).
 *   nibble = (CNF << 2) | MODE = (0b11 << 2) | 0b11 = 0b1111 = 0xF.
 */
#define PB6_CRL_SHIFT (6U * 4U)              /* = 24 */
#define PB7_CRL_SHIFT (7U * 4U)              /* = 28 */
#define PB67_CRL_MASK ((0xFU << PB6_CRL_SHIFT) | (0xFU << PB7_CRL_SHIFT))
#define PB67_CRL_AFOD ((0xFU << PB6_CRL_SHIFT) | (0xFU << PB7_CRL_SHIFT))

/* ====================================================================== */
/* USART1 (only used to narrate the I2C sequence over the terminal)       */
/* ====================================================================== */
#define USART1_SR     (*(volatile uint32_t *)0x40013800U)
#define USART1_DR     (*(volatile uint32_t *)0x40013804U)
#define USART1_BRR    (*(volatile uint32_t *)0x40013808U)
#define USART1_CR1    (*(volatile uint32_t *)0x4001380CU)
#define USART_SR_TXE  (1U << 7)
#define USART_CR1_TE  (1U << 3)
#define USART_CR1_UE  (1U << 13)
#define USART1_BRR_9600 (0x341U)            /* 9600 baud @ 8 MHz           */

/* ====================================================================== */
/* I2C1                                                                   */
/* ====================================================================== */
#define I2C1_CR1      (*(volatile uint32_t *)0x40005400U)
#define I2C1_CR2      (*(volatile uint32_t *)0x40005404U)
#define I2C1_OAR1     (*(volatile uint32_t *)0x40005408U)
#define I2C1_DR       (*(volatile uint32_t *)0x40005410U)
#define I2C1_SR1      (*(volatile uint32_t *)0x40005414U)
#define I2C1_SR2      (*(volatile uint32_t *)0x40005418U)
#define I2C1_CCR      (*(volatile uint32_t *)0x4000541CU)
#define I2C1_TRISE    (*(volatile uint32_t *)0x40005420U)

#define I2C_CR1_PE    (1U << 0)
#define I2C_CR1_START (1U << 8)
#define I2C_CR1_STOP  (1U << 9)
#define I2C_CR1_ACK   (1U << 10)

#define I2C_SR1_SB    (1U << 0)    /* start condition generated            */
#define I2C_SR1_ADDR  (1U << 1)    /* address sent and ACKed               */
#define I2C_SR1_BTF   (1U << 2)    /* byte transfer finished               */
#define I2C_SR1_RxNE  (1U << 6)    /* receive data register not empty      */
#define I2C_SR1_TxE   (1U << 7)    /* transmit data register empty         */

/*
 * Timing for ~100 kHz standard-mode SCL from an 8 MHz APB1 clock:
 *   FREQ[5:0] in CR2 = APB1 frequency in MHz = 8.
 *   CCR (standard mode) = PCLK1 / (2 * SCL) = 8e6 / (2 * 100e3) = 40.
 *   TRISE = (max rise 1000 ns / Tpclk1) + 1 = (1000ns / 125ns) + 1
 *         = 8 + 1 = 9.
 */
#define I2C_FREQ_8MHZ   (8U)
#define I2C_CCR_100KHZ  (40U)
#define I2C_TRISE_8MHZ  (9U)

/* The ADXL345 accelerometer's 7-bit address (illustrative target ONLY -
 * NOT present in QEMU). Its DEVID register is at 0x00 and reads 0xE5 on
 * real hardware. */
#define ADXL345_ADDR_7BIT  (0x53U)
#define ADXL345_REG_DEVID  (0x00U)

/* Bound for every flag wait. Large enough to let a real device respond,
 * small enough that QEMU returns promptly instead of hanging. */
#define I2C_WAIT_GUARD     (200000U)

/* ====================================================================== */
/* 1. USART1 polled TX + printf retarget (narration only)                 */
/* ====================================================================== */
static void uart_init(void)
{
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);
    GPIOA_CRH = (GPIOA_CRH & ~PA9_CRH_MASK) | PA9_CRH_AF;
    USART1_BRR = USART1_BRR_9600;
    USART1_CR1 = (USART_CR1_TE | USART_CR1_UE);
}

static void uart_putc(uint8_t c)
{
    /* QEMU sets TXE, so this poll always makes progress (no hang here). */
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

/* Minimal newlib syscall stubs to keep the link warning-clean. */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)     { (void)fd; if (st != NULL) { st->st_mode = S_IFCHR; } return 0; }

/* ====================================================================== */
/* 2. I2C1 init: clocks, pins, timing, enable                             */
/* ====================================================================== */
static void i2c1_init(void)
{
    /* Enable clocks: GPIO port B (PB6/PB7) and AFIO on APB2; I2C1 on APB1.
     * A peripheral with no clock ignores all register writes. */
    RCC_APB2ENR |= (RCC_IOPBEN | RCC_AFIOEN);
    RCC_APB1ENR |= RCC_I2C1EN;

    /* PB6 (SCL) and PB7 (SDA): alternate-function open-drain, 50 MHz.
     * Read-modify-write so other pins in CRL are untouched. */
    GPIOB_CRL = (GPIOB_CRL & ~PB67_CRL_MASK) | PB67_CRL_AFOD;

    /* The peripheral must be DISABLED (PE = 0) while we set timing. After
     * reset PE is already 0; we make it explicit. */
    I2C1_CR1 &= ~I2C_CR1_PE;

    /* CR2 FREQ[5:0] tells the peripheral the APB1 clock frequency in MHz so
     * it can derive its internal timings. APB1 = 8 MHz here, so FREQ = 8. */
    I2C1_CR2 = I2C_FREQ_8MHZ;

    /* CCR sets the SCL clock period (standard mode 100 kHz here). */
    I2C1_CCR = I2C_CCR_100KHZ;

    /* TRISE bounds the maximum SCL rise time. */
    I2C1_TRISE = I2C_TRISE_8MHZ;

    /* Finally enable the peripheral. */
    I2C1_CR1 |= I2C_CR1_PE;
}

/*
 * Bounded wait for a SR1 flag. Returns 1 if the flag set within the guard,
 * 0 on timeout. The 'volatile' read of SR1 forces the compiler to actually
 * poll the register each iteration.
 */
static int i2c_wait_flag(uint32_t flag)
{
    uint32_t guard = I2C_WAIT_GUARD;
    while (((I2C1_SR1 & flag) == 0U) && (guard > 0U))
    {
        guard--;
    }
    return (guard > 0U) ? 1 : 0;
}

/*
 * Master-transmit ONE register write to a 7-bit slave address, narrating
 * each step. Walks the RM0041 sec.26.3.3 master-transmitter sequence:
 *   PE -> START -> wait SB -> write address -> wait ADDR -> read SR1,SR2
 *   -> write data -> wait BTF/TxE -> STOP.
 *
 * In QEMU every wait times out at the SB step (I2C not emulated) and we
 * report which step we reached, then return without hanging.
 */
static void i2c_write_byte(uint8_t addr7, uint8_t reg, uint8_t value)
{
    printf("\n[i2c] master-transmit to slave 0x%02X, reg 0x%02X = 0x%02X\n",
           (unsigned int)addr7, (unsigned int)reg, (unsigned int)value);

    /* Step 1: generate a START condition. This makes the master own the bus
     * and (on hardware) sets SR1.SB once the start is on the wire. */
    printf("[i2c] step 1: setting START...\n");
    I2C1_CR1 |= I2C_CR1_START;

    /* Step 2: wait for SB (start bit sent). */
    printf("[i2c] step 2: waiting for SB (start sent)...\n");
    if (i2c_wait_flag(I2C_SR1_SB) == 0)
    {
        printf("[i2c] SB not set - I2C not emulated in QEMU; correct on hardware.\n");
        printf("[i2c] aborting cleanly (no hang). Generating STOP.\n");
        I2C1_CR1 |= I2C_CR1_STOP;
        return;
    }

    /* Step 3: write the 7-bit address shifted left, with R/W bit = 0 (write)
     * into DR. Writing DR here clears SB. */
    printf("[i2c] step 3: SB set; writing address byte to DR...\n");
    I2C1_DR = (uint32_t)((addr7 << 1) | 0U);

    /* Step 4: wait for ADDR (address sent and ACKed by the slave). */
    printf("[i2c] step 4: waiting for ADDR (address ACKed)...\n");
    if (i2c_wait_flag(I2C_SR1_ADDR) == 0)
    {
        printf("[i2c] ADDR not set - no slave ACK (no device on bus in QEMU).\n");
        I2C1_CR1 |= I2C_CR1_STOP;
        return;
    }

    /* Step 5: clear ADDR by reading SR1 THEN SR2. The read of both status
     * registers is the documented sequence that clears the ADDR flag. */
    printf("[i2c] step 5: ADDR set; reading SR1 then SR2 to clear ADDR...\n");
    {
        volatile uint32_t tmp;
        tmp = I2C1_SR1;
        tmp = I2C1_SR2;
        (void)tmp;
    }

    /* Step 6: write the register pointer, then wait TxE/BTF. */
    printf("[i2c] step 6: writing register pointer 0x%02X...\n",
           (unsigned int)reg);
    I2C1_DR = (uint32_t)reg;
    if (i2c_wait_flag(I2C_SR1_TxE) == 0)
    {
        printf("[i2c] TxE not set - transfer cannot progress in QEMU.\n");
        I2C1_CR1 |= I2C_CR1_STOP;
        return;
    }

    /* Step 7: write the data byte, wait for BTF (byte transfer finished). */
    printf("[i2c] step 7: writing data 0x%02X; waiting for BTF...\n",
           (unsigned int)value);
    I2C1_DR = (uint32_t)value;
    if (i2c_wait_flag(I2C_SR1_BTF) == 0)
    {
        printf("[i2c] BTF not set - transfer cannot complete in QEMU.\n");
        I2C1_CR1 |= I2C_CR1_STOP;
        return;
    }

    /* Step 8: generate a STOP to release the bus. */
    printf("[i2c] step 8: BTF set; generating STOP. Transfer complete.\n");
    I2C1_CR1 |= I2C_CR1_STOP;
}

/* ====================================================================== */
/* 3. Application                                                         */
/* ====================================================================== */
int main(void)
{
    uart_init();
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    printf("\r\n=== Module 12: I2C Driver (register-level) ===\r\n");
    printf("Target: ADXL345 accelerometer @ 0x%02X (ILLUSTRATIVE -\n",
           (unsigned int)ADXL345_ADDR_7BIT);
    printf("        NOT present in QEMU; I2C is not emulated).\n");

    i2c1_init();
    printf("[i2c] init done: FREQ=%u CCR=%u TRISE=%u, PE set.\n",
           (unsigned int)I2C_FREQ_8MHZ,
           (unsigned int)I2C_CCR_100KHZ,
           (unsigned int)I2C_TRISE_8MHZ);

    /* Attempt a write to the (absent) ADXL345 DEVID register. In QEMU this
     * stops at the SB wait and reports it; on hardware it would complete. */
    i2c_write_byte(ADXL345_ADDR_7BIT, ADXL345_REG_DEVID, 0x00U);

    printf("\n=== done; program reached the end WITHOUT hanging ===\r\n");

    for (;;)
    {
        /* idle */
    }

    /* not reached */
    return 0;
}
