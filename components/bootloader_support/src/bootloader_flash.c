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

#ifdef CONFIG_IDF_TARGET_ESP32

#include <stddef.h>

#include <bootloader_flash.h>
#include <esp_log.h>
#include <esp_spi_flash.h> /* including in bootloader for error values */

#ifndef BOOTLOADER_BUILD
/* Normal app version maps to esp_spi_flash.h operations...
 */
static const char *TAG = "bootloader_mmap";

static spi_flash_mmap_handle_t map;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    ESP_LOGF("FUNC", "bootloader_mmap");

    if (map) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* existing mapping in use... */
    }
    const void *result = NULL;
    uint32_t src_page = src_addr & ~(SPI_FLASH_MMU_PAGE_SIZE-1);
    size += (src_addr - src_page);
    esp_err_t err = spi_flash_mmap(src_page, size, SPI_FLASH_MMAP_DATA, &result, &map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_flash_mmap failed: 0x%x", err);
        return NULL;
    }
    return (void *)((intptr_t)result + (src_addr - src_page));
}

void bootloader_munmap(const void *mapping)
{
    ESP_LOGF("FUNC", "bootloader_munmap");

    if(mapping && map) {
        spi_flash_munmap(map);
    }
    map = 0;
}

esp_err_t bootloader_flash_read(size_t src, void *dest, size_t size)
{
    ESP_LOGF("FUNC", "bootloader_flash_read");

    return spi_flash_read(src, dest, size);
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size)
{
    ESP_LOGF("FUNC", "bootloader_flash_write");

    return spi_flash_write(dest_addr, src, size);
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    ESP_LOGF("FUNC", "bootloader_flash_erase_sector");

    return spi_flash_erase_sector(sector);
}

#else
/* Bootloader version, uses ROM functions only */
#include <soc/dport_reg.h>
#include <rom/spi_flash.h>
#include <rom/cache.h>

static const char *TAG = "bootloader_flash";

/* Use first 50 blocks in MMU for bootloader_mmap,
   50th block for bootloader_flash_read
*/
#define MMU_BLOCK0_VADDR  0x3f400000
#define MMU_BLOCK50_VADDR 0x3f720000
#define MMU_FLASH_MASK    0xffff0000
#define MMU_BLOCK_SIZE    0x00010000

static bool mapped;

// Current bootloader mapping (ab)used for bootloader_read()
static uint32_t current_read_mapping = UINT32_MAX;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    ESP_LOGF("FUNC", "bootloader_mmap");

    if (mapped) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* can't map twice */
    }
    if (size > 0x320000) {
        /* Allow mapping up to 50 of the 51 available MMU blocks (last one used for reads) */
        ESP_LOGE(TAG, "bootloader_mmap excess size %x", size);
        return NULL;
    }

    uint32_t src_addr_aligned = src_addr & MMU_FLASH_MASK;
    uint32_t count = (size + (src_addr - src_addr_aligned) + 0xffff) / MMU_BLOCK_SIZE;
    Cache_Read_Disable(0);
    Cache_Flush(0);
    ESP_LOGD(TAG, "mmu set paddr=%08x count=%d", src_addr_aligned, count );
    int e = cache_flash_mmu_set(0, 0, MMU_BLOCK0_VADDR, src_addr_aligned, 64, count);
    if (e != 0) {
        ESP_LOGE(TAG, "cache_flash_mmu_set failed: %d\n", e);
        Cache_Read_Enable(0);
        return NULL;
    }
    Cache_Read_Enable(0);

    mapped = true;

    return (void *)(MMU_BLOCK0_VADDR + (src_addr - src_addr_aligned));
}

void bootloader_munmap(const void *mapping)
{
    ESP_LOGF("FUNC", "bootloader_munmap");

    if (mapped)  {
        /* Full MMU reset */
        Cache_Read_Disable(0);
        Cache_Flush(0);
        mmu_init(0);
        mapped = false;
        current_read_mapping = UINT32_MAX;
    }
}

static esp_err_t spi_to_esp_err(esp_rom_spiflash_result_t r)
{
    ESP_LOGF("FUNC", "spi_to_esp_err");

    switch(r) {
    case ESP_ROM_SPIFLASH_RESULT_OK:
        return ESP_OK;
    case ESP_ROM_SPIFLASH_RESULT_ERR:
        return ESP_ERR_FLASH_OP_FAIL;
    case ESP_ROM_SPIFLASH_RESULT_TIMEOUT:
        return ESP_ERR_FLASH_OP_TIMEOUT;
    default:
        return ESP_FAIL;
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

    Cache_Read_Disable(0);
    Cache_Flush(0);
    esp_rom_spiflash_result_t r = esp_rom_spiflash_read(src_addr, dest, size);
    Cache_Read_Enable(0);

    return spi_to_esp_err(r);
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size)
{
    ESP_LOGF("FUNC", "bootloader_flash_write");

    esp_err_t err;
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

    err = spi_to_esp_err(esp_rom_spiflash_unlock());
    if (err != ESP_OK) {
        return err;
    }

    return spi_to_esp_err(esp_rom_spiflash_write(dest_addr, src, size));
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    ESP_LOGF("FUNC", "bootloader_flash_erase_sector");

    return spi_to_esp_err(esp_rom_spiflash_erase_sector(sector));
}

#endif

#endif

#ifdef CONFIG_IDF_TARGET_ESP8266

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

#endif
