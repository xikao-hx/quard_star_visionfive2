#ifndef __QUARD_NOR_AGENT_PROTOCOL_H__
#define __QUARD_NOR_AGENT_PROTOCOL_H__

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
typedef u32 quard_u32;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t quard_u32;
#endif

/* Linux/FreeRTOS mailbox ABI for the VisionFive 2 SPI NOR service. */
#define QUARD_NOR_ABI_VERSION       1U
#define QUARD_NOR_EXPECTED_PARTS    6U
#define QUARD_NOR_SHRAM_BASE        0x6E500000U
#define QUARD_NOR_SHRAM_SIZE        0x00200000U

enum nor_operation_type {
	NOR_GET_INFO = 0,
	NOR_WRITE,
	NOR_READ,
	NOR_ERASE,
	NOR_SEC_WRITE,
	NOR_SEC_READ,
};

enum nor_operation_status {
	NOR_OP_OK = 0,
	NOR_OP_FAILED,
};

struct quard_nor_common_param {
	quard_u32	op_offset;
	quard_u32	op_len;
	quard_u32	status; /* Response only. */
	quard_u32	shram_phy_addr;
	quard_u32	check_sum;
	quard_u32	reserved;
};

/* NOR geometry and the validated GPT partition view. */
#define QUARD_NOR_MAX_PARTS	64
#define QUARD_NOR_PARTNAME_MAX	32
typedef struct quard_nor_part_info {
	char	name[QUARD_NOR_PARTNAME_MAX];
	quard_u32 offset;
	quard_u32 length;
} nor_part_t;

struct quard_nor_info {
	quard_u32	abi_version;
	quard_u32	capacity;
	quard_u32	id;
	quard_u32	otp_size;
	quard_u32	sector_size;
	quard_u32	page_size;
	quard_u32	erase_size;
	quard_u32	nparts;
	nor_part_t	parts[QUARD_NOR_MAX_PARTS];
} __attribute__((aligned(32)));

_Static_assert(sizeof(struct quard_nor_common_param) == 24,
	       "quard_nor_common_param ABI changed");
_Static_assert(sizeof(nor_part_t) == 40, "nor_part_t ABI changed");
_Static_assert(offsetof(struct quard_nor_info, parts) == 32,
	       "quard_nor_info header ABI changed");
_Static_assert(sizeof(struct quard_nor_info) == 2592,
	       "quard_nor_info ABI changed");

#endif
