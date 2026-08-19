/*
 * Copyright (c) 2016-2022, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define LOG_TAG "GPT"
#include "elog.h"
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "efi.h"
#include "gpt.h"
#include <stdbool.h>
#include "pflash.h"

int unicode_to_ascii(unsigned short *str_in, unsigned char *str_out)
{
	uint8_t *name;
	int i;

	assert((str_in != NULL) && (str_out != NULL));

	name = (uint8_t *)str_in;

	assert(name[0] != '\0');

	/* check whether the unicode string is valid */
	for (i = 1; i < (EFI_NAMELEN << 1); i += 2) {
		if (name[i] != '\0') {
			return -EINVAL;
		}
	}
	/* convert the unicode string to ascii string */
	for (i = 0; i < (EFI_NAMELEN << 1); i += 2) {
		str_out[i >> 1] = name[i];
		if (name[i] == '\0') {
			break;
		}
	}
	return 0;
}

int parse_gpt_entry(gpt_header_t *header, struct quard_nor_info *pinfo) {

	uint32_t entry_start_offset = (uint32_t)(header->part_lba * 512); 
	uint32_t max_entries = (header->list_num > 128) ? 128 : header->list_num;

	/* gpt分区 */
	pinfo->parts[0].offset = 0;
    pinfo->parts[0].length = 256 * 1024; 
    strcpy(pinfo->parts[0].name, "gpt");

	/* 遍历分区entry */
	uint32_t part_no = 1;
	gpt_entry_t entry;
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t current_entry_offset = entry_start_offset + (i * header->part_size);	
        bool is_empty = true;
        
        if (ota_get_entry(&entry, current_entry_offset, &is_empty) != 0) {
            LOG_W("ota_get_entry %d failed!", i);
            return -1;
        }
        
        if (is_empty) {
            break;
        }

		nor_part_t *part = &pinfo->parts[part_no];
		part->offset = (uint32_t)(entry.first_lba * 512);
		part->length = (uint32_t)((entry.last_lba - entry.first_lba + 1) * 512);
		unicode_to_ascii(entry.name, (uint8_t *)part->name);

        part_no++;
    }

    pinfo->nparts = part_no;

	return 0;
}
