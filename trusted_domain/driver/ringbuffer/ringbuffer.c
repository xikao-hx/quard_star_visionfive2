/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-09-30     Bernard      first version.
 * 2013-05-08     Grissiom     reimplement
 * 2016-08-18     heyuanjie    add interface
 * 2021-07-20     arminker     fix write_index bug in function rt_ringbuffer_put_force
 * 2021-08-14     Jackistang   add comments for function interface.
 */

#include "ringbuffer.h"
#include <string.h>

#ifndef ALIGN_SIZE
#define ALIGN_SIZE           4
#endif

#ifndef ALIGN_DOWN
#define ALIGN_DOWN(size, align)      ((size) & ~((align) - 1))
#endif

#define malloc pvPortMalloc
#define free vPortFree

/**
 * @brief Initialize the ring buffer object.
 *
 * @param rb        A pointer to the ring buffer object.
 * @param pool      A pointer to the buffer.
 * @param size      The size of the buffer in bytes.
 */
void quard_ringbuffer_init(struct quard_ringbuffer *rb,
                        uint8_t           *pool,
                        int32_t            size)
{
    ASSERT(rb != NULL);
    ASSERT(size > 0);

    /* initialize read and write index */
    rb->read_mirror = rb->read_index = 0;
    rb->write_mirror = rb->write_index = 0;

    /* set buffer pool and size */
    rb->buffer_ptr = pool;
    rb->buffer_size = ALIGN_DOWN(size, ALIGN_SIZE);
}
//RTM_EXPORT(quard_ringbuffer_init);

/**
 * @brief Put a block of data into the ring buffer. If the capacity of ring buffer is insufficient, it will discard out-of-range data.
 *
 * @param rb            A pointer to the ring buffer object.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of data in bytes.
 *
 * @return Return the data size we put into the ring buffer.
 */
