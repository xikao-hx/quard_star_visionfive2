/*
 * Copyright (c) 2016-2024, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>

//#include <lib/cassert.h>
#include "efi.h"
#include "uuid.h"

# define PLAT_PARTITION_MAX_ENTRIES	128

# define PLAT_PARTITION_BLOCK_SIZE	512

#define LEGACY_PARTITION_BLOCK_SIZE	512

#define LBA(n) ((unsigned long long)(n) * PLAT_PARTITION_BLOCK_SIZE)


#define TYPE_FLASH 0
#define TYPE_MMC 1

typedef struct partition_entry {
	uint64_t		start;
	uint64_t		length;
	char			name[EFI_NAMELEN];
	struct efi_guid		part_guid;
	struct efi_guid		type_guid;
} partition_entry_t;

typedef struct partition_entry_list {
	partition_entry_t	list[PLAT_PARTITION_MAX_ENTRIES];
	unsigned int		entry_count;
} partition_entry_list_t;

const partition_entry_t *get_partition_entry(const char *name, int type);

int get_partition_index(const char *name, int type);

void partition_init(unsigned int image_id);
int gpt_partition_init(char *dev);

int32_t get_partition_size_by_name(const char *name, int type, uint32_t *size);

#endif /* PARTITION_H */
