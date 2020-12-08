// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#ifndef BOOTLOADER_BUILD
#include "esp_spi_flash.h"
#endif

#ifdef CONFIG_SOC_FULL_ICACHE
#define SOC_CACHE_SIZE 1 // 32KB
#else
#define SOC_CACHE_SIZE 0 // 16KB
#endif

extern void Cache_Read_Disable();
extern void Cache_Read_Enable(uint8_t map, uint8_t p, uint8_t v);

static const char *TAG = "bootloader_flash";

typedef enum { SPI_FLASH_RESULT_OK = 0,
               SPI_FLASH_RESULT_ERR = 1,
               SPI_FLASH_RESULT_TIMEOUT = 2 } SpiFlashOpResult;

SpiFlashOpResult SPIRead(uint32_t addr, void *dst, uint32_t size);
SpiFlashOpResult SPIWrite(uint32_t addr, const uint8_t *src, uint32_t size);
SpiFlashOpResult SPIEraseSector(uint32_t sector_num);

static bool mapped;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    ESP_LOGF("FUNC", "bootloader_mmap");

    if (mapped) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* can't map twice */
    }

    /* 0: 0x000000 - 0x1fffff */
    /* 1: 0x200000 - 0x3fffff */
    /* 2: 0x400000 - 0x5fffff */
    /* 3: 0x600000 - 0x7fffff */

    uint32_t region;
    uint32_t sub_region;
    uint32_t mapped_src;

    if (src_addr < 0x200000) {
        region = 0;
    } else if (src_addr < 0x400000) {
        region = 1;
    } else if (src_addr < 0x600000) {
        region = 2;
    } else if (src_addr < 0x800000) {
        region = 3;
    } else {
        ESP_LOGE(TAG, "flash mapped address %p is invalid", (void *)src_addr);
        while (1);
    }

    /* 0: 0x000000 - 0x0fffff \              */
    /*                         \             */
    /*                           0x40200000  */
    /*                         /             */
    /* 1: 0x100000 - 0x1fffff /              */
    mapped_src =  src_addr & 0x1fffff;
    if (mapped_src < 0x100000) {
        sub_region = 0;
    } else {
        sub_region = 1;
        mapped_src -= 0x100000;
    }

    Cache_Read_Disable();

    Cache_Read_Enable(sub_region, region, SOC_CACHE_SIZE);

    mapped = true;

    return (void *)(0x40200000 + mapped_src);
}

void bootloader_munmap(const void *mapping)
{
    ESP_LOGF("FUNC", "bootloader_munmap");

    if (mapped)  {
        Cache_Read_Disable();
        mapped = false;
    }
}

esp_err_t bootloader_flash_read(size_t src_addr, void *dest, size_t size)
{
    ESP_LOGF("FUNC", "bootloader_flash_read");

    if (src_addr & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read src_addr 0x%x not 4-byte aligned", src_addr);
        return ESP_FAIL;
    }
    if (size & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read size 0x%x not 4-byte aligned", size);
        return ESP_FAIL;
    }
    if ((intptr_t)dest & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read dest 0x%x not 4-byte aligned", (intptr_t)dest);
        return ESP_FAIL;
    }

#ifdef BOOTLOADER_BUILD
    SPIRead(src_addr, dest, size);
#else
    spi_flash_read(src_addr, dest, size);
#endif

    return ESP_OK;
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size)
{
    ESP_LOGF("FUNC", "bootloader_flash_write");

    size_t alignment = 4;
    if ((dest_addr % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write dest_addr 0x%x not %d-byte aligned", dest_addr, alignment);
        return ESP_FAIL;
    }
    if ((size % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write size 0x%x not %d-byte aligned", size, alignment);
        return ESP_FAIL;
    }
    if (((intptr_t)src % 4) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write src 0x%x not 4 byte aligned", (intptr_t)src);
        return ESP_FAIL;
    }

    SPIWrite(dest_addr, src, size);

    return ESP_OK;
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    ESP_LOGF("FUNC", "bootloader_flash_erase_sector");

    SPIEraseSector(sector);

    return ESP_OK;
}
