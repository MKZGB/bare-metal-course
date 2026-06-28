/*
 * solutions/11-dma.c  -  REFERENCE SOLUTION (Module 11: DMA Driver, UART TX)
 * =========================================================================
 *
 * Goal: configure DMA1 Channel 4 to push a string into USART1's data register
 * WITHOUT the CPU copying each byte. In Module 06 the CPU polled TXE and wrote
 * every byte itself (uart_putc). Here the DMA controller becomes the data
 * mover: we hand it a source address (our buffer), a destination address
 * (USART1_DR), and a count, then the peripheral copies bytes on its own while
 * the CPU is free to do other work.
 *
 * Board / chip: STM32VLDISCOVERY, STM32F100RBT6, default clock HSI = 8 MHz.
 * USART1 TX is on PA9 (alternate-function push-pull). RM0041 sec.10/13 (DMA),
 * sec.27 (USART), sec.7 (RCC), sec.8 (GPIO).
 *
 * WHY DMA1 Channel 4? Each DMA request line is hard-wired to a fixed channel
 * on the STM32F1. USART1_TX is mapped to DMA1 Channel 4 (RM0041 Table 78).
 *
 * ===========================================================================
 * !! CRITICAL - WHAT YOU WILL SEE IN QEMU !!
 *   qemu-system-arm -M stm32vldiscovery does NOT emulate the DMA controller.
 *   No transfer ever happens: the bytes in our buffer never reach USART1_DR,
 *   nothing appears on the serial console from the DMA path, and the transfer-
 *   complete flag (TCIF4) is NEVER set. A naive `while(!(DMA_ISR & TCIF4)){}`
 *   would therefore HANG FOREVER in QEMU.
 *
 *   So we do two things:
 *     (1) We print an explanatory banner FIRST using the polled uart_putc /
 *         printf path from Module 06 (USART1 itself IS emulated), so the
 *         student sees observable output describing what was set up.
 *     (2) We wait for TCIF4 with a BOUNDED loop (a guard counter), then print
 *         the outcome. On real hardware the flag sets and the loop exits
 *         early; in QEMU the guard expires and we report the limitation.
 *
 *   The register sequence below is correct for real STM32 hardware - on a real
 *   board the DMA really transmits the string and TCIF4 is set.
 * ===========================================================================
 *
 * No HAL, no CMSIS. Direct register access. MISRA-aligned: fixed-width types,
 * no dynamic memory, explicit masks and U suffixes.
 */

#include <stdint.h>
#include <stdio.h>      /* printf, setvbuf, _IONBF */
#include <sys/stat.h>   /* struct stat for the _fstat syscall stub */

/* ======================================================================== */
/* USART1 polled TX driver + printf retarget (carried over from Module 06)  */
/* ======================================================================== */

/* ---- RCC (clock gating) ------------------------------------------------- */
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018U)
#define RCC_AHBENR    (*(volatile uint32_t *)0x40021014U)
#define RCC_IOPAEN    (1U << 2)     /* GPIO port A clock enable               */
#define RCC_USART1EN  (1U << 14)    /* USART1 clock enable                    */
#define RCC_DMA1EN    (1U << 0)     /* DMA1 clock enable (AHB bus)            */

/* ---- GPIOA (PA9 = USART1 TX) -------------------------------------------- */
#define GPIOA_CRH     (*(volatile uint32_t *)0x40010804U)  /* pins 8-15      */
#define PA9_CRH_SHIFT (4U)
#define PA9_CRH_MASK  (0xFU << PA9_CRH_SHIFT)   /* clear pin-9 nibble        */
#define PA9_CRH_AF    (0xBU << PA9_CRH_SHIFT)   /* AF push-pull, 50 MHz      */

/* ---- USART1 ------------------------------------------------------------- */
#define USART1_BASE   0x40013800U
#define USART1_SR     (*(volatile uint32_t *)(USART1_BASE + 0x00U))
#define USART1_DR     (*(volatile uint32_t *)(USART1_BASE + 0x04U))
#define USART1_BRR    (*(volatile uint32_t *)(USART1_BASE + 0x08U))
#define USART1_CR1    (*(volatile uint32_t *)(USART1_BASE + 0x0CU))
#define USART1_CR3    (*(volatile uint32_t *)(USART1_BASE + 0x14U))

#define USART1_DR_ADDR (USART1_BASE + 0x04U)   /* DMA peripheral destination  */

