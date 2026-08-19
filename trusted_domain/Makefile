##########################################################################################################################
# trusted_domain GCC compiler Makefile
##########################################################################################################################

# ------------------------------------------------
# Generic Makefile (based on gcc)
# ------------------------------------------------

######################################
# target
######################################
TARGET = trusted_fw
BOARD ?= visionfive2
######################################
# building variables
######################################
# debug build?
DEBUG = 1
# optimization
OPT = -Og


PROJECTBASE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PROJECTBASE := $(PROJECTBASE:/=)
TOP_DIR = $(PROJECTBASE)


#######################################
# binaries
#######################################
CROSS_COMPILE ?= riscv64-unknown-elf-
CC        = $(CROSS_COMPILE)gcc
AS        = $(CROSS_COMPILE)gcc -x assembler-with-cpp
OBJCOPY   = $(CROSS_COMPILE)objcopy
OBJDUMP   = $(CROSS_COMPILE)objdump
AR        = $(CROSS_COMPILE)ar
SZ        = $(CROSS_COMPILE)size
LD        = $(CROSS_COMPILE)ld
HEX       = $(OBJCOPY) -O ihex
BIN       = $(OBJCOPY) -O binary -S
GDB       = $(CROSS_COMPILE)gdb

#######################################
# paths
#######################################
# firmware library path
PERIFLIB_PATH =

# Build path
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

######################################
# source
######################################
# C sources
# C_SOURCES =  \
# 		${wildcard $(TOP_DIR)/FreeRTOS-Kernel/*.c} \
# 		${wildcard $(TOP_DIR)/FreeRTOS-Kernel/portable/GCC/RISC-V/*.c} \
# 		${wildcard $(TOP_DIR)/FreeRTOS-Kernel/portable/MemMang/heap_4.c} \
# 		${wildcard $(TOP_DIR)/driver/*.c} \
# 		${wildcard $(TOP_DIR)/stubs/*.c} \
# 		${wildcard $(TOP_DIR)/riscv/*.c} \
# 		${wildcard $(TOP_DIR)/*.c}
# C sources
C_SOURCES =  \
        $(wildcard $(TOP_DIR)/FreeRTOS-Kernel/*.c) \
        $(wildcard $(TOP_DIR)/FreeRTOS-Kernel/portable/GCC/RISC-V/*.c) \
        $(wildcard $(TOP_DIR)/FreeRTOS-Kernel/portable/MemMang/heap_4.c) \
        $(shell find $(TOP_DIR)/driver -name "*.c") \
		$(shell find $(TOP_DIR)/board -name "*.c" 2>/dev/null) \
		$(shell find $(TOP_DIR)/app -name "*.c") \
        $(wildcard $(TOP_DIR)/stubs/*.c) \
        $(wildcard $(TOP_DIR)/riscv/*.c) \
        $(wildcard $(TOP_DIR)/*.c)

# VSHELL uses the mailbox router. Do not link the direct mailbox test client,
# which would claim the same IPI channels and bypass router dispatch.
C_SOURCES := $(filter-out $(TOP_DIR)/app/mailbox_test.c,$(C_SOURCES))

# ASM sources
ASM_SOURCES =  \
		${wildcard $(TOP_DIR)/*.S} \
		${wildcard $(TOP_DIR)/FreeRTOS-Kernel/portable/GCC/RISC-V/*.S}

######################################
# firmware library
######################################
PERIFLIB_SOURCES =


#######################################
# CFLAGS
#######################################
MCU = -march=rv64imafdc -mabi=lp64d -mcmodel=medany -msmall-data-limit=8 -fmessage-length=0 -fsigned-char

# macros for gcc
# AS defines
AS_DEFS = 

# C defines
C_DEFS =

ifeq ($(BOARD),visionfive2)
C_DEFS += -DBOARD_JH7110
AS_DEFS += -DBOARD_JH7110
LD_FILE = link-jh7110.lds
else
LD_FILE = link.lds
endif

DRIVER_INC = $(shell find $(TOP_DIR)/driver -type d)
APP_INC = $(shell find $(TOP_DIR)/app -type d)
# AS includes
AS_INCLUDES = \
		-I $(TOP_DIR)/riscv \
		-I $(TOP_DIR)/driver
AS_INCLUDES += $(addprefix -I ,$(DRIVER_INC))
AS_INCLUDES += $(addprefix -I ,$(APP_INC))

# C includes
# C_INCLUDES = \
# 		-I $(TOP_DIR)/FreeRTOS-Kernel/include \
# 		-I $(TOP_DIR)/FreeRTOS-Kernel/portable/GCC/RISC-V \
# 		-I $(TOP_DIR)/riscv \
# 		-I $(TOP_DIR)/stubs \
# 		-I $(TOP_DIR)/driver \
# 		-I $(TOP_DIR)

C_INCLUDES = \
		-I $(TOP_DIR)/FreeRTOS-Kernel/include \
		-I $(TOP_DIR)/FreeRTOS-Kernel/portable/GCC/RISC-V \
		-I $(TOP_DIR)/riscv \
		-I $(TOP_DIR)/stubs \
		-I $(TOP_DIR)

# 自动获取 driver 目录下所有的子目录并加上 -I 前缀
C_INCLUDES += $(addprefix -I ,$(DRIVER_INC))
C_INCLUDES += $(addprefix -I ,$(APP_INC))
C_INCLUDES += -I $(TOP_DIR)/board

# compile gcc flags
ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

# Generate dependency information
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)"

#######################################
# LDFLAGS
#######################################
# link script
LDSCRIPT = $(PROJECTBASE)/$(LD_FILE)

# libraries
LIBS = -lm 
LIBDIR =
LDFLAGS = $(MCU) -nostartfiles -T$(LDSCRIPT) $(LIBDIR) $(LIBS) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map -Wl,--gc-sections

# default action: build all
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).lst


#######################################
# build the application
#######################################
# list of objects
OBJECTS = $(addprefix $(OBJ_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
# list of ASM program objects
OBJECTS += $(addprefix $(OBJ_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

$(OBJ_DIR)/%.o: %.c Makefile | $(OBJ_DIR)
	@echo CC $(notdir $@)
	@$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(OBJ_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(OBJ_DIR)/%.o: %.S Makefile | $(OBJ_DIR)
	@echo AS $(notdir $@)
	@$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT) Makefile 
	@echo LD $(notdir $@)
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	@echo OBJCOPY $(notdir $@)
	@$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	@echo OBJCOPY $(notdir $@)
	@$(BIN) $< $@

$(BUILD_DIR)/%.lst: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	@echo OBJDUMP $(notdir $@)
	@$(OBJDUMP) --source --demangle --disassemble --reloc --wide $< > $@
	@$(SZ) --format=berkeley $<

$(BUILD_DIR):
	mkdir $@

ifeq ($(OBJ_DIR), $(wildcard $(OBJ_DIR)))
else
$(OBJ_DIR):$(BUILD_DIR)
	mkdir $@
endif

#######################################
# clean up
#######################################
clean:
	-rm -fR $(BUILD_DIR)

#######################################
# use gdb debug
#######################################
debug:
	$(GDB) -x $(BUILD_DIR)/../gdb.sh

#######################################
# dependencies
#######################################
#-include $(shell mkdir .dep 2>/dev/null) $(wildcard .dep/*)

# *** EOF ***
