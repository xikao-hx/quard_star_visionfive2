# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2016, NVIDIA CORPORATION.
#

obj-$(CONFIG_$(SPL_)DM_MAILBOX) += mailbox-uclass.o
obj-$(CONFIG_SANDBOX_MBOX) += sandbox-mbox.o
obj-$(CONFIG_SANDBOX_MBOX) += sandbox-mbox-test.o
obj-$(CONFIG_STM32_IPCC) += stm32-ipcc.o
obj-$(CONFIG_TEGRA_HSP) += tegra-hsp.o
obj-$(CONFIG_K3_SEC_PROXY) += k3-sec-proxy.o
obj-$(CONFIG_ZYNQMP_IPI) += zynqmp-ipi.o
ifeq ($(CONFIG_SPL_BUILD),)
obj-$(CONFIG_QUARD_IPI_MAILBOX) += quard_ipi_mailbox.o
obj-$(CONFIG_QUARD_MBOX_ROUTER) += quard_mbox_router.o
endif