#define USART_SR_TXE  (1U << 7)     /* transmit data register empty           */
#define USART_CR1_TE  (1U << 3)     /* transmitter enable                     */
#define USART_CR1_UE  (1U << 13)    /* USART enable                           */
#define USART_CR3_DMAT (1U << 7)    /* DMA enable for transmitter             */

#define USART1_BRR_9600  (0x341U)   /* 8 MHz / (16 * 9600), see Module 06    */

static void uart_init(void)
{
    /* Clock GPIO port A (PA9) and USART1. */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* PA9 = alternate-function push-pull so USART1 drives the pin. */
    GPIOA_CRH = (GPIOA_CRH & ~PA9_CRH_MASK) | PA9_CRH_AF;

    USART1_BRR = USART1_BRR_9600;
    USART1_CR1 = (USART_CR1_TE | USART_CR1_UE);
}

/* Polled single-byte send: QEMU sets TXE, so this never hangs. */
static void uart_putc(uint8_t c)
{
    while ((USART1_SR & USART_SR_TXE) == 0U)
    {
        /* wait until the TX data register is empty */
    }
    USART1_DR = (uint32_t)c;
}

/* Retarget printf to USART1 (same approach as Module 06). */
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

/* newlib syscall stubs, keep the link warning-clean (see Module 06). */
int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int off, int dir)    { (void)fd; (void)off; (void)dir; return 0; }
int _isatty(int fd)                     { (void)fd; return 1; }
int _fstat(int fd, struct stat *st)     { (void)fd; if (st != NULL) { st->st_mode = S_IFCHR; } return 0; }

/* ======================================================================== */
/* DMA1 controller registers (base 0x40020000, RM0041 sec.13)              */
/* ======================================================================== */
/*
 * The DMA controller has shared status registers plus a block of per-channel
 * config registers. Each channel's block is 0x14 (20) bytes wide and starts
 * at offset 0x08 + (ch - 1) * 0x14:
 *
 *   channel 1 -> 0x08, channel 2 -> 0x1C, channel 3 -> 0x30,
 *   channel 4 -> 0x44, ...
 *
 * For USART1_TX we use CHANNEL 4, so its registers are:
 *   CCR4   = 0x40020000 + 0x44 = 0x40020044   (config)
 *   CNDTR4 = 0x40020000 + 0x48 = 0x40020048   (number of data items)
 *   CPAR4  = 0x40020000 + 0x4C = 0x4002004C   (peripheral address)
 *   CMAR4  = 0x40020000 + 0x50 = 0x40020050   (memory address)
 */
#define DMA1_BASE     0x40020000U
#define DMA1_ISR      (*(volatile uint32_t *)(DMA1_BASE + 0x00U))  /* status  */
#define DMA1_IFCR     (*(volatile uint32_t *)(DMA1_BASE + 0x04U))  /* clear   */

#define DMA1_CCR4     (*(volatile uint32_t *)(DMA1_BASE + 0x44U))
#define DMA1_CNDTR4   (*(volatile uint32_t *)(DMA1_BASE + 0x48U))
#define DMA1_CPAR4    (*(volatile uint32_t *)(DMA1_BASE + 0x4CU))
#define DMA1_CMAR4    (*(volatile uint32_t *)(DMA1_BASE + 0x50U))

/* ---- CCRx (channel configuration) bit fields --------------------------- */
#define DMA_CCR_EN    (1U << 0)     /* channel enable                         */
#define DMA_CCR_TCIE  (1U << 1)     /* transfer-complete interrupt enable     */
#define DMA_CCR_DIR   (1U << 4)     /* 1 = read from memory (memory -> periph)*/
#define DMA_CCR_CIRC  (1U << 5)     /* circular mode                          */
#define DMA_CCR_MINC  (1U << 7)     /* memory increment after each transfer   */
/* PSIZE [9:8] and MSIZE [11:10] left 00 = 8-bit, PL [13:12] left 00 = low.  */

/*
 * The status register packs 4 flags per channel. For channel 4 they live in
 * bits [15:12]: GIF4(12) TCIF4(13) HTIF4(14) TEIF4(15). The CLEAR register
 * (IFCR) uses the same bit layout - write 1 to clear.
 */
#define DMA_ISR_TCIF4   (1U << 13)  /* channel 4 transfer-complete flag       */
#define DMA_IFCR_CTCIF4 (1U << 13)  /* write 1 to clear TCIF4                  */
#define DMA_IFCR_CGIF4  (1U << 12)  /* write 1 to clear the global flag       */

