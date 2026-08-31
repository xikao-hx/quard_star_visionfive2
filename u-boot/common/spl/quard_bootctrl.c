// SPDX-License-Identifier: GPL-2.0+
/*
 * Quard basic A/B payload selection for VisionFive 2 SPL.
 *
 * Only target_bank/current_bank are consumed from recovery metadata. All
 * other fields are preserved without affecting payload selection.
 */

#include <common.h>
#include <errno.h>
#include <part_efi.h>
#include <spi_flash.h>
#include <u-boot/crc.h>
#include <asm/unaligned.h>

#include <quard_nor_layout.h>
#include <quard_recovery_config.h>

#include "quard_bootctrl.h"

#define QUARD_GPT_HEADER_LBA		1U
#define QUARD_GPT_ENTRY_LBA		2U
#define QUARD_GPT_HEADER_SIZE		92U
#define QUARD_GPT_ENTRY_COUNT		128U
#define QUARD_GPT_ENTRY_SIZE		128U
#define QUARD_GPT_REVISION		0x00010000U
#define QUARD_GPT_SIGNATURE		0x5452415020494645ULL
#define QUARD_GPT_PROTECTIVE_TYPE	0xEEU

struct quard_partition {
	const char *name;
	u32 offset;
	u32 size;
};

struct quard_partition_view {
	u32 recovery_offset;
	u32 payload_offset[2];
};

static const struct quard_partition quard_expected_partitions[] = {
	{ QUARD_NOR_SPL_A_NAME, QUARD_NOR_SPL_A_OFFSET, QUARD_NOR_SPL_A_SIZE },
	{ QUARD_NOR_SPL_B_NAME, QUARD_NOR_SPL_B_OFFSET, QUARD_NOR_SPL_B_SIZE },
	{ QUARD_NOR_GPT_NAME, QUARD_NOR_GPT_OFFSET, QUARD_NOR_GPT_SIZE },
	{ QUARD_NOR_RECOVERY_NAME, QUARD_NOR_RECOVERY_OFFSET,
	  QUARD_NOR_RECOVERY_SIZE },
	{ QUARD_NOR_FW_PAYLOAD_A_NAME, QUARD_NOR_FW_PAYLOAD_A_OFFSET,
	  QUARD_NOR_FW_PAYLOAD_A_SIZE },
	{ QUARD_NOR_FW_PAYLOAD_B_NAME, QUARD_NOR_FW_PAYLOAD_B_OFFSET,
	  QUARD_NOR_FW_PAYLOAD_B_SIZE },
};

