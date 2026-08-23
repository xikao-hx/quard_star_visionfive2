/* SPDX-License-Identifier: BSD-3-Clause */
#include "gpt.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../../common_inc/bsp/quard_nor_layout.h"

#define GPT_REVISION_1_0       0x00010000U
#define GPT_HEADER_MIN_SIZE    92U
#define GPT_HEADER_LOCAL_LBA   1U
#define GPT_ENTRY_LOCAL_LBA    2U
#define GPT_ENTRY_COUNT        128U
#define GPT_PROTECTIVE_TYPE    0xEEU
#define MBR_PARTITION_OFFSET   446U
#define MBR_SIGNATURE_OFFSET   510U

static uint16_t read_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const void *buffer, uint32_t length)
{
	const uint8_t *data = buffer;
	uint32_t i;
	uint32_t bit;

	for (i = 0; i < length; i++) {
		crc ^= data[i];
		for (bit = 0; bit < 8U; bit++)
			crc = (crc >> 1) ^ (0xEDB88320U &
					      (0U - (crc & 1U)));
	}
	return crc;
}

static bool guid_is_zero(const struct efi_guid *guid)
{
	const uint8_t *bytes = (const uint8_t *)guid;
	uint32_t i;

	for (i = 0; i < sizeof(*guid); i++) {
		if (bytes[i] != 0U)
			return false;
	}
	return true;
}

static int decode_name(const unsigned short *source, char *destination)
{
	uint32_t i;
	bool terminated = false;

	for (i = 0; i < EFI_NAMELEN; i++) {
		uint16_t character = source[i];

		if (character == 0U) {
			terminated = true;
			break;
		}
		if (character > 0x7FU || i >= QUARD_NOR_PARTNAME_MAX - 1U)
			return -EINVAL;
		destination[i] = (char)character;
	}
	if (!terminated || i == 0U)
		return -EINVAL;
	destination[i] = '\0';
	return 0;
}

static bool names_are_unique(const struct quard_nor_info *info,
			     const char *name)
{
	uint32_t i;

	for (i = 0; i < info->nparts; i++) {
		if (strcmp(info->parts[i].name, name) == 0)
			return false;
	}
	return true;
}

static int validate_mbr(gpt_read_fn read_fn, void *context, uint8_t *sector)
{
	uint32_t flash_lbas = QUARD_NOR_FLASH_SIZE / QUARD_NOR_SECTOR_SIZE;
	const uint8_t *entry = sector + MBR_PARTITION_OFFSET;
	int ret;

	ret = read_fn(context, QUARD_NOR_GPT_OFFSET, sector,
		      QUARD_NOR_SECTOR_SIZE);
	if (ret != 0)
		return ret;
	if (read_le16(sector + MBR_SIGNATURE_OFFSET) != 0xAA55U ||
	    entry[4] != GPT_PROTECTIVE_TYPE || read_le32(entry + 8) != 1U ||
	    read_le32(entry + 12) != flash_lbas - 1U)
		return -EINVAL;
	return 0;
}

static int read_and_validate_header(gpt_read_fn read_fn, void *context,
				    uint8_t *sector, gpt_header_t *header)
{
	uint32_t stored_crc;
	uint32_t calculated_crc;
	uint64_t entries_end;
	uint64_t flash_lbas = QUARD_NOR_FLASH_SIZE / QUARD_NOR_SECTOR_SIZE;
	int ret;

	ret = read_fn(context,
		      QUARD_NOR_GPT_OFFSET + GPT_HEADER_LOCAL_LBA *
		      QUARD_NOR_SECTOR_SIZE,
		      sector, QUARD_NOR_SECTOR_SIZE);
	if (ret != 0)
		return ret;
	memcpy(header, sector, sizeof(*header));
	if (memcmp(header->signature, GPT_SIGNATURE, 8) != 0 ||
	    header->revision != GPT_REVISION_1_0 ||
	    header->size != GPT_HEADER_MIN_SIZE ||
	    header->size > QUARD_NOR_SECTOR_SIZE || header->reserved != 0U ||
	    header->current_lba != GPT_HEADER_LOCAL_LBA ||
	    header->part_lba != GPT_ENTRY_LOCAL_LBA ||
	    header->backup_lba != flash_lbas - 1U || header->first_lba != 0U ||
	    header->last_lba != flash_lbas - 1U ||
	    header->list_num != GPT_ENTRY_COUNT ||
	    header->part_size != sizeof(gpt_entry_t))
		return -EINVAL;

	entries_end = header->part_lba * QUARD_NOR_SECTOR_SIZE +
		      (uint64_t)header->list_num * header->part_size;
	if (entries_end > QUARD_NOR_GPT_SIZE)
		return -ERANGE;

	stored_crc = header->header_crc;
	memset(sector + offsetof(gpt_header_t, header_crc), 0,
	       sizeof(header->header_crc));
	calculated_crc = ~crc32_update(~0U, sector, header->size);
	if (calculated_crc != stored_crc)
		return -EBADMSG;
	return 0;
}

