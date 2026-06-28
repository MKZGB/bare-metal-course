# ======================================================================
# common/Makefile - shared build rules for every module in the course
# ======================================================================
#
# This one Makefile builds ANY module against the shared startup code
# (startup.c) and linker script (linker.ld). You never copy build rules
# into a module; you point this Makefile at your source file instead.
#
# USAGE (run from inside a module folder, e.g. 04-gpio-output/):
#
#     make -f ../common/Makefile SRC=main.c          # build  -> build/firmware.elf
#     make -f ../common/Makefile SRC=main.c run       # build + run in QEMU
#     make -f ../common/Makefile SRC=main.c clean      # remove build/
#
# To build a reference solution from the repo root, for example:
#
#     make -f common/Makefile SRC=solutions/04-gpio-output.c run
#
# ----------------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------------
# PREFIX is the cross-compiler prefix. Override it if your toolchain is
# installed somewhere not on PATH, e.g.:
#     make -f ../common/Makefile PREFIX=/opt/arm/bin/arm-none-eabi- SRC=main.c
PREFIX ?= arm-none-eabi-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

# Directory this Makefile lives in (the common/ folder), resolved no
# matter where 'make' is invoked from.
COMMON_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

STARTUP := $(COMMON_DIR)startup.c
LDSCRIPT := $(COMMON_DIR)linker.ld

# ----------------------------------------------------------------------
# Module source - MUST be supplied on the command line as SRC=...
# ----------------------------------------------------------------------
ifndef SRC
$(error You must specify the module source, e.g.  make -f ../common/Makefile SRC=main.c)
endif

BUILD := build
TARGET := $(BUILD)/firmware

# ----------------------------------------------------------------------
# CPU / architecture flags for the Cortex-M3 in the STM32F100.
#   -mcpu=cortex-m3   : generate Cortex-M3 instructions
#   -mthumb           : Cortex-M only runs the Thumb-2 instruction set
# ----------------------------------------------------------------------
ARCHFLAGS = -mcpu=cortex-m3 -mthumb

# Compiler flags:
#   -std=c11          : modern C, with fixed-width <stdint.h> types
#   -Og -g3           : light optimisation that keeps debugging sane
#   -Wall -Wextra     : maximum warnings (MISRA-friendly discipline)
#   -ffreestanding    : no hosted C library assumptions (bare metal)
#   -ffunction-sections / -fdata-sections + --gc-sections: drop unused code
CFLAGS = $(ARCHFLAGS) -std=c11 -Og -g3 -Wall -Wextra \
         -ffreestanding -ffunction-sections -fdata-sections \
         -I$(COMMON_DIR)

# Linker flags:
#   -T linker.ld         : use our memory map
#   -nostartfiles        : do NOT pull in the toolchain's crt0; we supply our own
#                          startup.c (Reset_Handler). We still let the linker
#                          pull in newlib-nano so that, in the UART module, a
#                          retargeted printf() is available to anyone who wants
#                          it. Modules that never call a libc function link none
#                          of it (library objects are only included on demand).
#   --specs=nano.specs   : newlib-nano (small, integer-only printf)
#   --specs=nosys.specs  : default stub syscalls; you override _write() to send
#                          characters to the UART (see Module 06).
#   --gc-sections        : remove unreferenced sections
#   -Map                 : produce a map file for inspection
LDFLAGS = $(ARCHFLAGS) -T$(LDSCRIPT) -nostartfiles \
          --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map=$(TARGET).map

# All sources: the module + the shared startup code.
SRCS = $(SRC) $(STARTUP)

# ----------------------------------------------------------------------
# Targets
# ----------------------------------------------------------------------
.PHONY: all run clean size

all: $(TARGET).elf $(TARGET).bin size

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET).elf: $(SRCS) $(LDSCRIPT) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(TARGET).elf
	$(SIZE) $(TARGET).elf

# Run the ELF in QEMU.
#
# IMPORTANT QEMU GOTCHA: the stm32vldiscovery machine instantiates THREE
# USARTs (USART1, USART2, USART3), and QEMU insists every serial device
# has a backend. A bare "-serial stdio" therefore fails with:
#     "cannot use stdio by multiple character devices"
# The fix is to give all three a backend: USART1 -> your terminal (stdio),
# USART2 and USART3 -> discarded (null). USART1 is the one this course uses.
#
#   -display none : no graphical window (this is a headless board anyway)
#   -serial stdio : USART1  -> your terminal  (what you type / see)
#   -serial null  : USART2  -> discarded
#   -serial null  : USART3  -> discarded
#
# To quit QEMU: press Ctrl-A, release, then press X.
QEMU = qemu-system-arm
QEMUFLAGS = -M stm32vldiscovery -display none \
            -serial stdio -serial null -serial null \
            -kernel $(TARGET).elf

run: $(TARGET).elf
	$(QEMU) $(QEMUFLAGS)

clean:
	rm -rf $(BUILD)