/* The message DMA will move from SRAM into USART1_DR, byte by byte. */
static const char dma_message[] = "Hello via DMA1 Channel 4 -> USART1_TX\r\n";

/* ------------------------------------------------------------------------ */
/* Configure DMA1 Channel 4 to drive USART1 TX                              */
/* ------------------------------------------------------------------------ */
static void dma_uart_tx_start(const char *buf, uint16_t len)
{
    /* 1. Enable the DMA1 clock (it lives on the AHB bus, not APB2). */
    RCC_AHBENR |= RCC_DMA1EN;

    /* 2. Make sure the channel is disabled before reconfiguring it: CPAR/CMAR/
     *    CNDTR may only be written while EN = 0. */
    DMA1_CCR4 &= ~DMA_CCR_EN;

    /* 3. Peripheral address = where bytes go: the USART1 data register. */
    DMA1_CPAR4 = USART1_DR_ADDR;

    /* 4. Memory address = where bytes come from: our buffer in SRAM. */
    DMA1_CMAR4 = (uint32_t)buf;

    /* 5. How many items to move (8-bit items, so = number of chars). */
    DMA1_CNDTR4 = (uint32_t)len;

    /* 6. Direction = read from memory (memory -> peripheral) and increment the
     *    MEMORY pointer after each byte so we walk through the buffer. The
     *    peripheral pointer (USART1_DR) stays fixed, so MINC only, no PINC.
     *    PSIZE/MSIZE stay 00 (8-bit), PL stays 00 (low priority). */
    DMA1_CCR4 = (DMA_CCR_DIR | DMA_CCR_MINC);

    /* 7. Tell USART1 to issue a DMA request whenever its TX register is empty
     *    (CR3.DMAT). Without this the channel would sit idle. */
    USART1_CR3 |= USART_CR3_DMAT;

    /* 8. Finally enable the channel. From here the DMA controller copies bytes
     *    on its own; the CPU does NOT touch USART1_DR. */
    DMA1_CCR4 |= DMA_CCR_EN;
}

/* ------------------------------------------------------------------------ */
/* Application                                                              */
/* ------------------------------------------------------------------------ */
int main(void)
{
    uint32_t guard;
    uint16_t len;

    uart_init();
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- Observable banner, printed the OLD way (CPU-polled uart_putc). ---- */
    printf("\r\n=== Module 11: DMA Driver (UART TX) ===\n");
    printf("This banner is sent by the CPU itself (polled uart_putc),\n");
    printf("exactly like Module 06.\n\n");
    printf("Now configuring DMA1 Channel 4 to transmit a string with NO\n");
    printf("CPU involvement:\n");
    printf("  CPAR4  = USART1_DR  (0x%08X)\n", (unsigned int)USART1_DR_ADDR);
    printf("  CMAR4  = &message   (0x%08X)\n", (unsigned int)(uint32_t)dma_message);
    printf("  CNDTR4 = %u bytes\n", (unsigned int)(sizeof(dma_message) - 1U));
    printf("  CCR4   : DIR=1 (mem->periph), MINC=1, then EN=1\n");
    printf("  USART1_CR3.DMAT = 1\n\n");

    /* Length excludes the terminating NUL. */
    len = (uint16_t)(sizeof(dma_message) - 1U);

    /* Kick off the hardware transfer. */
    dma_uart_tx_start(dma_message, len);

    /* ---- BOUNDED wait for transfer-complete (TCIF4). --------------------- *
     * On real hardware TCIF4 sets when all bytes have been moved and the loop
     * exits early. In QEMU the DMA is not emulated, so TCIF4 NEVER sets - the
     * guard counter expires instead, and we report that. Crucially this does
     * NOT hang. */
    guard = 1000000U;
    while (((DMA1_ISR & DMA_ISR_TCIF4) == 0U) && (guard > 0U))
    {
        guard--;
    }

    if (guard == 0U)
    {
        printf("Result: transfer did not complete - DMA not emulated by QEMU;\n");
        printf("        the DMA-sent string above is absent here but the\n");
        printf("        register setup is correct on real hardware.\n");
    }
    else
    {
        /* Real hardware path: clear the flag and report success. */
        DMA1_IFCR = (DMA_IFCR_CTCIF4 | DMA_IFCR_CGIF4);
        printf("Result: DMA transfer complete (TCIF4 set) - string sent by DMA.\n");
    }

    printf("=== done; CPU idle ===\n");

    for (;;)
    {
        /* nothing more to do */
    }

    /* not reached */
    return 0;
}
