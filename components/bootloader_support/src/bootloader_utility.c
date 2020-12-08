// Copyright 2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"

#include <stdbool.h>
#include <sys/param.h>
#include <string.h>

#include "bootloader_config.h"
#include "bootloader_utility.h"
#include "bootloader_flash.h"
#include "bootloader_common.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_flash_partitions.h"
#include "esp_private/esp_system_internal.h"

static const char* TAG = "boot";

bool bootloader_utility_load_partition_table(bootloader_state_t* bs)
{
    ESP_LOGF("FUNC", "bootloader_utility_load_partition_table");

    const esp_partition_info_t *partitions;
    const char *partition_usage;
    esp_err_t err;
    int num_partitions;

    rtc_sys_info.old_sysconf_addr = 0;
#ifdef CONFIG_ESP8266_OTA_FROM_OLD
    if (esp_patition_table_init_location()) {
        ESP_LOGE(TAG, "Failed to update partition table location");
        return false;
    }
#endif

    partitions = bootloader_mmap(ESP_PARTITION_TABLE_ADDR, ESP_PARTITION_TABLE_MAX_LEN);
    if (!partitions) {
            ESP_LOGE(TAG, "bootloader_mmap(0x%x, 0x%x) failed", ESP_PARTITION_TABLE_ADDR, ESP_PARTITION_TABLE_MAX_LEN);
            return false;
    }
    ESP_LOGD(TAG, "mapped partition table 0x%x at 0x%x", ESP_PARTITION_TABLE_ADDR, (intptr_t)partitions);

    err = esp_partition_table_basic_verify(partitions, true, &num_partitions);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify partition table");
        return false;
    }

    ESP_LOGI(TAG, "Partition Table:");
    ESP_LOGI(TAG, "## Label            Usage          Type ST Offset   Length");

    for(int i = 0; i < num_partitions; i++) {
//        const esp_partition_info_t *partition = &partitions[i];
        esp_partition_info_t partiton_local;
        esp_partition_info_t *partition = &partiton_local;

        memcpy(&partiton_local, (void *)((intptr_t)partitions + i * sizeof(esp_partition_info_t)), sizeof(esp_partition_info_t));

        ESP_LOGD(TAG, "load partition table entry 0x%x", (intptr_t)partition);
        ESP_LOGD(TAG, "type=%x subtype=%x", partition->type, partition->subtype);
        partition_usage = "unknown";

        /* valid partition table */
        switch(partition->type) {
        case PART_TYPE_APP: /* app partition */
            switch(partition->subtype) {
            case PART_SUBTYPE_FACTORY: /* factory binary */
                bs->factory = partition->pos;
                partition_usage = "factory app";
                break;
            case PART_SUBTYPE_TEST: /* test binary */
                bs->test = partition->pos;
                partition_usage = "test app";
                break;
            default:
                /* OTA binary */
                if ((partition->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
                    bs->ota[partition->subtype & PART_SUBTYPE_OTA_MASK] = partition->pos;
                    ++bs->app_count;
                    partition_usage = "OTA app";
                }
                else {
                    partition_usage = "Unknown app";
                }
                break;
            }
            break; /* PART_TYPE_APP */
        case PART_TYPE_DATA: /* data partition */
            switch(partition->subtype) {
            case PART_SUBTYPE_DATA_OTA: /* ota data */
                bs->ota_info = partition->pos;
                partition_usage = "OTA data";
                break;
            case PART_SUBTYPE_DATA_RF:
#ifdef CONFIG_LOAD_OLD_RF_PARAMETER
                bs->rf = partition->pos;
#endif
                partition_usage = "RF data";
                break;
            case PART_SUBTYPE_DATA_WIFI:
                partition_usage = "WiFi data";
                break;
            default:
                partition_usage = "Unknown data";
                break;
            }
            break; /* PARTITION_USAGE_DATA */
        default: /* other partition type */
            break;
        }

        /* print partition type info */
        ESP_LOGI(TAG, "%2d %-16s %-16s %02x %02x %08x %08x", i, partition->label, partition_usage,
                 partition->type, partition->subtype,
                 partition->pos.offset, partition->pos.size);
    }

    bootloader_munmap(partitions);

#ifdef CONFIG_ESP8266_OTA_FROM_OLD
    if (esp_patition_table_init_data(bs)) {
        ESP_LOGE(TAG,"Failed to update partition data");
        return false;
    }
#endif

    ESP_LOGI(TAG,"End of partition table");
    return true;
}

