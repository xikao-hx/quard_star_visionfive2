#ifndef _HUNTER_BOOT
#define _HUNTER_BOOT

#ifndef __ASSEMBLER__
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 quard_log_u32;
#else
#include <stdint.h>
typedef uint32_t quard_log_u32;
#endif
#endif

/* mmap */
#define BUF_INIT_VALUE			(0)
#define MCU_LOG_BUF_FLAG		(1)
#define BL1_SBI_LOG_BUF_FLAG    (2)
#define UBOOT_LOG_BUF_FLAG		(3)
#define BUF_MAX_VALUE		    (4)

#define LOG_BUF_RESERVED_BASE_ADDR    (0x6e404000)
#define LOG_BUF_RESERVED_SIZE         (0x6000)

#define MCU_LOG_BUF_BASE_ADDR         (0x6e404000)
#define UBOOT_LOG_BUF_BASE_ADDR       (0x6e406000)
#define BL1_SBI_LOG_BUF_BASE_ADDR     (0x6e408000)

#define MCU_LOG_BUF_SIZE	        0x2000
#define BL1_SBI_LOG_BUF_SIZE        0x2000
#define UBOOT_LOG_BUF_SIZE		    0x2000

#define RAMLOG_MAGIC                   (0x52414d4cU)

#define OFFSET                         (sizeof(struct ramlog_buffer))
#define SPL_LOG_BUF_OFFSET             (0x000)
#define SPL_LOG_BUF_SIZE               (0x400)
#define SPL_RAMLOG_SIZE                (SPL_LOG_BUF_SIZE - OFFSET)

/* Compatibility names for the historical BL1 terminology. */
#define BL1_LOG_BUF_OFFSET             SPL_LOG_BUF_OFFSET
#define BL1_LOG_BUF_SIZE               SPL_LOG_BUF_SIZE
#define BL1_RAMLOG_SIZE                SPL_RAMLOG_SIZE

#define SBI_LOG_BUF_OFFSET	     	(0x400)
#define SBI_LOG_BUF_SIZE		    (0x1000)
#define SBI_RAMLOG_SIZE				(SBI_LOG_BUF_SIZE - OFFSET)
#define UBOOT_RAMLOG_SIZE		    (UBOOT_LOG_BUF_SIZE - OFFSET)

#define MCU_LOG_FULL_MSG	"mcu buffer is captured"

#ifndef __ASSEMBLER__
struct ramlog_buffer {
    quard_log_u32 magic;
    quard_log_u32 head;
    quard_log_u32 tail;
    quard_log_u32 size;
};

typedef char quard_ramlog_header_must_be_16_bytes[
    sizeof(struct ramlog_buffer) == 16 ? 1 : -1];
#endif

#endif