static const u8 quard_basic_data_guid[sizeof(efi_guid_t)] = {
	0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

static u8 recovery_sector[QUARD_NOR_ERASE_SIZE] __aligned(8);
static u8 recovery_verify_sector[QUARD_NOR_ERASE_SIZE] __aligned(8);

static const char *recovery_bank_name(u32 bank)
{
	return bank == RECOVERY_BANK_B ? "b" : "a";
}

static int recovery_valid(const recovery_config_t *cfg)
{
	if (!cfg || cfg->magic != RECOVERY_MAGIC ||
	    cfg->struct_version != RECOVERY_STRUCT_VERSION ||
	    cfg->current_bank > RECOVERY_BANK_B ||
	    cfg->target_bank > RECOVERY_BANK_B)
		return 0;

	return crc32(0, (const unsigned char *)cfg,
		     RECOVERY_HEADER_CRC32_OFFSET) == cfg->header_crc32;
}

static int recovery_write_config(struct spi_flash *flash, u32 recovery_offset,
				 recovery_config_t *cfg)
{
	int ret;

	if (!flash || !cfg ||
	    recovery_offset > QUARD_NOR_FLASH_SIZE - QUARD_NOR_ERASE_SIZE)
		return -EINVAL;

	cfg->header_crc32 = crc32(0, (const unsigned char *)cfg,
				  RECOVERY_HEADER_CRC32_OFFSET);

	ret = spi_flash_erase(flash, recovery_offset, QUARD_NOR_ERASE_SIZE);
	if (ret)
		return ret;

	ret = spi_flash_write(flash, recovery_offset, QUARD_NOR_ERASE_SIZE,
			      recovery_sector);
	if (ret)
		return ret;

	ret = spi_flash_read(flash, recovery_offset, QUARD_NOR_ERASE_SIZE,
			     recovery_verify_sector);
	if (ret)
		return ret;

	return memcmp(recovery_sector, recovery_verify_sector,
		      QUARD_NOR_ERASE_SIZE) ? -EIO : 0;
}

static int recovery_sync_current_bank(struct spi_flash *flash,
				      u32 recovery_offset,
				      recovery_config_t *cfg,
				      u32 selected_bank)
{
	if (!cfg || selected_bank > RECOVERY_BANK_B)
		return -EINVAL;

	if (cfg->current_bank == selected_bank)
		return 0;

	cfg->current_bank = selected_bank;
	return recovery_write_config(flash, recovery_offset, cfg);
}

static int gpt_name_matches(const gpt_entry *entry, const char *expected)
{
	size_t index;
	size_t length = strlen(expected);

	if (length >= PARTNAME_SZ)
		return 0;

	for (index = 0; index < PARTNAME_SZ; index++) {
		u16 value = le16_to_cpu(entry->partition_name[index]);

		if (index < length) {
			if (value != (u8)expected[index])
				return 0;
		} else if (value != 0U) {
			return 0;
		}
	}

	return 1;
}

static int gpt_entry_unused(const gpt_entry *entry)
{
	const u8 *bytes = (const u8 *)entry;
	size_t index;

	for (index = 0; index < sizeof(*entry); index++) {
		if (bytes[index] != 0U)
			return 0;
	}

	return 1;
}

static int gpt_validate(struct spi_flash *flash,
			struct quard_partition_view *view)
{
	u8 mbr[QUARD_NOR_SECTOR_SIZE];
	gpt_header header;
	gpt_entry entry;
	u32 stored_header_crc;
	u32 entries_crc = 0;
	u32 index;
	u32 flash_lbas = QUARD_NOR_FLASH_SIZE / QUARD_NOR_SECTOR_SIZE;
	int ret;

	if (!flash || !view || flash->mtd.size < QUARD_NOR_FLASH_SIZE)
		return -EINVAL;
	memset(view, 0, sizeof(*view));

	ret = spi_flash_read(flash, QUARD_NOR_GPT_OFFSET, sizeof(mbr), mbr);
	if (ret)
		return ret;

	if (mbr[510] != 0x55 || mbr[511] != 0xaa ||
	    mbr[446 + 4] != QUARD_GPT_PROTECTIVE_TYPE ||
	    get_unaligned_le32(mbr + 446 + 8) != 1U ||
	    get_unaligned_le32(mbr + 446 + 12) != flash_lbas - 1U)
		return -EINVAL;

	ret = spi_flash_read(flash,
			     QUARD_NOR_GPT_OFFSET + QUARD_GPT_HEADER_LBA *
			     QUARD_NOR_SECTOR_SIZE,
			     sizeof(header), &header);
	if (ret)
		return ret;

	if (le64_to_cpu(header.signature) != QUARD_GPT_SIGNATURE ||
	    le32_to_cpu(header.revision) != QUARD_GPT_REVISION ||
	    le32_to_cpu(header.header_size) != QUARD_GPT_HEADER_SIZE ||
	    le32_to_cpu(header.reserved1) != 0U ||
	    le64_to_cpu(header.my_lba) != QUARD_GPT_HEADER_LBA ||
	    le64_to_cpu(header.alternate_lba) != flash_lbas - 1U ||
	    le64_to_cpu(header.first_usable_lba) != 0U ||
	    le64_to_cpu(header.last_usable_lba) != flash_lbas - 1U ||
	    le64_to_cpu(header.partition_entry_lba) != QUARD_GPT_ENTRY_LBA ||
	    le32_to_cpu(header.num_partition_entries) != QUARD_GPT_ENTRY_COUNT ||
	    le32_to_cpu(header.sizeof_partition_entry) != QUARD_GPT_ENTRY_SIZE)
		return -EINVAL;

	stored_header_crc = le32_to_cpu(header.header_crc32);
	header.header_crc32 = cpu_to_le32(0);
	if (crc32(0, (const unsigned char *)&header,
		  QUARD_GPT_HEADER_SIZE) != stored_header_crc)
		return -EINVAL;

	for (index = 0; index < QUARD_GPT_ENTRY_COUNT; index++) {
		const struct quard_partition *expected;
		u64 first_lba;
		u64 last_lba;
		u64 expected_first;
		u64 expected_last;

		ret = spi_flash_read(flash,
			QUARD_NOR_GPT_OFFSET + QUARD_GPT_ENTRY_LBA *
			QUARD_NOR_SECTOR_SIZE + index * sizeof(entry),
			sizeof(entry), &entry);
		if (ret)
			return ret;

		entries_crc = crc32(entries_crc, (const unsigned char *)&entry,
				    sizeof(entry));
		if (index >= ARRAY_SIZE(quard_expected_partitions)) {
			if (!gpt_entry_unused(&entry))
				return -EINVAL;
			continue;
		}

		expected = &quard_expected_partitions[index];
		first_lba = le64_to_cpu(entry.starting_lba);
		last_lba = le64_to_cpu(entry.ending_lba);
		expected_first = expected->offset / QUARD_NOR_SECTOR_SIZE;
		expected_last = (expected->offset + expected->size) /
				QUARD_NOR_SECTOR_SIZE - 1U;

		if (!gpt_name_matches(&entry, expected->name) ||
		    memcmp(&entry.partition_type_guid, quard_basic_data_guid,
			   sizeof(quard_basic_data_guid)) ||
		    first_lba != expected_first || last_lba != expected_last ||
		    entry.attributes.raw != 0ULL)
			return -EINVAL;

		switch (index) {
		case 3:
			view->recovery_offset = first_lba * QUARD_NOR_SECTOR_SIZE;
			break;
		case 4:
			view->payload_offset[RECOVERY_BANK_A] =
				first_lba * QUARD_NOR_SECTOR_SIZE;
			break;
		case 5:
			view->payload_offset[RECOVERY_BANK_B] =
				first_lba * QUARD_NOR_SECTOR_SIZE;
			break;
		default:
			break;
		}
	}

	if (entries_crc != le32_to_cpu(header.partition_entry_array_crc32))
		return -EINVAL;

	return 0;
}

static u32 quard_bootctrl_get_active_bank(struct spi_flash *flash,
					  u32 recovery_offset)
{
	recovery_config_t *cfg = (recovery_config_t *)recovery_sector;
	u32 selected_bank;
	int ret;

	ret = spi_flash_read(flash, recovery_offset, QUARD_NOR_ERASE_SIZE,
			     recovery_sector);
	if (ret || !recovery_valid(cfg)) {
		puts("SPL bootctrl: recovery metadata is invalid, fallback to slot a\n");
		return RECOVERY_BANK_A;
	}

	selected_bank = cfg->target_bank;
	printf("SPL bootctrl: target=%s current=%s\n",
	       recovery_bank_name(selected_bank),
	       recovery_bank_name(cfg->current_bank));

	ret = recovery_sync_current_bank(flash, recovery_offset, cfg,
					 selected_bank);
	if (ret)
		printf("SPL bootctrl: warning: current bank write failed: %d\n", ret);

	printf("SPL bootctrl: use slot %s\n",
	       recovery_bank_name(selected_bank));
	return selected_bank;
}

int quard_bootctrl_select_payload(struct spi_flash *flash,
				  unsigned int *payload_offset,
				  unsigned int *selected_bank)
{
	struct quard_partition_view view;
	u32 bank = RECOVERY_BANK_A;
	int ret;

	if (!flash || !payload_offset)
		return -EINVAL;

	ret = gpt_validate(flash, &view);
	if (ret) {
		printf("SPL bootctrl: GPT invalid (%d), fallback to slot a @ 0x%x\n",
		       ret, QUARD_NOR_FW_PAYLOAD_A_OFFSET);
		*payload_offset = QUARD_NOR_FW_PAYLOAD_A_OFFSET;
	} else {
		bank = quard_bootctrl_get_active_bank(flash,
						view.recovery_offset);
		*payload_offset = view.payload_offset[bank];
		printf("SPL bootctrl: selected payload slot %s @ 0x%x\n",
		       recovery_bank_name(bank), *payload_offset);
	}

	if (selected_bank)
		*selected_bank = bank;

	return 0;
}