size_t quard_ringbuffer_put(struct quard_ringbuffer *rb,
                            const uint8_t     *ptr,
                            uint32_t           length)
{
    uint32_t size;

    ASSERT(rb != NULL);

    /* whether has enough space */
    size = quard_ringbuffer_space_len(rb);

    /* no space */
    if (size == 0)
        return 0;

    /* drop some data */
    if (size < length)
        length = size;

    if (rb->buffer_size - rb->write_index > length)
    {
        /* read_index - write_index = empty space */
        memcpy(&rb->buffer_ptr[rb->write_index], ptr, length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->write_index += length;
        return length;
    }

    memcpy(&rb->buffer_ptr[rb->write_index],
              &ptr[0],
              rb->buffer_size - rb->write_index);
    memcpy(&rb->buffer_ptr[0],
              &ptr[rb->buffer_size - rb->write_index],
              length - (rb->buffer_size - rb->write_index));

    /* we are going into the other side of the mirror */
    rb->write_mirror = ~rb->write_mirror;
    rb->write_index = length - (rb->buffer_size - rb->write_index);

    return length;
}
//RTM_EXPORT(quard_ringbuffer_put);

/**
 * @brief Put a block of data into the ring buffer. If the capacity of ring buffer is insufficient, it will overwrite the existing data in the ring buffer.
 *
 * @param rb            A pointer to the ring buffer object.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of data in bytes.
 *
 * @return Return the data size we put into the ring buffer.
 */
size_t quard_ringbuffer_put_force(struct quard_ringbuffer *rb,
                                  const uint8_t     *ptr,
                                  uint32_t           length)
{
    uint32_t space_length;

    ASSERT(rb != NULL);

    space_length = quard_ringbuffer_space_len(rb);

    if (length > rb->buffer_size)
    {
        ptr = &ptr[length - rb->buffer_size];
        length = rb->buffer_size;
    }

    if (rb->buffer_size - rb->write_index > length)
    {
        /* read_index - write_index = empty space */
        memcpy(&rb->buffer_ptr[rb->write_index], ptr, length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->write_index += length;

        if (length > space_length)
            rb->read_index = rb->write_index;

        return length;
    }

    memcpy(&rb->buffer_ptr[rb->write_index],
              &ptr[0],
              rb->buffer_size - rb->write_index);
    memcpy(&rb->buffer_ptr[0],
              &ptr[rb->buffer_size - rb->write_index],
              length - (rb->buffer_size - rb->write_index));

    /* we are going into the other side of the mirror */
    rb->write_mirror = ~rb->write_mirror;
    rb->write_index = length - (rb->buffer_size - rb->write_index);

    if (length > space_length)
    {
        if (rb->write_index <= rb->read_index)
            rb->read_mirror = ~rb->read_mirror;
        rb->read_index = rb->write_index;
    }

    return length;
}
//RTM_EXPORT(quard_ringbuffer_put_force);

/**
 * @brief Get data from the ring buffer.
 *
 * @param rb            A pointer to the ring buffer.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of the data we want to read from the ring buffer.
 *
 * @return Return the data size we read from the ring buffer.
 */
size_t quard_ringbuffer_get(struct quard_ringbuffer *rb,
                            uint8_t           *ptr,
                            uint32_t           length)
{
    size_t size;

    ASSERT(rb != NULL);

    /* whether has enough data  */
    size = quard_ringbuffer_data_len(rb);

    /* no data */
    if (size == 0)
        return 0;

    /* less data */
    if (size < length)
        length = size;

    if (rb->buffer_size - rb->read_index > length)
    {
        /* copy all of data */
        memcpy(ptr, &rb->buffer_ptr[rb->read_index], length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->read_index += length;
        return length;
    }

    memcpy(&ptr[0],
              &rb->buffer_ptr[rb->read_index],
              rb->buffer_size - rb->read_index);
    memcpy(&ptr[rb->buffer_size - rb->read_index],
              &rb->buffer_ptr[0],
              length - (rb->buffer_size - rb->read_index));

    /* we are going into the other side of the mirror */
    rb->read_mirror = ~rb->read_mirror;
    rb->read_index = length - (rb->buffer_size - rb->read_index);

    return length;
}
//RTM_EXPORT(quard_ringbuffer_get);

/**
 * @brief Get data from the ring buffer in zero-copy mode.
 *
 * @param rb        A pointer to the ringbuffer.
 * @param ptr       When this function return, *ptr is a pointer to the first readable byte of the ring buffer.
 *
 * @note This function returns a direct pointer to the internal buffer and consumes the data
 *       (advances read_index). It returns the contiguous readable data length. If data wraps
 *       around the buffer end, call this function again to get the remaining segment.
 *
 * @return Return the contiguous readable data size we consumed from the ring buffer.
 */
size_t quard_ringbuffer_get_direct(struct quard_ringbuffer *rb, uint8_t **ptr)
{
    size_t size;

    ASSERT(rb != NULL);

    *ptr = NULL;

    /* whether has enough data  */
    size = quard_ringbuffer_data_len(rb);

    /* no data */
    if (size == 0)
        return 0;

    *ptr = &rb->buffer_ptr[rb->read_index];

    if ((size_t)(rb->buffer_size - rb->read_index) > size)
    {
        rb->read_index += size;
        return size;
    }

    size = rb->buffer_size - rb->read_index;

    /* we are going into the other side of the mirror */
    rb->read_mirror = ~rb->read_mirror;
    rb->read_index = 0;

    return size;
}
//RTM_EXPORT(quard_ringbuffer_get_direct);

/**
 * @brief Get the first readable byte of the ring buffer.
 *
 * @param rb        A pointer to the ringbuffer.
 * @param ptr       When this function return, *ptr is a pointer to the first readable byte of the ring buffer.
 *
 * @note It is recommended to read only one byte, otherwise it may cause buffer overflow.
 *
 * @return Return the size of the ring buffer.
 */
size_t quard_ringbuffer_peek(struct quard_ringbuffer *rb, uint8_t **ptr)
{
    size_t size;

    ASSERT(rb != NULL);

    *ptr = NULL;

    /* whether has enough data  */
    size = quard_ringbuffer_data_len(rb);

    /* no data */
    if (size == 0)
        return 0;

    *ptr = &rb->buffer_ptr[rb->read_index];

    if ((size_t)(rb->buffer_size - rb->read_index) > size)
    {
        return size;
    }

    size = rb->buffer_size - rb->read_index;

    return size;
}
//RTM_EXPORT(quard_ringbuffer_peek);

/**
 * @brief Put a byte into the ring buffer. If ring buffer is full, this operation will fail.
 *
 * @param rb        A pointer to the ring buffer object.
 * @param ch        A byte put into the ring buffer.
 *
 * @return Return the data size we put into the ring buffer. The ring buffer is full if returns 0. Otherwise, it will return 1.
 */
size_t quard_ringbuffer_putchar(struct quard_ringbuffer *rb, const uint8_t ch)
{
    ASSERT(rb != NULL);

    /* whether has enough space */
    if (!quard_ringbuffer_space_len(rb))
        return 0;

    rb->buffer_ptr[rb->write_index] = ch;

    /* flip mirror */
    if (rb->write_index == rb->buffer_size - 1)
    {
        rb->write_mirror = ~rb->write_mirror;
        rb->write_index = 0;
    }
    else
    {
        rb->write_index++;
    }

    return 1;
}
//RTM_EXPORT(quard_ringbuffer_putchar);

/**
 * @brief Put a byte into the ring buffer. If ring buffer is full, it will discard an old data and put into a new data.
 *
 * @param rb        A pointer to the ring buffer object.
 * @param ch        A byte put into the ring buffer.
 *
 * @return Return the data size we put into the ring buffer. Always return 1.
 */
size_t quard_ringbuffer_putchar_force(struct quard_ringbuffer *rb, const uint8_t ch)
{
    enum quard_ringbuffer_state old_state;

    ASSERT(rb != NULL);

    old_state = quard_ringbuffer_status(rb);

    rb->buffer_ptr[rb->write_index] = ch;

    /* flip mirror */
    if (rb->write_index == rb->buffer_size - 1)
    {
        rb->write_mirror = ~rb->write_mirror;
        rb->write_index = 0;
        if (old_state == QUARD_RINGBUFFER_FULL)
        {
            rb->read_mirror = ~rb->read_mirror;
            rb->read_index = rb->write_index;
        }
    }
    else
    {
        rb->write_index++;
        if (old_state == QUARD_RINGBUFFER_FULL)
            rb->read_index = rb->write_index;
    }

    return 1;
}

/**
 * @brief Get a byte from the ring buffer.
 *
 * @param rb        The pointer to the ring buffer object.
 * @param ch        A pointer to the buffer, used to store one byte.
 *
 * @return 0    The ring buffer is empty.
 * @return 1    Success
 */
size_t quard_ringbuffer_getchar(struct quard_ringbuffer *rb, uint8_t *ch)
{
    ASSERT(rb != NULL);

    /* ringbuffer is empty */
    if (!quard_ringbuffer_data_len(rb))
        return 0;

    /* put byte */
    *ch = rb->buffer_ptr[rb->read_index];

    if (rb->read_index == rb->buffer_size - 1)
    {
        rb->read_mirror = ~rb->read_mirror;
        rb->read_index = 0;
    }
    else
    {
        rb->read_index++;
    }

    return 1;
}
//RTM_EXPORT(quard_ringbuffer_getchar);

/**
 * @brief Get the size of data in the ring buffer in bytes.
 *
 * @param rb        The pointer to the ring buffer object.
 *
 * @return Return the size of data in the ring buffer in bytes.
 */
size_t quard_ringbuffer_data_len(struct quard_ringbuffer *rb)
{
    switch (quard_ringbuffer_status(rb))
    {
    case QUARD_RINGBUFFER_EMPTY:
        return 0;
    case QUARD_RINGBUFFER_FULL:
        return rb->buffer_size;
    case QUARD_RINGBUFFER_HALFFULL:
    default:
    {
        size_t wi = rb->write_index, ri = rb->read_index;

        if (wi > ri)
            return wi - ri;
        else
            return rb->buffer_size - (ri - wi);
    }
    }
}
//RTM_EXPORT(quard_ringbuffer_data_len);

/**
 * @brief Reset the ring buffer object, and clear all contents in the buffer.
 *
 * @param rb        A pointer to the ring buffer object.
 */
void quard_ringbuffer_reset(struct quard_ringbuffer *rb)
{
    ASSERT(rb != NULL);

    rb->read_mirror = 0;
    rb->read_index = 0;
    rb->write_mirror = 0;
    rb->write_index = 0;
}
//RTM_EXPORT(quard_ringbuffer_reset);

#ifdef QUARD_USING_HEAP

/**
 * @brief Create a ring buffer object with a given size.
 *
 * @param size      The size of the buffer in bytes.
 *
 * @return Return a pointer to ring buffer object. When the return value is NULL, it means this creation failed.
 */
struct quard_ringbuffer *quard_ringbuffer_create(uint32_t size)
{
    struct quard_ringbuffer *rb;
    uint8_t *pool;

    ASSERT(size > 0);

    size = ALIGN_DOWN(size, ALIGN_SIZE);

    rb = (struct quard_ringbuffer *)malloc(sizeof(struct quard_ringbuffer));
    if (rb == NULL)
        goto exit;

    pool = (uint8_t *)malloc(size);
    if (pool == NULL)
    {
        free(rb);
        rb = NULL;
        goto exit;
    }
    quard_ringbuffer_init(rb, pool, size);

exit:
    return rb;
}
//RTM_EXPORT(quard_ringbuffer_create);

/**
 * @brief Destroy the ring buffer object, which is created by quard_ringbuffer_create() .
 *
 * @param rb        A pointer to the ring buffer object.
 */
void quard_ringbuffer_destroy(struct quard_ringbuffer *rb)
{
    ASSERT(rb != NULL);

    free(rb->buffer_ptr);
    free(rb);
}
//RTM_EXPORT(quard_ringbuffer_destroy);

#endif