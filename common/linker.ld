/*
 * linker.ld - Linker script for the STM32F100RBT6 (STM32VLDISCOVERY)
 * ====================================================================
 *
 * This single linker script is shared by every module in the course.
 * It describes TWO things to the linker (ld):
 *
 *   1. The MEMORY map of the chip  - where Flash and RAM physically live.
 *   2. The SECTIONS layout         - which parts of your program go where.
 *
 * Target chip: STM32F100RBT6 (Cortex-M3, "Value Line")
 *   - 128 KB Flash, mapped at 0x08000000
 *   -   8 KB SRAM,  mapped at 0x20000000
 *
 * Reference: RM0041 sec. 3 (Memory map) and the STM32F100xB datasheet.
 *
 * NOTE on units: 'K' here means 1024 bytes (ld convention).
 */

/* The very first instruction executed after reset is found via the vector
 * table, but the linker still likes to know a default entry symbol. */
ENTRY(Reset_Handler)

/* ----------------------------------------------------------------------
 * 1. Physical memory regions of the STM32F100RBT6.
 * ----------------------------------------------------------------------
 *   rx  = readable + executable  (Flash: holds code and constants)
 *   rwx = readable + writable + executable (SRAM: data, stack, heap)
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

/* Top of stack = end of RAM. The Cortex-M3 stack grows DOWNWARDS, so the
 * initial Main Stack Pointer (MSP) is loaded with the highest RAM address.
 * The CPU loads this value from vector-table entry 0 automatically at reset.
 */
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 0x20002000 */

SECTIONS
{
    /* ------------------------------------------------------------------
     * .isr_vector - the interrupt/exception vector table.
     * MUST be the first thing in Flash (at 0x08000000) because the
     * Cortex-M3 fetches the initial SP and Reset vector from address 0.
     * KEEP() stops the linker garbage-collecting it (nothing "calls" it).
     * ------------------------------------------------------------------ */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* ------------------------------------------------------------------
     * .text - program code and read-only literals. Lives in Flash.
     * ------------------------------------------------------------------ */
    .text :
    {
        . = ALIGN(4);
        *(.text)            /* normal code                              */
        *(.text*)           /* code from -ffunction-sections            */
        *(.rodata)          /* const data, string literals              */
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;         /* end of code; start of init-data image    */
    } > FLASH

    /* ------------------------------------------------------------------
     * .data - INITIALISED globals (e.g. `int x = 5;`).
     *
     * Tricky part: the *initial values* must survive power-off, so they
     * are stored in Flash, but the variables must be *writable*, so they
     * live in RAM at run time. The linker therefore gives .data two
     * addresses:
     *     VMA (run address) -> in RAM   (where code accesses the variable)
     *     LMA (load address)-> in Flash (where the init image is stored)
     * The startup code copies the image from LMA to VMA before main().
     *
     * _sidata = LMA (source, in Flash) ; _sdata/_edata = VMA (dest, RAM).
     * ------------------------------------------------------------------ */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;         /* start of .data in RAM                     */
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;         /* end of .data in RAM                       */
    } > RAM AT > FLASH      /* VMA in RAM, LMA in FLASH                  */

    /* ------------------------------------------------------------------
     * .bss - ZERO-INITIALISED globals (e.g. `int y;`).
     * Occupies no space in Flash; startup code zeroes this RAM region.
     * ------------------------------------------------------------------ */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;          /* start of .bss in RAM                      */
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;          /* end of .bss in RAM                        */
        __bss_end__ = _ebss;
    } > RAM

    /* 'end' / '_end' mark the start of the heap. newlib's _sbrk (used by
     * printf for its stdio buffers) needs this symbol. We use no malloc in
     * our own code, but defining it lets a retargeted printf link cleanly.
     * PROVIDE = only define if the program does not define it itself.    */
    PROVIDE(end  = _ebss);
    PROVIDE(_end = _ebss);

    /* ------------------------------------------------------------------
     * ._user_heap_stack - a guard region that makes the linker FAIL at
     * build time if the requested minimum heap+stack would not fit in
     * the remaining RAM. We use no dynamic memory (MISRA), so the heap
     * is 0, but we keep a minimum stack reservation as a safety check.
     * ------------------------------------------------------------------ */
    _Min_Heap_Size  = 0;
    _Min_Stack_Size = 0x400;   /* reserve at least 1 KB for the stack    */

    ._user_heap_stack :
    {
        . = ALIGN(8);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM

    /* Discard exception-unwinding tables we do not use on bare metal. */
    /DISCARD/ :
    {
        *(.ARM.exidx)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
