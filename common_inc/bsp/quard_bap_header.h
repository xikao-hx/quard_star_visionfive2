#ifndef QUARD_BAP_HEADER_H
#define QUARD_BAP_HEADER_H

#include <stdint.h>

/* 定义魔数，用于识别 OTA 镜像 */
#define OTA_MAGIC 0x51535452  // "QSTR" (Quard STaR)

#define QUARD_OTA_OK      0
#define QUARD_OTA_ERROR  -1

struct binary_header {
    /* 前 8 字节：不参与 Header CRC 计算 */
    uint32_t magic;             
    uint32_t version;          

    /* 参与 Header CRC 计算的起始位置 */
    uint32_t image_length;      
    uint32_t binary_checksum;   
    
    uint32_t target_address;    
    uint32_t timestamp;         
    uint8_t  reserved[36];      
    uint32_t header_checksum;
} __attribute__((packed));

#endif /* QUARD_BAP_HEADER_H */
