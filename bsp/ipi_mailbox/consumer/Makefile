CURRENT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(CURRENT_DIR)/../../..)
KERN_DIR ?= $(PROJECT_ROOT)/linux
KBUILD_OUTPUT ?= $(PROJECT_ROOT)/work/linux

ccflags-y += -std=gnu99
ccflags-y += -I$(CURRENT_DIR)
ccflags-y += -I$(PROJECT_ROOT)/common_inc/bsp

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KERN_DIR) O=$(KBUILD_OUTPUT) M=$(CURRENT_DIR) modules

clean:
	$(MAKE) -C $(KERN_DIR) O=$(KBUILD_OUTPUT) M=$(CURRENT_DIR) clean

obj-m   += quard_mbox_router.o
obj-m   += quard_remote_console.o
obj-m   += quard_log.o
obj-m   += quard_nor_client.o
