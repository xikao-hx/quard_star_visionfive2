/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __QUARD_BOOTCTRL_H__
#define __QUARD_BOOTCTRL_H__

#include <linux/types.h>

struct spi_flash;

int quard_bootctrl_select_payload(struct spi_flash *flash,
				  unsigned int *payload_offset,
				  unsigned int *selected_bank);

#endif /* __QUARD_BOOTCTRL_H__ */