int bootloader_utility_get_selected_boot_partition(const bootloader_state_t *bs)
{
    ESP_LOGF("FUNC", "bootloader_utility_get_selected_boot_partition");

    esp_ota_select_entry_t sa,sb;
    const esp_ota_select_entry_t *ota_select_map;

    if (bs->ota_info.offset != 0) {
        // partition table has OTA data partition
        if (bs->ota_info.size < 2 * SPI_SEC_SIZE) {
            ESP_LOGE(TAG, "ota_info partition size %d is too small (minimum %d bytes)", bs->ota_info.size, sizeof(esp_ota_select_entry_t));
            return INVALID_INDEX; // can't proceed
        }

        ESP_LOGD(TAG, "OTA data offset 0x%x", bs->ota_info.offset);
        ota_select_map = bootloader_mmap(bs->ota_info.offset, bs->ota_info.size);
        if (!ota_select_map) {
            ESP_LOGE(TAG, "bootloader_mmap(0x%x, 0x%x) failed", bs->ota_info.offset, bs->ota_info.size);
            return INVALID_INDEX; // can't proceed
        }
        memcpy(&sa, ota_select_map, sizeof(esp_ota_select_entry_t));
        memcpy(&sb, (uint8_t *)ota_select_map + SPI_SEC_SIZE, sizeof(esp_ota_select_entry_t));
        bootloader_munmap(ota_select_map);

        ESP_LOGD(TAG, "OTA sequence values A 0x%08x B 0x%08x", sa.ota_seq, sb.ota_seq);
        if(sa.ota_seq == UINT32_MAX && sb.ota_seq == UINT32_MAX) {
            ESP_LOGD(TAG, "OTA sequence numbers both empty (all-0xFF)");
            if (bs->factory.offset != 0) {
                ESP_LOGI(TAG, "Defaulting to factory image");
                return FACTORY_INDEX;
            } else {
                ESP_LOGI(TAG, "No factory image, trying OTA 0");
                return 0;
            }
        } else  {
            bool ota_valid = false;
            const char *ota_msg;
            int ota_seq; // Raw OTA sequence number. May be more than # of OTA slots
            if(bootloader_common_ota_select_valid(&sa) && bootloader_common_ota_select_valid(&sb)) {
                ota_valid = true;
                ota_msg = "Both OTA values";
                ota_seq = MAX(sa.ota_seq, sb.ota_seq) - 1;
            } else if(bootloader_common_ota_select_valid(&sa)) {
                ota_valid = true;
                ota_msg = "Only OTA sequence A is";
                ota_seq = sa.ota_seq - 1;
            } else if(bootloader_common_ota_select_valid(&sb)) {
                ota_valid = true;
                ota_msg = "Only OTA sequence B is";
                ota_seq = sb.ota_seq - 1;
            }

            if (ota_valid) {
                int ota_slot = ota_seq % bs->app_count; // Actual OTA partition selection
                ESP_LOGD(TAG, "%s valid. Mapping seq %d -> OTA slot %d", ota_msg, ota_seq, ota_slot);
                return ota_slot;
            } else if (bs->factory.offset != 0) {
                ESP_LOGE(TAG, "ota data partition invalid, falling back to factory");
                return FACTORY_INDEX;
            } else {
                ESP_LOGE(TAG, "ota data partition invalid and no factory, will try all partitions");
                return FACTORY_INDEX;
            }
        }
    }

    // otherwise, start from factory app partition and let the search logic
    // proceed from there
    return FACTORY_INDEX;
}

/* Given a partition index, return the partition position data from the bootloader_state_t structure */
static esp_partition_pos_t index_to_partition(const bootloader_state_t *bs, int index)
{
    ESP_LOGF("FUNC", "index_to_partition");

    if (index == FACTORY_INDEX) {
        return bs->factory;
    }

    if (index == TEST_APP_INDEX) {
        return bs->test;
    }

    if (index >= 0 && index < MAX_OTA_SLOTS && index < bs->app_count) {
        return bs->ota[index];
    }

    esp_partition_pos_t invalid = { 0 };
    return invalid;
}

