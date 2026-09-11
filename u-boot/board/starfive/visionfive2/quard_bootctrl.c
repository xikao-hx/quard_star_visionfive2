// SPDX-License-Identifier: GPL-2.0+
/*
 * VisionFive 2 U-Boot proper SD A/B selection.
 *
 * SPL owns recovery updates.  This helper only reads current_bank through the
 * mailbox-backed MTD device and derives the SD boot variables for this boot.
 */

#include <common.h>
#include <env.h>
#include <linux/err.h>
#include <linux/mtd/mtd.h>
#include <mtd.h>
#include <u-boot/crc.h>

#include <quard_recovery_config.h>

#include "quard_bootctrl.h"

#define QUARD_RECOVERY_MTD_NAME	"recovery"
#define QUARD_BOOTCTRL_ENV_VERSION	"1"

struct quard_sd_slot {
	const char *name;
	const char *boot_part;
	const char *root_part;
	const char *active_system;
};

static const struct quard_sd_slot quard_sd_slots[] = {
	[RECOVERY_BANK_A] = {
		.name = "a",
		.boot_part = "3",
		.root_part = "4",
		.active_system = "0",
	},
	[RECOVERY_BANK_B] = {
		.name = "b",
		.boot_part = "5",
		.root_part = "6",
		.active_system = "1",
	},
};

static int quard_bootctrl_refresh_env(void)
{
	static char * const managed_vars[] = {
		"sd_ab_env_version",
		"default_bootpart",
		"default_rootpart",
		"sd_ab_enabled",
		"sd_ab_failed",
		"use_default_parts",
		"use_sd_ab_parts",
		"sd_ab_fail",
		"load_sdk_uenv",
		"legacy_mmc_boot",
		"mmc_test_and_boot",
		"bootenv_mmc",
		"bootenv_nvme",
		"sdk_boot_env",
	};
	const char *version = env_get("sd_ab_env_version");

	if (version && !strcmp(version, QUARD_BOOTCTRL_ENV_VERSION))
		return 0;

	if (!env_set_default_vars(ARRAY_SIZE(managed_vars), managed_vars, 0))
		return -EIO;

	puts("SD bootctrl: refreshed A/B scripts from the default environment\n");
	return 0;
}

static bool quard_recovery_valid(const recovery_config_t *cfg)
{
	if (cfg->magic != RECOVERY_MAGIC ||
	    cfg->struct_version != RECOVERY_STRUCT_VERSION ||
	    cfg->current_bank > RECOVERY_BANK_B ||
	    cfg->target_bank > RECOVERY_BANK_B)
		return false;

	return crc32(0, (const unsigned char *)cfg,
		     RECOVERY_HEADER_CRC32_OFFSET) == cfg->header_crc32;
}

static int quard_recovery_read(recovery_config_t *cfg)
{
	struct mtd_info *mtd;
	size_t retlen = 0;
	int ret;

	ret = mtd_probe_devices();
	if (ret)
		return ret;

	mtd = get_mtd_device_nm(QUARD_RECOVERY_MTD_NAME);
	if (IS_ERR_OR_NULL(mtd))
		return IS_ERR(mtd) ? PTR_ERR(mtd) : -ENODEV;

	if (mtd->size < sizeof(*cfg)) {
		ret = -EOVERFLOW;
		goto out_put;
	}

	ret = mtd_read(mtd, 0, sizeof(*cfg), &retlen, (u8 *)cfg);
	if (!ret && retlen != sizeof(*cfg))
		ret = -EIO;

out_put:
	put_mtd_device(mtd);
	return ret;
}

static int quard_sd_slot_export(u32 bank)
{
	const struct quard_sd_slot *slot;
	int ret;

	if (bank >= ARRAY_SIZE(quard_sd_slots))
		return -EINVAL;

	slot = &quard_sd_slots[bank];
	ret = env_set("active_bank", slot->name);
	if (ret)
		return ret;
	ret = env_set("active_system", slot->active_system);
	if (ret)
		return ret;
	ret = env_set("sd_bootpart", slot->boot_part);
	if (ret)
		return ret;
	ret = env_set("sd_rootpart", slot->root_part);
	if (ret)
		return ret;
	ret = env_set("sd_ab_failed", "0");
	if (ret)
		return ret;

	/* Publish validity last so scripts cannot consume a partial mapping. */
	return env_set("sd_ab_enabled", "1");
}

int quard_bootctrl_apply(void)
{
	recovery_config_t cfg;
	u32 bank = RECOVERY_BANK_A;
	int ret;

	ret = quard_bootctrl_refresh_env();
	if (ret) {
		printf("SD bootctrl: failed to refresh A/B scripts: %d\n", ret);
		return ret;
	}

	/* Start other boot media from their legacy 3/4 mapping. */
	ret = env_set("bootpart", quard_sd_slots[RECOVERY_BANK_A].boot_part);
	if (ret)
		return ret;
	ret = env_set("rootpart", quard_sd_slots[RECOVERY_BANK_A].root_part);
	if (ret)
		return ret;
	ret = env_set("sd_ab_boot", "0");
	if (ret)
		return ret;
	ret = env_set("sd_ab_enabled", "0");
	if (ret)
		return ret;

	ret = quard_recovery_read(&cfg);
	if (ret) {
		printf("SD bootctrl: recovery transport failed: %d\n", ret);
		return ret;
	}

	if (!quard_recovery_valid(&cfg)) {
		puts("SD bootctrl: recovery metadata invalid, fallback to slot a\n");
	} else {
		bank = cfg.current_bank;
		printf("SD bootctrl: current=%s target=%s\n",
		       quard_sd_slots[cfg.current_bank].name,
		       quard_sd_slots[cfg.target_bank].name);
	}

	ret = quard_sd_slot_export(bank);
	if (ret) {
		printf("SD bootctrl: failed to export slot variables: %d\n", ret);
		return ret;
	}

	printf("SD bootctrl: slot %s uses SD boot %s, rootfs %s\n",
	       quard_sd_slots[bank].name, quard_sd_slots[bank].boot_part,
	       quard_sd_slots[bank].root_part);
	return 0;
}
