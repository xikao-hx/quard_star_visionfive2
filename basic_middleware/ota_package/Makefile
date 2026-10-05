MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

$(info MAKEFILE_DIR is $(MAKEFILE_DIR))

CROSS_COMPILE = riscv64-unknown-linux-gnu-
REPO_ROOT := $(abspath $(MAKEFILE_DIR)/../..)
TARGET_SYSROOT ?= $(REPO_ROOT)/work/buildroot_rootfs/host/riscv64-buildroot-linux-gnu/sysroot
OPENSSL_INCLUDE_DIR := $(TARGET_SYSROOT)/usr/include
OTA_INFO_DIR := ../ota_info
OTA_INFO_INCLUDE_DIR := $(OTA_INFO_DIR)/lib
OTA_INFO_LIB_DIR := $(OTA_INFO_DIR)/build
OTA_INFO_SHARED_LIB := $(OTA_INFO_LIB_DIR)/libbh_ota.so
CRYPTO_SHARED_LIB := $(TARGET_SYSROOT)/usr/lib/libcrypto.so.1.1

CC := $(CROSS_COMPILE)gcc

CFLAGS := -Wall -fPIC -O0 -march=rv64imafdc -mabi=lp64d \
	-I$(OPENSSL_INCLUDE_DIR) -I./lib -I$(OTA_INFO_INCLUDE_DIR)
LDFLAGS := -Wl,-rpath-link,$(OTA_INFO_LIB_DIR) -Wl,-rpath-link,$(TARGET_SYSROOT)/usr/lib

APP_DIR := app
LIB_DIR := lib
BUILD_DIR := build

APP_SOURCES := $(wildcard $(APP_DIR)/*.c)
LIB_SOURCES := $(wildcard $(LIB_DIR)/*.c)
APP_OBJS := $(patsubst $(APP_DIR)/%.c,$(BUILD_DIR)/%.o,$(APP_SOURCES))
LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SOURCES))
APP_NAMES := $(patsubst $(APP_DIR)/%.c,%,$(APP_SOURCES))
APP_TARGETS := $(patsubst %,$(BUILD_DIR)/%,$(APP_NAMES))

all: $(APP_TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: $(BUILD_DIR)/%.o $(LIB_OBJS) $(OTA_INFO_SHARED_LIB) $(CRYPTO_SHARED_LIB) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(BUILD_DIR)/$*.o $(LIB_OBJS) $(OTA_INFO_SHARED_LIB) $(CRYPTO_SHARED_LIB)
	@echo "Application built: $@"

$(BUILD_DIR)/%.o: $(APP_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled application object: $@"

$(BUILD_DIR)/%.o: $(LIB_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled library object: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned up."

.PHONY: all clean install

BIN_FILES = $(APP_TARGETS)
INST_BINDIR ?= ./__install/platform/bin

install:
	@mkdir -p $(INST_BINDIR)
	cp -a $(BIN_FILES) $(INST_BINDIR)/
