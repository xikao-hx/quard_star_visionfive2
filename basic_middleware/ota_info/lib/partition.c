/*
 * Copyright (c) 2016-2024, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>

#include "efi.h"
#include "partition.h"
#include "gpt.h"
#include "mbr.h"
#include "tf_crc32.h"

static uint8_t mbr_sector[PLAT_PARTITION_BLOCK_SIZE];

static partition_entry_list_t flash_parts_list;
static partition_entry_list_t mmc_parts_list;

static int gpt_fd;

// static int read_exact(int fd, void *buf, size_t size)
// {
// 	uint8_t *ptr = buf;
// 	size_t total = 0U;
// 	ssize_t bytes;

// 	while (total < size) {
// 		bytes = read(fd, ptr + total, size - total);
// 		if (bytes <= 0) {
// 			if (bytes == 0) {
// 				printf("Unexpected EOF while reading %zu bytes\n", size);
// 				return -EINVAL;
// 			}
// 			printf("Failed to read data (%zd)\n", bytes);
// 			return -errno;
// 		}
// 		total += (size_t)bytes;
// 	}

// 	return 0;
// }

#if DEBUG 
static void dump_entries(partition_entry_list_t *plist)
{
	char name[EFI_NAMELEN];
	int i, j, len;
	int num = plist->entry_count;

	printf("Partition table with %d entries:\n", num);
	for (i = 0; i < num; i++) {
		len = snprintf(name, EFI_NAMELEN, "%s", plist->list[i].name);
		for (j = 0; j < EFI_NAMELEN - len - 1; j++) {
			name[len + j] = ' ';
		}
		name[EFI_NAMELEN - 1] = '\0';
		printf("%d: %s %" PRIx64 "-%" PRIx64 "\n", i + 1, name, plist->list[i].start,
			plist->list[i].start + plist->list[i].length - 4);
	}
}
#endif

/*
 * Load the first sector that carries MBR header.
 * The MBR boot signature should be always valid whether it's MBR or GPT.
 */
static int load_mbr_header(int fd, mbr_entry_t *mbr_entry)
{
	int result;
	mbr_entry_t tmp;

	assert(mbr_entry != NULL);
	/* MBR partition table is in LBA0. */
	result = lseek(fd, MBR_OFFSET, SEEK_SET);
	if (result != 0) {
		printf("Failed to seek (%i)\n", result);
		return result;
	}
	result = read(fd, (void*)&mbr_sector, PLAT_PARTITION_BLOCK_SIZE);
	if ((result == -1)) {
		printf("Failed to read data (%i)\n", result);
		return result;
	}

	/* Check MBR boot signature. */
	if ((mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 2] != MBR_SIGNATURE_FIRST) ||
	    (mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 1] != MBR_SIGNATURE_SECOND)) {
		printf("MBR boot signature failure\n");
		return -ENOENT;
	}

	memcpy(&tmp, mbr_sector + MBR_PRIMARY_ENTRY_OFFSET, sizeof(tmp));

	if (tmp.first_lba != 1) {
		printf("MBR header may have an invalid first LBA\n");
		return -EINVAL;
	}

#if 0
	if ((tmp.sector_nums == 0) || (tmp.sector_nums == UINT32_MAX)) {
		printf("MBR header entry has an invalid number of sectors\n");
		return -EINVAL;
	}
#endif

	memcpy(mbr_entry, &tmp, sizeof(mbr_entry_t));

	return 0;
}

/*
 * Load GPT header and check the GPT signature and header CRC.
 * If partition numbers could be found, check & update it.
 */
static int load_gpt_header(int fd, size_t header_offset,
			   gpt_header_t *header, partition_entry_list_t *plist)
{
	int result;
	uint32_t header_crc, calc_crc;

	result = lseek(fd, header_offset, SEEK_SET);
	if (result == -1) {
		printf("Failed to seek into the GPT image at offset (%zu)\n",
			header_offset);
		return result;
	}
	result = read(fd, (void *)header, sizeof(gpt_header_t));
	if (result == -1) {
		printf("GPT header read error(%i) \n", result);
		return result;
	}
	if (memcmp(header->signature, GPT_SIGNATURE,
			   sizeof(header->signature)) != 0) {
		printf("GPT header signature failure\n");
		return -EINVAL;
	}

	/*
	 * UEFI Spec 2.8 March 2019 Page 119: HeaderCRC32 value is
	 * computed by setting this field to 0, and computing the
	 * 32-bit CRC for HeaderSize bytes.
	 */
	header_crc = header->header_crc;
	header->header_crc = 0U;

	calc_crc = tf_crc32(0U, (uint8_t *)header, sizeof(gpt_header_t));
	if (header_crc != calc_crc) {
		printf("Invalid GPT Header CRC: Expected 0x%x but got 0x%x.\n",
		      header_crc, calc_crc);
		return -EINVAL;
	}

	header->header_crc = header_crc;

	/* partition numbers can't exceed PLAT_PARTITION_MAX_ENTRIES */
	plist->entry_count = header->list_num;
	if (plist->entry_count > PLAT_PARTITION_MAX_ENTRIES) {
		plist->entry_count = PLAT_PARTITION_MAX_ENTRIES;
	}

	return 0;
}

