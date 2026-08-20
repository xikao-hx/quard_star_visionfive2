
CURRENT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(CURRENT_DIR)/../..)
CC := $(CROSS_COMPILE)gcc
CPPFLAGS += -I$(PROJECT_ROOT)/common_inc/bsp
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all clean

all: soc_logd

soc_logd: soc_logd.c quard_soc_log.h $(PROJECT_ROOT)/common_inc/bsp/quard_log.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ soc_logd.c

clean:
	$(RM) soc_logd
