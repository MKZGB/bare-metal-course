# Bare-Metal Embedded Systems Course — STM32VLDISCOVERY (QEMU)

A self-contained, hands-on course in **bare-metal** embedded programming on the
**STM32VLDISCOVERY** board (chip **STM32F100RBT6**, ARM Cortex-M3), built to run
entirely inside **QEMU** — no physical hardware required. You write every driver
yourself, directly against the hardware registers (no HAL, no CMSIS, no vendor
library), the way real firmware is built when you need to understand and control
every byte.

> **New here? Open [`01-arm-architecture/01-arm-architecture.html`](01-arm-architecture/01-arm-architecture.html) in a browser and start reading.** Each module is an HTML page; the modules link to each other and to the [glossary](glossary.html).
>
> 🌐 **Two languages.** Every module exists in English (`NN-name.html`) and Portuguese (`NN-name-pt.html`), with a language switcher in the top bar of each page. Portuguese readers can start at [`01-arm-architecture/01-arm-architecture-pt.html`](01-arm-architecture/01-arm-architecture-pt.html) — see also [`README-pt.md`](README-pt.md). The C reference solutions in `solutions/` are shared by both (code is language-neutral).

---

## Section 1 — What this course is

### Purpose
To take a programmer who **knows C and general computer architecture** but has
**no ARM or bare-metal experience** and make them comfortable writing register-level
drivers for an ARM Cortex-M microcontroller: GPIO, UART, timers, interrupts, DMA,
I²C and SPI — understanding not just *how* but *why*.

### How it teaches
The course **does not write the code for you.** Each module gives you:
1. **Theory** — the concept, the relevant registers and their bit fields (with
   exact addresses from the STM32F100 reference manual **RM0041**), and diagrams.
2. **A guided exercise** — a goal, the registers to use, a `.c` **skeleton with
   `TODO` markers you fill in**, step-by-step **hints that are hidden until you
   click them**, and the exact command to build and run it.
3. **Verification questions** — with answers hidden until you click.

Complete reference solutions live in [`solutions/`](solutions/). They are there if
you get truly stuck — but you will learn far more if you fight with the exercise
first.

### Target board & chip
| Property | Value |
|----------|-------|
| Board | STM32VLDISCOVERY |
| MCU | STM32F100RBT6 ("Value Line") |
| Core | ARM Cortex-M3 (ARMv7-M, Thumb-2), little-endian |
| Flash | 128 KB @ `0x08000000` |
| SRAM | 8 KB @ `0x20000000` |
| Default clock | 8 MHz internal HSI (no PLL configured in this course) |
| Reference manual | **RM0041** (STM32F100xx) |
| LEDs | LD3 green = **PC9**, LD4 blue = **PC8** |
| User button | B1 = **PA0** (active high) |

### What QEMU can and cannot emulate (read this — it is essential)
QEMU's `stm32vldiscovery` machine emulates the **CPU core and the USARTs faithfully**,
but it is **not a full chip model**. I verified each peripheral empirically; this
table tells you what you will actually observe, so you never waste time thinking
your correct code is "broken":

| Peripheral | Emulated in QEMU? | What you see |
|-----------|:-----------------:|--------------|
| **USART1/2/3** | ✅ Full | Real serial I/O. **This is your main output channel.** |
| **SysTick** | ✅ Full | Counts and interrupts correctly. Module 08 is fully observable. |
| **NVIC / exceptions** | ✅ Full | Interrupts dispatch correctly (e.g. UART RX IRQ works). |
| **GPIO** (all ports) | ❌ None | Writes are dropped; `IDR`/`ODR`/`CRL`/`CRH` read back `0`. **No LED lights, the button always reads 0.** Your code is still correct for real hardware. |
| **TIM2/3/4** | ❌ None | The counter stays frozen at 0; the update flag (`UIF`) never sets. |
| **DMA1** | ❌ None | No transfer occurs; the completion flag never sets. |
| **I²C1/2** | ❌ None | Registers read 0; no external device (e.g. an ADXL345) exists to talk to. |
| **SPI1** | ⚠️ Partial | Registers read back and `TXE`/`RXNE` flags behave, but there is **no slave**, so received data is meaningless. |

**Consequence:** modules for un-emulated peripherals (GPIO, timers, DMA, I²C) teach
the **register-level state machine** and are written so the program **never hangs**
waiting on a flag QEMU will never set — it explains the limitation and moves on. The
driver code is correct for a real STM32VLDISCOVERY; QEMU simply doesn't model the
silicon. Modules 06–08 and 10 give you genuinely visible, interactive results.

> **The single most important QEMU gotcha** (Module 06 covers it in detail): the
> machine has **three** USARTs, so a bare `-serial stdio` fails with
> *"cannot use stdio by multiple character devices."* You must give all three a
> backend. The shared Makefile's `run` target already does this for you:
> ```
> qemu-system-arm -M stm32vldiscovery -display none -serial stdio -serial null -serial null -kernel build/firmware.elf
> ```
> If UART output ever fails to appear, this routing (USART1 → stdio) is the fix.
> Quit QEMU with <kbd>Ctrl-A</kbd> then <kbd>X</kbd>.

---

## Section 2 — How to use it

### Prerequisites
- Comfortable with C (pointers, bitwise operators, `volatile`, fixed-width types).
- Basic computer-architecture literacy (registers, memory, stack).
- Ubuntu/Debian Linux (the commands below assume it; any Linux works).

### Toolchain setup
You need the ARM cross-compiler and QEMU:

