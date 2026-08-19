#ifndef __MAILBOX_IPI_H__
#define __MAILBOX_IPI_H__

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define PL320_MAX_CHANS   32

static inline uint32_t sys_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void sys_write32(uint32_t value, uintptr_t addr)
{
    *(volatile uint32_t *)addr = value;
}

enum ipc_dir {
	SEND_TYPE = 0,
	RECV_TYPE,
};

struct mbox_client {
	enum ipc_dir dir;
	uint8_t idx;
	uint8_t src;
	uint8_t dst;

	void (*rx_callback)(struct mbox_client *cl, void *msg);
	void (*tx_done)(struct mbox_client *cl, void *msg, int r);
};

struct mbox_chan {
	uint32_t resp[7];
	struct mbox_client cl;
	struct mbox_chan_intr *mb;

	StaticSemaphore_t xMutex_buf;
	StaticSemaphore_t xCompletionSemaphore_buf;
	SemaphoreHandle_t xMutex;
	SemaphoreHandle_t xCompletionSemaphore;
};

#ifdef BOARD_JH7110
void jh7110_mailbox_debug(uint32_t *count, uint32_t *irq,
                          uint32_t *pending, uint32_t *pending_after,
                          uint32_t *cmd0, uint32_t *cmd1,
                          uint32_t *irq0, uint32_t *irq1,
                          uint32_t *handled, uint32_t *plic_pending,
                          uint32_t *plic_enable, uint32_t *plic_threshold);
#endif

struct mbox_chan_intr {
	uint8_t idx;
	uint8_t intr_s;
	uint8_t intr_d;
	uint32_t reg;
	int irq;
	bool master;
	volatile uint8_t *data;
	volatile uint32_t *ack;
	
	struct mbox_controller *mbox;
};

struct mbox_controller {
	uint32_t base;
	struct mbox_chan_intr mb[PL320_MAX_CHANS];
	struct mbox_chan chan[PL320_MAX_CHANS];
};

enum mbox_type {
	EXTER0_MBOX = 0,
	SOC_MAX_MBOX,
};

void quard_mailbox_ipi_controller_init(enum mbox_type type);
struct mbox_chan *mbox_request_channel(enum mbox_type type, struct mbox_client *client);
int mbox_send_message(struct mbox_chan *chan, void *mssg);

#define BITS_PER_LONG		(sizeof(unsigned long) * 8)

#define __GENMASK(h, l) \
	(((~(0UL)) - ((1UL) << (l)) + 1) & \
	 (~(0UL) >> (BITS_PER_LONG - 1 - (h))))

 static unsigned long __ffs(unsigned long word)
 {
	 int num = 0;
 
	 if ((word & 0xffff) == 0) {
		 num += 16;
		 word >>= 16;
	 }
	 if ((word & 0xff) == 0) {
		 num += 8;
		 word >>= 8;
	 }
	if ((word & 0xf) == 0) {
		num += 4;
		word >>= 4;
	}
	if ((word & 0x3) == 0) {
		num += 2;
		word >>= 2;
	}
	if ((word & 0x1) == 0)
		num += 1;

	return num;
}

static inline
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
			    unsigned long offset)
{
	unsigned long val;

	if (offset >= size)
		return size;

	val = *addr & __GENMASK(size - 1, offset);
	return val ? __ffs(val) : size;
}
	
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) = find_next_bit((addr), (size), (bit)), (bit) < (size); (bit)++)

#endif
