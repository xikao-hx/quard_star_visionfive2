# SPDX-License-Identifier: GPL-2.0+
#
# Copyright (C) 2022-2023 StarFive Technology Co., Ltd.
#

obj-y	:= starfive_visionfive2.o

obj-$(CONFIG_SPL_BUILD) += spl.o
obj-$(CONFIG_ID_EEPROM) += visionfive2-i2c-eeprom.o

ifndef CONFIG_SPL_BUILD
obj-$(CONFIG_QUARD_BOOTCTRL) += quard_bootctrl.o
CFLAGS_quard_bootctrl.o += -I$(srctree)/../common_inc/bsp
endif
