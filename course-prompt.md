# Bare-Metal Embedded Systems Course — STM32VLDISCOVERY (QEMU)

Create a self-contained bare-metal embedded systems course for the **STM32VLDISCOVERY** board (STM32F100RBT6, Cortex-M3), targeting **QEMU emulation** (`qemu-system-arm -M stm32vldiscovery`).

## Pedagogical Rules (IMPORTANT)

- The course is for someone who **knows C and generic architectures** but has **no prior ARM or bare-metal experience** (intermediate level).
- **The AI must NOT do the work for the student.** It guides, explains, and provides exercises that the student must solve themselves.
- Each module provides theory, a guided exercise the student writes, and verification questions.
- Complete reference solutions go in a **separate `solutions/` folder** — the student should not be tempted to look, but they are there if stuck.

## Output Folder Structure (Ubuntu)

```
~/bare-metal-course/
├── README.md
├── glossary.html
├── common/
│   ├── startup.c            # vector table + reset handler (explained)
│   ├── linker.ld            # linker script (explained)
│   └── Makefile             # shared build rules for all modules
├── solutions/
│   ├── 04-gpio-output.c
│   ├── 05-gpio-input.c
│   └── ...                  # one reference solution per coding module
├── 01-arm-architecture/
│   └── index.html
├── 02-arm-programmer-model/
│   └── index.html
├── 03-memory-mapped-registers/
│   └── index.html
├── 04-gpio-output/
│   └── index.html
├── 05-gpio-input/
│   └── index.html
├── 06-uart-transmit/
│   └── index.html
├── 07-uart-receive/
│   └── index.html
├── 08-systick-timer/
│   └── index.html
├── 09-general-purpose-timers/
│   └── index.html
├── 10-interrupts-nvic/
│   └── index.html
├── 11-dma/
│   └── index.html
├── 12-i2c/
│   └── index.html
└── 13-spi/
    └── index.html
```

## Shared Infrastructure (`common/` folder)

- Provide a **single startup code** (`startup.c`): vector table, reset handler, `.data`/`.bss` initialization. Explain every part.
- Provide a **single linker script** (`linker.ld`): Flash at 0x08000000, SRAM at 0x20000000 for STM32F100RBT6 (128 KB Flash, 8 KB SRAM). Explain memory regions and sections.
- Provide a **shared Makefile** that compiles any module against the common startup/linker, using `arm-none-eabi-gcc`.
- Modules should reuse this infrastructure instead of repeating it — this teaches real bare-metal project structure.

## Each HTML module must contain

### 1. Theory Section
- Concept explanation at intermediate level.
- Register descriptions with bit fields explained — reference STM32F100 **RM0041** register addresses and bit names.
- Diagrams or ASCII art where helpful.
- Explicit references to the relevant RM0041 sections.

### 2. Guided Exercise Section
- A practical exercise in **C** (bare-metal, no HAL, no STM32 library, no CMSIS abstractions).
- The student writes the code. The HTML provides:
  - The exercise goal.
  - Which registers to use and why.
  - Step-by-step hints, **collapsed by default, revealed on click**.
  - A skeleton `.c` file (with `TODO` markers the student must fill in) shown in the HTML.
  - The exact QEMU command to run/test it.

### 3. Verification Questions
- 3–5 questions per module to confirm understanding.
- Answers **collapsed/hidden by default, revealed on click**.

## Module List (in recommended order)

1. ARM Design Philosophy & RISC Architecture
2. ARM Programmer's Model (registers, modes, exceptions)
3. Memory-Mapped Registers (MMIO, register structs)
4. GPIO Output Driver (MODER, BSRR)
5. GPIO Input Driver
6. UART Transmit Driver + retargeting printf
7. UART Receive Driver
8. SysTick Timer
9. General Purpose Timers (Output Compare, Input Capture)
10. Interrupt Programming (NVIC, GPIO, UART, SysTick, Timer IRQs)
11. DMA Driver (UART Transmitter)
12. I2C Driver (register-level only — see QEMU limitation note)
13. SPI Driver (register-level only — see QEMU limitation note)

## Reading Order & Dependencies

- The README must include an **index with the recommended sequence** and **explicit dependencies** between modules (e.g. "complete 03 before 04", "10 requires 04 and 08").

## QEMU Output Verification (CRITICAL)

- UART modules (06, 07) **must explain how to see output in QEMU** — using `-serial stdio` or semihosting. Without this the student writes a driver and sees nothing.
- **Module 06 must include an early sanity check**: a minimal "hello" over UART to confirm the student's setup actually shows output **before** building anything more complex on top.
- If UART output does not appear with the `stm32vldiscovery` machine, the README should note possible fixes (alternative serial routing or semihosting).

## Technical Constraints

- All register addresses must match **STM32F100RBT6 (RM0041)**.
- QEMU target: `qemu-system-arm -M stm32vldiscovery`.
- Toolchain: `arm-none-eabi-gcc`.
- Linker script and startup code must be explained and provided (in `common/`).
- **No HAL, no CMSIS abstractions** — direct register access only.
- All C code must follow **MISRA C** guidelines: thorough comments, no dynamic memory allocation, explicit fixed-width types (`uint32_t`, etc.), and memory-efficient practices.
- Modules **12 (I2C)** and **13 (SPI)** must clearly state that **no external device is emulated in QEMU** — focus is on register-level state-machine understanding, not communication with a real sensor (e.g. ADXL345).

## Glossary (`glossary.html`)

- A standalone glossary of key terms (MMIO, NVIC, vector table, BSRR, semihosting, reset handler, etc.) for consistency across modules.

## README.md (two sections, as required)

- **Section 1 — What this course is**: purpose, scope, target board, what QEMU can and cannot emulate.
- **Section 2 — How to use it**: prerequisites, toolchain setup (`arm-none-eabi-gcc`), QEMU setup, how to compile and run each exercise (using the shared Makefile), recommended reading order with dependencies, and where the reference solutions live.