/*
 * Load a single MBR entry based on details from MBR header.
 */
static int load_mbr_entry(int fd, mbr_entry_t *mbr_entry,
			  int part_number)
{
	uintptr_t offset;
	int result;

	assert(mbr_entry != NULL);
	/* MBR partition table is in LBA0. */
	result = lseek(fd, MBR_OFFSET, SEEK_SET);
	if (result != 0) {
		printf("Failed to seek (%i)\n", result);
		return result;
	}
	result = read(fd, (void*)&mbr_sector,
			 PLAT_PARTITION_BLOCK_SIZE);
	if (result != 0) {
		printf("Failed to read data (%i)\n", result);
		return result;
	}

	/* Check MBR boot signature. */
	if ((mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 2] != MBR_SIGNATURE_FIRST) ||
	    (mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 1] != MBR_SIGNATURE_SECOND)) {
		printf("MBR Entry boot signature failure\n");
		return -ENOENT;
	}
	offset = (uintptr_t)&mbr_sector +
		MBR_PRIMARY_ENTRY_OFFSET +
		MBR_PRIMARY_ENTRY_SIZE * part_number;
	memcpy(mbr_entry, (void *)offset, sizeof(mbr_entry_t));

	return 0;
}

/*
 * Load MBR entries based on max number of partition entries.
 */
static int load_mbr_entries(int fd, partition_entry_list_t *plist)
{
	mbr_entry_t mbr_entry;
	unsigned int i;

	plist->entry_count = MBR_PRIMARY_ENTRY_NUMBER;

	for (i = 0U; i < plist->entry_count; i++) {
		load_mbr_entry(fd, &mbr_entry, i);
		plist->list[i].start = mbr_entry.first_lba * 512;
		plist->list[i].length = mbr_entry.sector_nums * 512;
		plist->list[i].name[0] = mbr_entry.type;
	}

	return 0;
}

/*
 * Try to read and load a single GPT entry.
 */
static int load_gpt_entry(uintptr_t fd, gpt_entry_t *entry)
{
	int result;

	assert(entry != NULL);
	result = read(fd, (void *)entry, sizeof(gpt_entry_t));
	if (result == -1)  {
		printf("GPT Entry read error(%i) or read mismatch occurred,"
			"expected(%zu) \n", result,
			sizeof(gpt_entry_t));
		return -EINVAL;
	}

	return result;
}

/*
 * Retrieve each entry in the partition table, parse the data from each
 * entry and store them in the list of partition table entries.
 */
static int load_partition_gpt(uintptr_t fd, gpt_header_t header, 
		partition_entry_list_t *plist)
{
	const signed long long gpt_entry_offset = LBA(header.part_lba);
	gpt_entry_t entry;
	int result;
	unsigned int i;
	uint32_t calc_crc = 0U;

	result = lseek(fd, gpt_entry_offset, SEEK_SET);
	if (result == -1) {
		printf("Failed to seek (%i), Failed loading GPT partition"
			"table entries\n", result);
		return result;
	}

	for (i = 0U; i < plist->entry_count; i++) {
		result = load_gpt_entry(fd, &entry);
		if (result == -1) {
			printf("Failed to load gpt entry data(%u) error is (%i)\n",
				i, result);
			return result;
		}

		result = parse_gpt_entry(&entry, &plist->list[i]);
		if (result != 0) {
			result = lseek(fd, (gpt_entry_offset + (i * sizeof(gpt_entry_t))), SEEK_SET);
			if (result == -1) {
				printf("Failed to seek (%i)\n", result);
				return result;
			}
			break;
		}

		/*
		 * Calculate CRC of Partition entry array to compare with CRC
		 * value in header
		 */
		calc_crc = tf_crc32(calc_crc, (uint8_t *)&entry, sizeof(gpt_entry_t));
	}
	if (i == 0) {
		printf("No Valid GPT Entries found\n");
		return -EINVAL;
	}

	/*
	 * Only records the valid partition number that is loaded from
	 * partition table.
	 */
	plist->entry_count = i;
#if DEBUG
	dump_entries(plist);
#endif

	/*
	 * If there are less valid entries than the possible number of entries
	 * from the header, continue to load the partition entry table to
	 * calculate the full CRC in order to check against the partition CRC
	 * from the header for validation.
	 */
	for (; i < header.list_num; i++) {
		result = load_gpt_entry(fd, &entry);
		if (result == -1) {
			printf("Failed to load gpt entry data(%u) error is (%i)\n",
				i, result);
			return result;
		}

		calc_crc = tf_crc32(calc_crc, (uint8_t *)&entry, sizeof(gpt_entry_t));
	}

	if (header.part_crc != calc_crc) {
		printf("Invalid GPT Partition Array Entry CRC: Expected 0x%x"
				" but got 0x%x.\n", header.part_crc, calc_crc);
		return -EINVAL;
	}

	return 0;
}

