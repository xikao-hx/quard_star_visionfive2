/*
 * Copyright (c) 2016-2023, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef GPT_H
#define GPT_H

#include "efi.h"
#include "uuid.h"
#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"

#define PARTITION_TYPE_GPT		0xee
#define GPT_SIGNATURE			"EFI PART"

typedef struct gpt_entry {
	struct efi_guid		type_uuid;// 16
	struct efi_guid		unique_uuid;//16
	unsigned long long	first_lba;// 8
	unsigned long long	last_lba; // 8
	unsigned long long	attr; // 8
	unsigned short		name[EFI_NAMELEN]; //72
} gpt_entry_t;

typedef struct gpt_header {
	unsigned char		signature[8];
	unsigned int		revision;
	unsigned int		size;
	unsigned int		header_crc;
	unsigned int		reserved;
	unsigned long long	current_lba;
	unsigned long long	backup_lba;
	unsigned long long	first_lba;
	unsigned long long	last_lba;
	struct efi_guid		disk_uuid;
	/* starting LBA of array of partition entries */
	unsigned long long	part_lba;
	/* number of partition entries in array */
	unsigned int		list_num;
	/* size of a single partition entry (usually 128) */
	unsigned int		part_size;
	unsigned int		part_crc;
} __attribute__((packed)) gpt_header_t;

typedef int (*gpt_read_fn)(void *context, uint32_t offset, void *buffer,
			   uint32_t length);

int gpt_read_partitions(gpt_read_fn read_fn, void *context,
			struct quard_nor_info *info);

#endif /* GPT_H */
