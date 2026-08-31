
MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REPO_ROOT := $(abspath $(MAKEFILE_DIR)/../..)

$(info MAKEFILE_DIR is $(MAKEFILE_DIR))

CROSS_COMPILE ?= $(REPO_ROOT)/work/buildroot_initramfs/host/bin/riscv64-buildroot-linux-gnu-

CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AR := $(CROSS_COMPILE)ar
RANLIB := $(CROSS_COMPILE)ranlib

CFLAGS := -Wall -fPIC -O0 -march=rv64imafdc -mabi=lp64d
LDFLAGS := -shared

MAJOR_VERSION := 1
MINOR_VERSION := 0
PATCH_VERSION := 0

LIB_DIR := lib
APP_DIR := app
BUILD_DIR := build

LIB_SOURCES := $(wildcard $(LIB_DIR)/*.c)
LIB_OBJS := $(patsubst $(LIB_DIR)/%.c, $(BUILD_DIR)/%.o, $(LIB_SOURCES))
LIB_NAME := bh_ota
LIB_VERSIONED_TARGET := $(BUILD_DIR)/lib$(LIB_NAME).so.$(MAJOR_VERSION).$(MINOR_VERSION).$(PATCH_VERSION)
# LIB_TARGET := $(BUILD_DIR)/lib$(LIB_NAME).a
LIB_MAJOR_LINK := $(BUILD_DIR)/lib$(LIB_NAME).so.$(MAJOR_VERSION)
LIB_SO_LINK := $(BUILD_DIR)/lib$(LIB_NAME).so

APP_SOURCES := $(wildcard $(APP_DIR)/*.c)
APP_OBJS := $(patsubst $(APP_DIR)/%.c, $(BUILD_DIR)/%.o, $(APP_SOURCES))
APP_NAMES := $(patsubst $(APP_DIR)/%.c,%,$(APP_SOURCES))
APP_TARGETS := $(patsubst %,$(BUILD_DIR)/%,$(APP_NAMES))

# ARFLAGS := rcs
all: $(LIB_VERSIONED_TARGET) $(LIB_SO_LINK) $(APP_TARGETS)
# all: $(LIB_TARGET) $(APP_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# $(LIB_TARGET): $(LIB_OBJS) | $(BUILD_DIR)
# 	$(AR) $(ARFLAGS) $@ $^
# 	@echo "Static library built: $@"

$(LIB_VERSIONED_TARGET): $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -Wl,-soname,$(notdir $(LIB_MAJOR_LINK)) -o $@ $^
	@echo "Shared library built: $@"
	@ln -sf $(notdir $@) $(LIB_MAJOR_LINK)
	@ln -sf $(notdir $@) $(LIB_SO_LINK)


# $(APP_TARGET): $(APP_OBJS) $(LIB_TARGET) | $(BUILD_DIR)
$(BUILD_DIR)/%: $(BUILD_DIR)/%.o $(LIB_SO_LINK) | $(BUILD_DIR)
	$(CC) -o $@ $^ -L$(BUILD_DIR) -l$(LIB_NAME) -I./lib
	@echo "Application built: $@"

$(BUILD_DIR)/%.o: $(LIB_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled library object: $@"

$(BUILD_DIR)/%.o: $(APP_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@ -I./lib
	@echo "Compiled application object: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned up."

.PHONY: all clean

H_FILES = ./lib/bh_ota_hal.hello
A_FILES = ./build/libbh_ota.a
# SO_FILES = ./build/libbh_ota.so*
BIN_FILES = $(APP_TARGETS)
SCRIPT_FILES = ./create_link.sh
INST_LIBDIR ?= ./__install/platform/lib/
INST_INCLUDEDIR ?= ./__install/common_inc/
INST_BINDIR ?= ./__install/platform/bin

.PHONY: install
install: 
	@mkdir -p $(INST_LIBDIR)
	@mkdir -p $(INST_INCLUDEDIR)
	@mkdir -p $(INST_BINDIR)
	@for file in $(H_FILES); do \
	    echo Installing $$file to $(INST_INCLUDEDIR); \
	    install -m 644 $$file $(INST_INCLUDEDIR); \
	done

# cp -a $(SO_FILES) $(INST_LIBDIR)/
	cp -a $(A_FILES) $(INST_LIBDIR)/
	cp -a $(BIN_FILES) $(INST_BINDIR)/
	cp -a $(SCRIPT_FILES) $(INST_BINDIR)/