```bash
# ARM bare-metal GCC toolchain (provides arm-none-eabi-gcc, objdump, etc.)
sudo apt update
sudo apt install gcc-arm-none-eabi

# QEMU with ARM system emulation (provides qemu-system-arm)
sudo apt install qemu-system-arm

# Verify
arm-none-eabi-gcc --version
qemu-system-arm -M help | grep stm32vldiscovery
```

If you cannot use `apt` (e.g. no root), download the prebuilt **Arm GNU Toolchain**
(`arm-none-eabi`) from <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>,
extract it anywhere, and pass its path to the build with
`PREFIX=/path/to/bin/arm-none-eabi-` (see below).

### Project layout
```
bare-metal-course/
├── README.md                 ← you are here
├── glossary.html             ← key terms, consistent across modules
├── common/                   ← shared infrastructure (reused by every module)
│   ├── startup.c             ← vector table + reset handler (.data/.bss init)
│   ├── linker.ld             ← memory map (Flash/SRAM) + section layout
│   ├── style.css             ← shared page styling
│   └── Makefile              ← one build for every module
├── solutions/                ← complete reference solutions (peek only if stuck)
│   ├── 04-gpio-output.c … 13-spi.c
├── 01-arm-architecture/ … 13-spi/   ← per module: NN-name.html (EN) + NN-name-pt.html (PT)
```

The **`common/`** folder is the heart of real bare-metal structure: a single
`startup.c` and `linker.ld` are reused by *every* exercise. You never copy build
boilerplate — you point the shared Makefile at your source file.

### How to build and run an exercise
Write your code in a file inside the module folder (e.g. `04-gpio-output/main.c`),
then, **from inside that module folder**:

```bash
# build only
make -f ../common/Makefile SRC=main.c

# build and run in QEMU
make -f ../common/Makefile SRC=main.c run

# remove build artifacts
make -f ../common/Makefile SRC=main.c clean
```

To build/run a **reference solution** instead, point `SRC` at it:

```bash
cd 06-uart-transmit
make -f ../common/Makefile SRC=../solutions/06-uart-transmit.c run
```

If your toolchain is not on `PATH`, add `PREFIX`:

```bash
make -f ../common/Makefile PREFIX=/opt/arm/bin/arm-none-eabi- SRC=main.c run
```

Quit QEMU with <kbd>Ctrl-A</kbd> then <kbd>X</kbd>.

### Recommended reading order & dependencies
Work through the modules in numeric order. The hard dependencies are:

```
 01 ARM Architecture ─► 02 Programmer's Model ─► 03 Memory-Mapped Registers
                                                          │
                                                          ▼
                                                 04 GPIO Output
                                                  │        │
                                          ┌───────┘        ▼
                                          ▼            05 GPIO Input
                                   06 UART Transmit ◄───────┘ (needs 04)
                                          │
                                          ▼
                                   07 UART Receive
                                          │
                                          ▼
                                   08 SysTick Timer
                                          │
                  ┌───────────────┬───────┼───────────────┐
                  ▼               ▼       ▼                ▼
        09 GP Timers     10 Interrupts (NVIC)     (06/08 outputs reused
        (needs 08)       (needs 04, 06, 08)        for visible results)
                  │               │
                  └──────┬────────┘
                         ▼
                   11 DMA (UART TX)  ─►  12 I²C  ─►  13 SPI
                   (needs 06)            (register-level)  (register-level)
```

| Module | Depends on | Why |
|--------|-----------|-----|
| 01 ARM Architecture | — | Foundation: RISC, load-store, Thumb-2. |
| 02 Programmer's Model | 01 | Registers, modes, vector table, the reset sequence. |
| 03 Memory-Mapped Registers | 02 | `volatile`, register structs, bit manipulation — used everywhere after. |
| 04 GPIO Output | 03 | First driver: clock gating, `CRL/CRH`, `BSRR`. |
| 05 GPIO Input | 04 | Adds `IDR` and input configuration. |
| 06 UART Transmit | 04 | First **visible** output; retargets `printf`. Reuses GPIO AF config. |
| 07 UART Receive | 06 | Adds `RXNE`/receive to the UART driver. |
| 08 SysTick Timer | 06 | Real timing; prints over UART. |
| 09 General-Purpose Timers | 08 | Time-base, output-compare, input-capture (register-level in QEMU). |
| 10 Interrupts (NVIC) | 04, 06, 08 | Vector table, NVIC enable, UART-RX & SysTick IRQs. |
| 11 DMA | 06 | UART-TX via DMA (register-level in QEMU). |
| 12 I²C | 06 | Register-level state machine (no slave in QEMU). |
| 13 SPI | 06 | Register-level state machine (registers emulated, no slave). |

> ⚠️ **A note on "MODER":** if you have seen STM32F4 tutorials, you may expect a
> `MODER` register for GPIO. **The STM32F100 (an STM32F1-family part) has no
> `MODER`.** It configures pins with `CRL`/`CRH` (4 bits per pin). This course uses
> the correct F1 registers throughout — see Module 04.

### Where the reference solutions live
[`solutions/NN-name.c`](solutions/) — one complete, heavily commented file per
coding module (04–13). They build and run with the shared Makefile exactly like
your own code. Modules 01–03 are conceptual and reveal their answers through the
in-page hints instead of a separate file.

---

*Built and verified against `arm-none-eabi-gcc` 13.2 and `qemu-system-arm` 8.2.
All register addresses follow STM32F100 **RM0041**.*
