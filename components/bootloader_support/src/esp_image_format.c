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

#include <string.h>
#include <stdlib.h>
#include <sys/param.h>

#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_spi_flash.h>
#include <bootloader_flash.h>
#include <bootloader_random.h>
#include <bootloader_sha.h>

static const char *TAG = "esp_image";

#define HASH_LEN 32 /* SHA-256 digest length */

#define SHA_CHUNK 1024

#define MAX_CHECKSUM_READ_SIZE SPI_FLASH_SEC_SIZE

#define SIXTEEN_MB 0x1000000
#define ESP_ROM_CHECKSUM_INITIAL 0xEF

/* Headroom to ensure between stack SP (at time of checking) and data loaded from flash */
#define STACK_LOAD_HEADROOM 32768

/* Mmap source address mask */
#define MMAP_ALIGNED_MASK 0x0000FFFF

#if defined(BOOTLOADER_BUILD) && defined(BOOTLOADER_UNPACK_APP)
/* 64 bits of random data to obfuscate loaded RAM with, until verification is complete
   (Means loaded code isn't executable until after the secure boot check.)
*/
static uint32_t ram_obfs_value[2];
#endif

/* Return true if load_addr is an address the bootloader should load into */
static bool should_load(uint32_t load_addr);
/* Return true if load_addr is an address the bootloader should map via flash cache */
static bool should_map(uint32_t load_addr);

/* Load or verify a segment */
static esp_err_t process_segment(int index, uint32_t flash_addr, esp_image_segment_header_t *header, bool silent, bool do_load, bootloader_sha256_handle_t sha_handle, uint32_t *checksum);

/* split segment and verify if data_len is too long */
static esp_err_t process_segment_data(intptr_t load_addr, uint32_t data_addr, uint32_t data_len, bool do_load, bootloader_sha256_handle_t sha_handle, uint32_t *checksum);

/* Verify the main image header */
static esp_err_t verify_image_header(uint32_t src_addr, const esp_image_header_t *image, bool silent);

/* Verify a segment header */
static esp_err_t verify_segment_header(int index, const esp_image_segment_header_t *segment, uint32_t segment_data_offs, bool silent);

/* Log-and-fail macro for use in esp_image_load */
#define FAIL_LOAD(...) do {                         \
        if (!silent) {                              \
            ESP_LOGE(TAG, __VA_ARGS__);             \
        }                                           \
        goto err;                                   \
    }                                               \
    while(0)

static esp_err_t verify_checksum(bootloader_sha256_handle_t sha_handle, uint32_t checksum_word, esp_image_metadata_t *data);

