/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-08-14     Jackistang   add comments for function interface.
 */
#ifndef RINGBUFFER_H__
#define RINGBUFFER_H__

#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


#ifndef ASSERT
    #define ASSERT(x) \
        do { \
            if (!(x)) { \
                taskDISABLE_INTERRUPTS(); \
                for(;;); \
            } \
        } while(0)
#endif

/* ring buffer */
struct quard_ringbuffer
{
    uint8_t *buffer_ptr;
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
    int32_t buffer_size;
};

enum quard_ringbuffer_state
{
    QUARD_RINGBUFFER_EMPTY,
    QUARD_RINGBUFFER_FULL,
    /* half full is neither full nor empty */
    QUARD_RINGBUFFER_HALFFULL,
};

/**
 * RingBuffer for DeviceDriver
 *
 * Please note that the ring buffer implementation of RT-Thread
 * has no thread wait or resume feature.
 */
void quard_ringbuffer_init(struct quard_ringbuffer *rb, uint8_t *pool, int32_t size);
void quard_ringbuffer_reset(struct quard_ringbuffer *rb);
size_t quard_ringbuffer_put(struct quard_ringbuffer *rb, const uint8_t *ptr, uint32_t length);
size_t quard_ringbuffer_put_force(struct quard_ringbuffer *rb, const uint8_t *ptr, uint32_t length);
size_t quard_ringbuffer_putchar(struct quard_ringbuffer *rb, const uint8_t ch);
size_t quard_ringbuffer_putchar_force(struct quard_ringbuffer *rb, const uint8_t ch);
size_t quard_ringbuffer_get(struct quard_ringbuffer *rb, uint8_t *ptr, uint32_t length);
size_t quard_ringbuffer_get_direct(struct quard_ringbuffer *rb, uint8_t **ptr);
size_t quard_ringbuffer_peek(struct quard_ringbuffer *rb, uint8_t **ptr);
size_t quard_ringbuffer_getchar(struct quard_ringbuffer *rb, uint8_t *ch);
size_t quard_ringbuffer_data_len(struct quard_ringbuffer *rb);

#ifdef RT_USING_HEAP
struct quard_ringbuffer* quard_ringbuffer_create(uint32_t length);
void quard_ringbuffer_destroy(struct quard_ringbuffer *rb);
#endif

static __inline enum quard_ringbuffer_state quard_ringbuffer_status(struct quard_ringbuffer *rb)
{
    if (rb->read_index == rb->write_index)
    {
        if (rb->read_mirror == rb->write_mirror)
            return QUARD_RINGBUFFER_EMPTY;
        else
            return QUARD_RINGBUFFER_FULL;
    }
    return QUARD_RINGBUFFER_HALFFULL;
}

/**
 * @brief Get the buffer size of the ring buffer object.
 *
 * @param rb        A pointer to the ring buffer object.
 *
 * @return  Buffer size.
 */
static __inline uint32_t quard_ringbuffer_get_size(struct quard_ringbuffer *rb)
{
    ASSERT(rb != NULL);
    return rb->buffer_size;
}

/** return the size of empty space in rb */
#define quard_ringbuffer_space_len(rb) ((rb)->buffer_size - quard_ringbuffer_data_len(rb))


#ifdef __cplusplus
}
#endif

#endif