static void log_invalid_app_partition(int index)
{
    ESP_LOGF("FUNC", "log_invalid_app_partition");

    const char *not_bootable = " is not bootable"; /* save a few string literal bytes */
    switch(index) {
    case FACTORY_INDEX:
        ESP_LOGE(TAG, "Factory app partition%s", not_bootable);
        break;
    case TEST_APP_INDEX:
        ESP_LOGE(TAG, "Factory test app partition%s", not_bootable);
        break;
    default:
        ESP_LOGE(TAG, "OTA app partition slot %d%s", index, not_bootable);
        break;
    }
}

/* Return true if a partition has a valid app image that was successfully loaded */
static bool try_load_partition(const esp_partition_pos_t *partition, esp_image_metadata_t *data)
{
    ESP_LOGF("FUNC", "try_load_partition");

    if (partition->size == 0) {
        ESP_LOGD(TAG, "Can't boot from zero-length partition");
        return false;
    }
#ifdef BOOTLOADER_BUILD
    if (esp_image_load(ESP_IMAGE_LOAD, partition, data) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded app from partition at offset 0x%x",
                 partition->offset);
        return true;
    }
#endif

    return false;
}

#define TRY_LOG_FORMAT "Trying partition index %d offs 0x%x size 0x%x"

static void bootloader_utility_start_image(uint32_t image_start, uint32_t image_size, uint32_t entry_addr)
{
    ESP_LOGF("FUNC", "bootloader_utility_start_image");

    void (*user_start)(size_t start_addr);

    bootloader_mmap(image_start, image_size);

    user_start = (void *)entry_addr;
    user_start(image_start);
}

bool bootloader_utility_load_boot_image(const bootloader_state_t *bs, int start_index, esp_image_metadata_t *result)
{
    ESP_LOGF("FUNC", "bootloader_utility_load_boot_image");

    int index = start_index;
    esp_partition_pos_t part;
    if(start_index == TEST_APP_INDEX) {
        if (try_load_partition(&bs->test, result)) {
            return true;
        } else {
            ESP_LOGE(TAG, "No bootable test partition in the partition table");
            return false;
        }
    }
    /* work backwards from start_index, down to the factory app */
    for(index = start_index; index >= FACTORY_INDEX; index--) {
        part = index_to_partition(bs, index);
        if (part.size == 0) {
            continue;
        }
        ESP_LOGD(TAG, TRY_LOG_FORMAT, index, part.offset, part.size);
        if (try_load_partition(&part, result)) {
            return true;
        }
        log_invalid_app_partition(index);
    }

    /* failing that work forwards from start_index, try valid OTA slots */
    for(index = start_index + 1; index < bs->app_count; index++) {
        part = index_to_partition(bs, index);
        if (part.size == 0) {
            continue;
        }
        ESP_LOGD(TAG, TRY_LOG_FORMAT, index, part.offset, part.size);
        if (try_load_partition(&part, result)) {
            return true;
        }
        log_invalid_app_partition(index);
    }

    if (try_load_partition(&bs->test, result)) {
        ESP_LOGW(TAG, "Falling back to test app as only bootable partition");
        return true;
    }

    ESP_LOGE(TAG, "No bootable app partitions in the partition table");
    bzero(result, sizeof(esp_image_metadata_t));
    return false;
}

void bootloader_utility_load_image(const esp_image_metadata_t* image_data)
{
    ESP_LOGF("FUNC", "bootloader_utility_load_image");

#ifdef BOOTLOADER_UNPACK_APP
    ESP_LOGI(TAG, "Disabling RNG early entropy source...");
    bootloader_random_disable();

    copy loaded segments to RAM, set up caches for mapped segments, and start application
    unpack_load_app(image_data);
#else
    bootloader_utility_start_image(image_data->start_addr, image_data->image_len, image_data->image.entry_addr);
#endif /* BOOTLOADER_UNPACK_APP */
}