esp_err_t esp_image_load(esp_image_load_mode_t mode, const esp_partition_pos_t *part, esp_image_metadata_t *data)
{
    ESP_LOGF("FUNC", "esp_image_load");

#ifdef BOOTLOADER_BUILD
    bool do_load = (mode == ESP_IMAGE_LOAD);
#else
    bool do_load = false; // Can't load the image in app mode
#endif
    bool silent = (mode == ESP_IMAGE_VERIFY_SILENT);
    esp_err_t err = ESP_OK;
    // checksum the image a word at a time. This shaves 30-40ms per MB of image size
    uint32_t checksum_word = ESP_ROM_CHECKSUM_INITIAL;
    bootloader_sha256_handle_t sha_handle = NULL;

    if (data == NULL || part == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (part->size > SIXTEEN_MB) {
        err = ESP_ERR_INVALID_ARG;
        FAIL_LOAD("partition size 0x%x invalid, larger than 16MB", part->size);
    }

    bzero(data, sizeof(esp_image_metadata_t));
    data->start_addr = part->offset;

    ESP_LOGD(TAG, "reading image header @ 0x%x", data->start_addr);
    err = bootloader_flash_read(data->start_addr, &data->image, sizeof(esp_image_header_t), true);
    if (err != ESP_OK) {
        goto err;
    }

    ESP_LOGD(TAG, "image header: 0x%02x 0x%02x 0x%02x 0x%02x %08x",
             data->image.magic,
             data->image.segment_count,
             data->image.spi_mode,
             data->image.spi_size,
             data->image.entry_addr);

    err = verify_image_header(data->start_addr, &data->image, silent);
    if (err != ESP_OK) {
goto err;
    }

    if (data->image.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        FAIL_LOAD("image at 0x%x segment count %d exceeds max %d",
                  data->start_addr, data->image.segment_count, ESP_IMAGE_MAX_SEGMENTS);
    }

    uint32_t next_addr = data->start_addr + sizeof(esp_image_header_t);

    for(int i = 0; i < data->image.segment_count; i++) {
        esp_image_segment_header_t *header = &data->segments[i];
        ESP_LOGV(TAG, "loading segment header %d at offset 0x%x", i, next_addr);
        err = process_segment(i, next_addr, header, silent, do_load, sha_handle, &checksum_word);
        if (err != ESP_OK) {
            goto err;
        }
        next_addr += sizeof(esp_image_segment_header_t);
        data->segment_data[i] = next_addr;
        next_addr += header->data_len;
    }

    // Segments all loaded, verify length
    uint32_t end_addr = next_addr;
    if (end_addr < data->start_addr) {
        FAIL_LOAD("image offset has wrapped");
    }

    data->image_len = end_addr - data->start_addr;
    ESP_LOGV(TAG, "image start 0x%08x end of last section 0x%08x", data->start_addr, end_addr);
#ifdef CONFIG_ENABLE_BOOT_CHECK_OCD
    if (!esp_cpu_in_ocd_debug_mode()) {
#endif
        err = verify_checksum(sha_handle, checksum_word, data);
        if (err != ESP_OK) {
            goto err;
        }
#ifdef CONFIG_ENABLE_BOOT_CHECK_OCD
    }
#endif
    if (data->image_len > part->size) {
        FAIL_LOAD("Image length %d doesn't fit in partition length %d", data->image_len, part->size);
    }

    sha_handle = NULL;
    if (err != ESP_OK) {
        goto err;
    }

#if defined(BOOTLOADER_BUILD) && defined(BOOTLOADER_UNPACK_APP)
    if (do_load) { // Need to deobfuscate RAM
        for (int i = 0; i < data->image.segment_count; i++) {
            uint32_t load_addr = data->segments[i].load_addr;
            if (should_load(load_addr)) {
                uint32_t *loaded = (uint32_t *)load_addr;
                for (int j = 0; j < data->segments[i].data_len/sizeof(uint32_t); j++) {
                    loaded[j] ^= (j & 1) ? ram_obfs_value[0] : ram_obfs_value[1];
                }
            }
        }
    }
#endif

    // Success!
    return ESP_OK;

 err:
    if (err == ESP_OK) {
      err = ESP_ERR_IMAGE_INVALID;
    }
    // Prevent invalid/incomplete data leaking out
    bzero(data, sizeof(esp_image_metadata_t));
    return err;
}

static esp_err_t verify_image_header(uint32_t src_addr, const esp_image_header_t *image, bool silent)
{
    ESP_LOGF("FUNC", "verify_image_header");

    esp_err_t err = ESP_OK;

    if (image->magic != ESP_IMAGE_HEADER_MAGIC) {
        if (!silent) {
            ESP_LOGE(TAG, "image at 0x%x has invalid magic byte", src_addr);
        }
        err = ESP_ERR_IMAGE_INVALID;
    }
    if (!silent) {
        if (image->spi_mode > ESP_IMAGE_SPI_MODE_SLOW_READ) {
            ESP_LOGW(TAG, "image at 0x%x has invalid SPI mode %d", src_addr, image->spi_mode);
        }
        if (image->spi_speed > ESP_IMAGE_SPI_SPEED_80M) {
            ESP_LOGW(TAG, "image at 0x%x has invalid SPI speed %d", src_addr, image->spi_speed);
        }
        if (image->spi_size > ESP_IMAGE_FLASH_SIZE_MAX) {
            ESP_LOGW(TAG, "image at 0x%x has invalid SPI size %d", src_addr, image->spi_size);
        }
    }
    return err;
}

static esp_err_t process_segment(int index, uint32_t flash_addr, esp_image_segment_header_t *header, bool silent, bool do_load, bootloader_sha256_handle_t sha_handle, uint32_t *checksum)
{
    ESP_LOGF("FUNC", "process_segment");

    esp_err_t err;

    /* read segment header */
    memset(header, 0, sizeof(esp_image_segment_header_t));
    err = bootloader_flash_read(flash_addr, header, sizeof(esp_image_segment_header_t), true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bootloader_flash_read failed at 0x%08x", flash_addr);
        return err;
    }

    intptr_t load_addr = header->load_addr;
    uint32_t data_len = header->data_len;
    uint32_t data_addr = flash_addr + sizeof(esp_image_segment_header_t);

    ESP_LOGV(TAG, "segment data length 0x%x data starts 0x%x", data_len, data_addr);

    err = verify_segment_header(index, header, data_addr, silent);
    if (err != ESP_OK) {
        return err;
    }

    if (data_len % 4 != 0) {
        FAIL_LOAD("unaligned segment length 0x%x", data_len);
    }

    bool is_mapping = should_map(load_addr);
    do_load = do_load && should_load(load_addr);

    if (!silent) {
        ESP_LOGI(TAG, "segment %d: paddr=0x%08x vaddr=0x%08x size=0x%05x (%6d) %s",
                 index, data_addr, load_addr,
                 data_len, data_len,
                 (do_load)?"load":(is_mapping)?"map":"");
    }

#ifdef BOOTLOADER_UNPACK_APP
    if (do_load) {
        /* Before loading segment, check it doesn't clobber bootloader RAM... */
        uint32_t end_addr = load_addr + data_len;
        if (end_addr < 0x40000000) {
           intptr_t sp = (intptr_t)get_sp();
           if (end_addr > sp - STACK_LOAD_HEADROOM) {
               ESP_LOGE(TAG, "Segment %d end address 0x%08x too high (bootloader stack 0x%08x liimit 0x%08x)",
                        index, end_addr, sp, sp - STACK_LOAD_HEADROOM);
               return ESP_ERR_IMAGE_INVALID;
           }
        }
    }
#endif
#if !defined(BOOTLOADER_BUILD) && defined(CONFIG_ENABLE_FLASH_MMAP)
    uint32_t free_page_count = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    ESP_LOGD(TAG, "free data page_count 0x%08x",free_page_count);
    uint32_t offset_page = 0;
    while (data_len >= free_page_count * SPI_FLASH_MMU_PAGE_SIZE) {
        offset_page = ((data_addr & MMAP_ALIGNED_MASK) != 0)?1:0;
        err = process_segment_data(load_addr, data_addr, (free_page_count - offset_page) * SPI_FLASH_MMU_PAGE_SIZE, do_load, sha_handle, checksum);
        if (err != ESP_OK) {
            return err;
        }
        data_addr += (free_page_count - offset_page) * SPI_FLASH_MMU_PAGE_SIZE;
        data_len -= (free_page_count - offset_page) * SPI_FLASH_MMU_PAGE_SIZE;
    }
#endif
    err = process_segment_data(load_addr, data_addr, data_len, do_load, sha_handle, checksum);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;

err:
    if (err == ESP_OK) {
        err = ESP_ERR_IMAGE_INVALID;
    }

    return err;
}

static esp_err_t process_segment_data(intptr_t load_addr, uint32_t data_addr, uint32_t data_len, bool do_load, bootloader_sha256_handle_t sha_handle, uint32_t *checksum)
{
    ESP_LOGF("FUNC", "process_segment_data");

    esp_err_t ret = ESP_OK;
    return ret;
}

static esp_err_t verify_segment_header(int index, const esp_image_segment_header_t *segment, uint32_t segment_data_offs, bool silent)
{
    ESP_LOGF("FUNC", "verify_segment_header");

    return ESP_OK;
}

static bool should_map(uint32_t load_addr)
{
    ESP_LOGF("FUNC", "should_map");

    return (load_addr >= 0x40200000 && load_addr < 0x40300000);
}

static bool should_load(uint32_t load_addr)
{
    ESP_LOGF("FUNC", "should_load");

    if (should_map(load_addr)) {
        return false;
    }

    return true;
}

esp_err_t esp_image_verify_bootloader(uint32_t *length)
{
    ESP_LOGF("FUNC", "esp_image_verify_bootloader");

    esp_image_metadata_t data;
    const esp_partition_pos_t bootloader_part = {
        .offset = ESP_BOOTLOADER_OFFSET,
        .size = ESP_PARTITION_TABLE_OFFSET - ESP_BOOTLOADER_OFFSET,
    };
    esp_err_t err = esp_image_load(ESP_IMAGE_VERIFY,
                                   &bootloader_part,
                                   &data);
    if (length != NULL) {
        *length = (err == ESP_OK) ? data.image_len : 0;
    }
    return err;
}

static esp_err_t verify_checksum(bootloader_sha256_handle_t sha_handle, uint32_t checksum_word, esp_image_metadata_t *data)
{
    ESP_LOGF("FUNC", "verify_checksum");

    esp_err_t err = ESP_OK;
    return err;
}
