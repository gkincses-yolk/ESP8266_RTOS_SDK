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
#include <stdint.h>

#include <xtensa/hal.h>

#include "esp_err.h"
#include "esp_log.h"
#include "rom/crc.h"

#include "rom/gpio.h"

#include "bootloader_config.h"
#include "bootloader_common.h"

static const char *TAG = "bootloader_common";

uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t *s)
{
    ESP_LOGF("FUNC", "bootloader_common_ota_select_crc");

    return crc32_le(UINT32_MAX, (uint8_t*)&s->ota_seq, 4);
}

bool bootloader_common_ota_select_valid(const esp_ota_select_entry_t *s)
{
    ESP_LOGF("FUNC", "bootloader_common_ota_select_valid");

    return s->ota_seq != UINT32_MAX && s->crc == bootloader_common_ota_select_crc(s);
}

esp_comm_gpio_hold_t bootloader_common_check_long_hold_gpio(uint32_t num_pin, uint32_t delay_sec)
{
    ESP_LOGF("FUNC", "bootloader_common_check_long_hold_gpio");

    gpio_pad_select_gpio(num_pin);
    gpio_pad_pullup(num_pin);

    uint32_t tm_start = esp_log_early_timestamp();
    if (GPIO_INPUT_GET(num_pin) == 1) {
        ESP_LOGD(TAG, "gpio %d input %x", num_pin, GPIO_INPUT_GET(num_pin));
        return GPIO_NOT_HOLD;
    }
    do {
        if (GPIO_INPUT_GET(num_pin) != 0) {
            ESP_LOGD(TAG, "gpio %d input %x", num_pin, GPIO_INPUT_GET(num_pin));
            return GPIO_SHORT_HOLD;
        }
    } while (delay_sec > ((esp_log_early_timestamp() - tm_start) / 1000L));
    ESP_LOGD(TAG, "gpio %d input %x", num_pin, GPIO_INPUT_GET(num_pin));
    return GPIO_LONG_HOLD;
}
