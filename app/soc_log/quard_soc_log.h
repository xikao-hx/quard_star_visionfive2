#ifndef __QUARD_SOC_LOG_H__
#define __QUARD_SOC_LOG_H__

#include <stdint.h>

enum quard_log_buffer_state
{
    SLAVE_BUFFER_EMPTY  = 0,
    SLAVE_BUFFER_FULL   = 1,
};


/* ring buffer must not be modified */
struct quard_log_ringbuffer
{
    uint8_t * ptr_res0;
    /* use the msb of the {read,write}_index as mirror bit. You can see this as
     * if the buffer adds a virtual mirror and the pointers point either to the
     * normal or to the mirrored buffer. If the write_index has the same value
     * with the read_index, but in a different mirror, the buffer is full.
     * While if the write_index and the read_index are the same and within the
     * same mirror, the buffer is empty. The ASCII art of the ringbuffer is:
     *
     *          mirror = 0                    mirror = 1
     * +---+---+---+---+---+---+---+|+~~~+~~~+~~~+~~~+~~~+~~~+~~~+
     * | 0 | 1 | 2 | 3 | 4 | 5 | 6 ||| 0 | 1 | 2 | 3 | 4 | 5 | 6 | Full
     * +---+---+---+---+---+---+---+|+~~~+~~~+~~~+~~~+~~~+~~~+~~~+
     *  read_idx-^                   write_idx-^
     *
     * +---+---+---+---+---+---+---+|+~~~+~~~+~~~+~~~+~~~+~~~+~~~+
     * | 0 | 1 | 2 | 3 | 4 | 5 | 6 ||| 0 | 1 | 2 | 3 | 4 | 5 | 6 | Empty
     * +---+---+---+---+---+---+---+|+~~~+~~~+~~~+~~~+~~~+~~~+~~~+
     * read_idx-^ ^-write_idx
     */

    uint32_t read_mirror : 1;
    uint32_t read_index : 31;
    uint32_t write_mirror : 1;
    uint32_t write_index : 31;
    /* as we use msb of index as mirror bit, the size should be signed and
     * could only be positive. */
    uint32_t buffer_size;
};

typedef struct
{
    uint32_t head_magic;
    uint32_t buffer_state;
    struct quard_log_ringbuffer rb_ctrl;
    uint32_t middle_magic;
    uint32_t buffer_first_byte;
} slave_buffer_cb;

#define MAGIC_PATTERN   (0xF1F11F1F)


#endif /* __QUARD_SOC_LOG_H__ */