/*
 * Load a GPT partition, Try retrieving and parsing the primary GPT header,
 * if its corrupted try loading backup GPT header and then retrieve list
 * of partition table entries found from the GPT.
 */
static int load_primary_gpt(int fd, unsigned int first_lba, 
		                            partition_entry_list_t *plist)
{
	int result;
	size_t gpt_header_offset;
	gpt_header_t header;

	/* Try to load Primary GPT header from LBA1 */
	gpt_header_offset = LBA(first_lba);
	result = load_gpt_header(fd, gpt_header_offset, &header, plist);
	if ((result != 0) || (header.part_lba == 0)) {
		printf("Failed to retrieve Primary GPT header,"
			"trying to retrieve back-up GPT header\n");
		return result;
	}

	return load_partition_gpt(fd, header, plist);
}

/*
 * Try retrieving a partition table entry based on the name of the partition.
 */
const partition_entry_t *get_partition_entry(const char *name, int type)
{
	unsigned int i;
    partition_entry_list_t *plist;

	if (type == TYPE_FLASH) {
        plist = &flash_parts_list;
    } else if (type == TYPE_MMC) {
        plist = &mmc_parts_list;
	}

	for (i = 0U; i < plist->entry_count; i++) {
		if (strcmp(name, plist->list[i].name) == 0) {
			return &plist->list[i];
		}
	}
	return NULL;
}

int32_t get_partition_size_by_name(const char *name, int type, uint32_t *size)
{
    partition_entry_list_t *plist;
	uint32_t i;

	if (type == TYPE_FLASH) {
        plist = &flash_parts_list;
    } else if (type == TYPE_MMC) {
        plist = &mmc_parts_list;
	}

	for (i = 0U; i < plist->entry_count; i++) {
		if (strcmp(name, plist->list[i].name) == 0) {
			*size = plist->list[i].length;
			return 0;
		}
	}

	*size = 0;

	return -1;
}

/*
 * Load the partition table info based on the image id provided.
 */
int gpt_partition_init(char *dev)
{
    partition_entry_list_t *plist;
	mbr_entry_t mbr_entry;
    int result;

	if (strstr(dev, "mtd") != NULL) {
        plist = &flash_parts_list;
    } else if (strstr(dev, "mmcblk") != NULL) {
        plist = &mmc_parts_list;
	}

	gpt_fd = open(dev, O_RDONLY);
	if (gpt_fd == -1) {
		printf("Open file '%s' failed (%d)\n", dev, errno);
		return -errno;
	}

	result = load_mbr_header(gpt_fd, &mbr_entry);
	if (result != 0) {
		printf("Failed to access (%i)\n", result);
		goto out;
	}
	if (mbr_entry.type == PARTITION_TYPE_GPT) {
		result = load_primary_gpt(gpt_fd, mbr_entry.first_lba, plist);
		if (result != 0) {
			close(gpt_fd);
			return -1;
			/*
			return load_backup_gpt(BKUP_GPT_IMAGE_ID,
					       mbr_entry.sector_nums);
						   */
		}
	} else {
		result = load_mbr_entries(gpt_fd, plist);
	}

out:
	close(gpt_fd);
	return result;
}

/*
 * Load the partition table info based on the image id provided.
 */
// int gpt_partition_init(char *dev) {
// 	int fd = -1;
// 	int result;
// 	size_t entry_array_size;
// 	size_t read_size;
// 	uint8_t *gpt_buf = NULL;
// 	uint32_t header_crc;
// 	uint32_t calc_crc = 0U;
// 	uint32_t entries_to_parse;
// 	unsigned int i;
// 	partition_entry_list_t *plist = NULL;
// 	mbr_entry_t *mbr_entry;
// 	gpt_header_t header;
// 	gpt_entry_t *entry;

