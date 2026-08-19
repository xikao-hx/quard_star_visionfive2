CURRENT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(CURRENT_DIR)/../../..)
KERN_DIR ?= $(PROJECT_ROOT)/linux
KBUILD_OUTPUT ?= $(PROJECT_ROOT)/work/linux

ccflags-y += -std=gnu99
ccflags-y += -I$(KERN_DIR)/drivers/mailbox

obj-m += starfive_ipi_mailbox.o
obj-m += starfive_ipi_mailbox-test.o

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KERN_DIR) O=$(KBUILD_OUTPUT) M=$(CURRENT_DIR) modules

clean:
	$(MAKE) -C $(KERN_DIR) O=$(KBUILD_OUTPUT) M=$(CURRENT_DIR) clean