static int validate_partition(const gpt_entry_t *entry,
			      struct quard_nor_info *info, uint32_t *previous_end)
{
	uint64_t offset;
	uint64_t length;
	nor_part_t *part;
	int ret;

	if (entry->first_lba > entry->last_lba ||
	    entry->last_lba >= QUARD_NOR_FLASH_SIZE / QUARD_NOR_SECTOR_SIZE ||
	    info->nparts >= QUARD_NOR_MAX_PARTS)
		return -ERANGE;
	offset = entry->first_lba * QUARD_NOR_SECTOR_SIZE;
	length = (entry->last_lba - entry->first_lba + 1U) *
		 QUARD_NOR_SECTOR_SIZE;
	if (offset > UINT32_MAX || length > UINT32_MAX ||
	    offset + length > QUARD_NOR_FLASH_SIZE || offset < *previous_end)
		return -ERANGE;

	part = &info->parts[info->nparts];
	memset(part, 0, sizeof(*part));
	ret = decode_name(entry->name, part->name);
	if (ret != 0 || !names_are_unique(info, part->name))
		return -EINVAL;
	part->offset = (uint32_t)offset;
	part->length = (uint32_t)length;
	*previous_end = part->offset + part->length;
	info->nparts++;
	return 0;
}

int gpt_read_partitions(gpt_read_fn read_fn, void *context,
			struct quard_nor_info *info)
{
	uint8_t sector[QUARD_NOR_SECTOR_SIZE];
	gpt_header_t header;
	gpt_entry_t entry;
	uint32_t entry_crc = ~0U;
	uint32_t previous_end = 0U;
	uint32_t i;
	bool empty_seen = false;
	bool canonical_gpt_seen = false;
	int ret;

	if (read_fn == NULL || info == NULL)
		return -EINVAL;
	info->nparts = 0U;
	memset(info->parts, 0, sizeof(info->parts));
	ret = validate_mbr(read_fn, context, sector);
	if (ret != 0)
		return ret;
	ret = read_and_validate_header(read_fn, context, sector, &header);
	if (ret != 0)
		return ret;

	for (i = 0; i < header.list_num; i++) {
		uint32_t local_offset = (uint32_t)header.part_lba *
			QUARD_NOR_SECTOR_SIZE + i * header.part_size;

		ret = read_fn(context, QUARD_NOR_GPT_OFFSET + local_offset,
			      &entry, sizeof(entry));
		if (ret != 0)
			return ret;
		entry_crc = crc32_update(entry_crc, &entry, sizeof(entry));
		if (guid_is_zero(&entry.type_uuid)) {
			empty_seen = true;
			continue;
		}
		if (empty_seen)
			return -EINVAL;
		ret = validate_partition(&entry, info, &previous_end);
		if (ret != 0)
			return ret;
		if (strcmp(info->parts[info->nparts - 1U].name,
			   QUARD_NOR_GPT_NAME) == 0) {
			const nor_part_t *part = &info->parts[info->nparts - 1U];

			if (part->offset != QUARD_NOR_GPT_OFFSET ||
			    part->length != QUARD_NOR_GPT_SIZE)
				return -EINVAL;
			canonical_gpt_seen = true;
		}
	}

	if (~entry_crc != header.part_crc)
		return -EBADMSG;
	if (info->nparts != QUARD_NOR_PARTITION_COUNT ||
	    info->nparts != QUARD_NOR_EXPECTED_PARTS || !canonical_gpt_seen)
		return -EINVAL;
	return 0;
}
