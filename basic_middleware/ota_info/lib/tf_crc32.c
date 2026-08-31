#include <stdint.h>
#include <stddef.h>
#include "tf_crc32.h"

uint32_t tf_crc32(uint32_t crc, const unsigned char *buf, size_t size)
{
    uint32_t i;
    // 标准 CRC32 多项式: 0xEDB88320
    crc = ~crc;
    while (size--) {
        crc ^= *buf++;
        for (i = 0; i < 8; i++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
    }
    return ~crc;
}