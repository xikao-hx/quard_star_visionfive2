// SPDX-License-Identifier: GPL-2.0+
/* Environment backend for a named MTD device. */

#include <common.h>
#include <env.h>
#include <env_internal.h>
#include <malloc.h>
#include <mtd.h>
#include <search.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <linux/err.h>

DECLARE_GLOBAL_DATA_PTR;

static int env_mtd_open(struct mtd_info **mtdp)
{
	struct mtd_info *mtd;
	u64 erase_len = roundup((u64)CONFIG_ENV_SIZE,
				CONFIG_ENV_SECT_SIZE);

	mtd_probe_devices();
	mtd = get_mtd_device_nm(CONFIG_ENV_MTD_NAME);
	if (IS_ERR_OR_NULL(mtd))
		return IS_ERR(mtd) ? PTR_ERR(mtd) : -ENODEV;
	if (!mtd->erasesize || !CONFIG_ENV_SECT_SIZE ||
	    CONFIG_ENV_SECT_SIZE < CONFIG_ENV_SIZE ||
	    CONFIG_ENV_OFFSET % mtd->erasesize ||
	    CONFIG_ENV_SECT_SIZE % mtd->erasesize ||
	    CONFIG_ENV_OFFSET >= mtd->size ||
	    erase_len > mtd->size - CONFIG_ENV_OFFSET) {
		put_mtd_device(mtd);
		return -EINVAL;
	}
	*mtdp = mtd;
	return 0;
}

static int env_mtd_read(struct mtd_info *mtd, loff_t offset, size_t len,
			void *buffer)
{
	size_t retlen = 0;
	int ret;

	ret = mtd_read(mtd, offset, len, &retlen, buffer);
	return ret ? ret : (retlen == len ? 0 : -EIO);
}

static int env_mtd_write(struct mtd_info *mtd, loff_t offset, size_t len,
			 const void *buffer)
{
	size_t retlen = 0;
	int ret;

	ret = mtd_write(mtd, offset, len, &retlen, buffer);
	return ret ? ret : (retlen == len ? 0 : -EIO);
}

static int env_mtd_load(void)
{
	struct mtd_info *mtd;
	env_t *env;
	int ret;

	env = memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!env) {
		env_set_default("malloc() failed", 0);
		return -ENOMEM;
	}
	ret = env_mtd_open(&mtd);
	if (ret) {
		env_set_default("MTD device unavailable", 0);
		goto out_free;
	}
	ret = env_mtd_read(mtd, CONFIG_ENV_OFFSET, CONFIG_ENV_SIZE, env);
	if (ret) {
		env_set_default("MTD read failed", 0);
		goto out_put;
	}
	ret = env_import((char *)env, 1, H_EXTERNAL);
	if (!ret)
		gd->env_valid = ENV_VALID;

out_put:
	put_mtd_device(mtd);
out_free:
	free(env);
	return ret;
}

static int env_mtd_save(void)
{
	u64 erase_len = roundup((u64)CONFIG_ENV_SIZE,
				CONFIG_ENV_SECT_SIZE);
	size_t saved_len = erase_len - CONFIG_ENV_SIZE;
	loff_t saved_offset = CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE;
	struct erase_info erase = { 0 };
	struct mtd_info *mtd;
	void *saved = NULL;
	env_t *env;
	int ret;

	env = memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!env)
		return -ENOMEM;
	ret = env_export(env);
	if (ret)
		goto out_free_env;
	ret = env_mtd_open(&mtd);
	if (ret)
		goto out_free_env;
	if (saved_len) {
		saved = malloc(saved_len);
		if (!saved) {
			ret = -ENOMEM;
			goto out_put;
		}
		ret = env_mtd_read(mtd, saved_offset, saved_len, saved);
		if (ret)
			goto out_put;
	}

	erase.mtd = mtd;
	erase.addr = CONFIG_ENV_OFFSET;
	erase.len = erase_len;
	ret = mtd_erase(mtd, &erase);
	if (ret)
		goto out_put;
	ret = env_mtd_write(mtd, CONFIG_ENV_OFFSET, CONFIG_ENV_SIZE, env);
	if (ret)
		goto out_put;
	if (saved_len)
		ret = env_mtd_write(mtd, saved_offset, saved_len, saved);

out_put:
	free(saved);
	put_mtd_device(mtd);
out_free_env:
	free(env);
	return ret;
}

static int env_mtd_erase(void)
{
	u64 erase_len = roundup((u64)CONFIG_ENV_SIZE,
				CONFIG_ENV_SECT_SIZE);
	struct erase_info erase = { 0 };
	struct mtd_info *mtd;
	int ret;

	ret = env_mtd_open(&mtd);
	if (ret)
		return ret;
	erase.mtd = mtd;
	erase.addr = CONFIG_ENV_OFFSET;
	erase.len = erase_len;
	ret = mtd_erase(mtd, &erase);
	put_mtd_device(mtd);
	return ret;
}

static int env_mtd_init(void)
{
	return -ENOENT;
}

U_BOOT_ENV_LOCATION(mtd) = {
	.location = ENVL_MTD,
	ENV_NAME("MTD")
	.load = env_mtd_load,
	.save = ENV_SAVE_PTR(env_mtd_save),
	.erase = ENV_ERASE_PTR(env_mtd_erase),
	.init = env_mtd_init,
};