// 	if (strstr(dev, "mtd") != NULL) {
// 		plist = &flash_parts_list;
// 	} else if (strstr(dev, "mmcblk") != NULL) {
// 		plist = &mmc_parts_list;
// 	} else {
// 		printf("Unsupported device for GPT init: %s\n", dev);
// 		return -1;
// 	}

// 	fd = open(dev, O_RDONLY);
// 	if (fd < 0) {
// 		printf("Open file failed\n");
// 		return -1;
// 	}

// 	if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
// 		printf("Failed to seek GPT device\n");
// 		goto err;
// 	}

// 	result = read_exact(fd, mbr_sector, PLAT_PARTITION_BLOCK_SIZE);
// 	if (result != 0) {
// 		goto err;
// 	}

// 	if ((mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 2] != MBR_SIGNATURE_FIRST) ||
// 	    (mbr_sector[LEGACY_PARTITION_BLOCK_SIZE - 1] != MBR_SIGNATURE_SECOND)) {
// 		printf("Invalid MBR signature\n");
// 		goto err;
// 	}

// 	mbr_entry = (mbr_entry_t *)(mbr_sector + MBR_PRIMARY_ENTRY_OFFSET);
// 	if (mbr_entry->type != PARTITION_TYPE_GPT || mbr_entry->first_lba != 1U) {
// 		printf("Device does not contain a valid GPT protective MBR\n");
// 		goto err;
// 	}

// 	result = read_exact(fd, &header, sizeof(header));
// 	if (result != 0) {
// 		goto err;
// 	}

// 	if (memcmp(header.signature, GPT_SIGNATURE, sizeof(header.signature)) != 0) {
// 		printf("Invalid GPT signature\n");
// 		goto err;
// 	}

// 	if (header.size < sizeof(gpt_header_t) || header.part_size != sizeof(gpt_entry_t)) {
// 		printf("Unsupported GPT geometry: header size %u, entry size %u\n",
// 		       header.size, header.part_size);
// 		goto err;
// 	}

// 	header_crc = header.header_crc;
// 	header.header_crc = 0U;
// 	calc_crc = tf_crc32(0U, (uint8_t *)&header, sizeof(header));
// 	header.header_crc = header_crc;
// 	if (header_crc != calc_crc) {
// 		printf("Invalid GPT Header CRC: Expected 0x%x but got 0x%x.\n",
// 		       header_crc, calc_crc);
// 		goto err;
// 	}

// 	entry_array_size = (size_t)header.list_num * header.part_size;
// 	read_size = (size_t)LBA(header.part_lba) + entry_array_size;
// 	gpt_buf = malloc(read_size);
// 	if (gpt_buf == NULL) {
// 		printf("Failed to allocate %zu bytes for GPT buffer\n", read_size);
// 		goto err;
// 	}

// 	if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
// 		printf("Failed to seek GPT device\n");
// 		goto err;
// 	}

// 	result = read_exact(fd, gpt_buf, read_size);
// 	if (result != 0) {
// 		goto err;
// 	}

// 	calc_crc = tf_crc32(0U, gpt_buf + LBA(header.part_lba), entry_array_size);
// 	if (header.part_crc != calc_crc) {
// 		printf("Invalid GPT Partition Array CRC: Expected 0x%x but got 0x%x.\n",
// 		       header.part_crc, calc_crc);
// 		goto err;
// 	}

// 	plist->entry_count = 0U;
// 	entries_to_parse = header.list_num;
// 	if (entries_to_parse > PLAT_PARTITION_MAX_ENTRIES) {
// 		entries_to_parse = PLAT_PARTITION_MAX_ENTRIES;
// 	}

// 	for (i = 0U; i < entries_to_parse; i++) {
// 		entry = (gpt_entry_t *)(gpt_buf + LBA(header.part_lba) +
// 				       ((size_t)i * header.part_size));
// 		if (guidcmp(&entry->type_uuid, &(struct efi_guid)NULL_GUID) == 0) {
// 			continue;
// 		}

// 		result = parse_gpt_entry(entry, &plist->list[plist->entry_count]);
// 		if (result == 0) {
// 			plist->entry_count++;
// 		}
// 	}

// 	printf("GPT Partition Table initialized, found %u partitions\n",
// 	       plist->entry_count);

// 	free(gpt_buf);
// 	close(fd);
// 	return 0;

// err:
// 	free(gpt_buf);
// 	close(fd);
// 	return -1;
// }
