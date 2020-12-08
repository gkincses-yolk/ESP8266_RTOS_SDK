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

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_image_format.h"
#include "esp_flash_partitions.h"
#include "bootloader_flash.h"

#include "esp8266/uart_register.h"
#include "esp8266/eagle_soc.h"
#include "esp8266/gpio_register.h"
#include "esp8266/pin_mux_register.h"
#include "esp8266/rom_functions.h"

#define BOOTLOADER_CONSOLE_CLK_FREQ 52 * 1000 * 1000

extern int _bss_start;
extern int _bss_end;
extern int _data_start;
extern int _data_end;

static const char* TAG = "boot";

static esp_err_t bootloader_main();
static void print_flash_info(const esp_image_header_t* pfhdr);
static void update_flash_config(const esp_image_header_t* pfhdr);

static void uart_console_configure(void)
{
    ESP_LOGF("FUNC", "uart_console_configure");

#if CONFIG_ESP_UART0_SWAP_IO
    while (READ_PERI_REG(UART_STATUS(0)) & (UART_TXFIFO_CNT << UART_TXFIFO_CNT_S));

    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_UART0_CTS);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_UART0_RTS);

    // UART0: TXD <-> RTS and RXD <-> CTS
    SET_PERI_REG_MASK(UART_SWAP_REG, 0x4);
#endif

#if CONFIG_ESP_CONSOLE_UART_NUM == 1
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);

    CLEAR_PERI_REG_MASK(UART_CONF1(CONFIG_ESP_CONSOLE_UART_NUM), UART_RX_FLOW_EN);
    CLEAR_PERI_REG_MASK(UART_CONF0(CONFIG_ESP_CONSOLE_UART_NUM), UART_TX_FLOW_EN);

    WRITE_PERI_REG(UART_CONF0(CONFIG_ESP_CONSOLE_UART_NUM),
                   0                // None parity
                   | (1 << 4)       // 1-bit stop
                   | (3 << 2)       // 8-bit data
                   | 0              // None flow control
                   | 0);            // None Inverse

    SET_PERI_REG_MASK(UART_CONF0(CONFIG_ESP_CONSOLE_UART_NUM), UART_RXFIFO_RST | UART_TXFIFO_RST);
    CLEAR_PERI_REG_MASK(UART_CONF0(CONFIG_ESP_CONSOLE_UART_NUM), UART_RXFIFO_RST | UART_TXFIFO_RST);
#endif

#ifdef CONFIG_ESP_CONSOLE_UART_BAUDRATE
    uart_div_modify(CONFIG_ESP_CONSOLE_UART_NUM, BOOTLOADER_CONSOLE_CLK_FREQ / CONFIG_ESP_CONSOLE_UART_BAUDRATE);
#endif
}

esp_err_t bootloader_init()
{
    ESP_LOGF("FUNC", "bootloader_init");

    //Clear bss
    memset(&_bss_start, 0, (&_bss_end - &_bss_start) * sizeof(_bss_start));

    if(bootloader_main() != ESP_OK){
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t bootloader_main()
{
    ESP_LOGF("FUNC", "bootloader_main");

#ifdef CONFIG_BOOTLOADER_DISABLE_JTAG_IO
    /* Set GPIO 12-15 to be normal GPIO  */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);

    /* Set GPIO 12-15 to be input mode  */
    GPIO_REG_WRITE(GPIO_ENABLE_W1TC_ADDRESS, BIT12 | BIT13 | BIT14 | BIT15);
#endif

    uart_console_configure();

    esp_image_header_t fhdr;
    if (bootloader_flash_read(ESP_BOOTLOADER_OFFSET, &fhdr, sizeof(esp_image_header_t), true) != ESP_OK) {
        ESP_LOGE(TAG, "failed to load bootloader header!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ESP-IDF %s 2nd stage bootloader", IDF_VER);

    ESP_LOGI(TAG, "compile time " __TIME__ );

#if defined(CONFIG_ESPTOOLPY_FLASHMODE_QIO) || defined(CONFIG_ESPTOOLPY_FLASHMODE_QOUT)
    fhdr.spi_mode = CONFIG_SPI_FLASH_MODE;
#endif

    extern void phy_reg_default(void);
    phy_reg_default();

    update_flash_config(&fhdr);

    print_flash_info(&fhdr);

    return ESP_OK;
}

static void update_flash_config(const esp_image_header_t* pfhdr)
{
    ESP_LOGF("FUNC", "update_flash_config");

#ifdef CONFIG_BOOTLOADER_INIT_SPI_FLASH
    extern void esp_spi_flash_init(uint32_t spi_speed, uint32_t spi_mode);

    esp_spi_flash_init(pfhdr->spi_speed, pfhdr->spi_mode);

    ESP_LOGD(TAG, "bootloader initialize SPI flash clock and I/O");
#endif /* CONFIG_BOOTLOADER_INIT_SPI_FLASH */

    Cache_Read_Disable();
}

static void print_flash_info(const esp_image_header_t* phdr)
{
    ESP_LOGF("FUNC", "print_flash_info");

#if (BOOT_LOG_LEVEL >= BOOT_LOG_LEVEL_NOTICE)

    ESP_LOGD(TAG, "magic %02x", phdr->magic );
    ESP_LOGD(TAG, "segments %02x", phdr->segment_count );
    ESP_LOGD(TAG, "spi_mode %02x", phdr->spi_mode );
    ESP_LOGD(TAG, "spi_speed %02x", phdr->spi_speed );
    ESP_LOGD(TAG, "spi_size %02x", phdr->spi_size );

    const char* str;
    switch ( phdr->spi_speed ) {
    case ESP_IMAGE_SPI_SPEED_40M:
        str = "40MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_26M:
        str = "26.7MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_20M:
        str = "20MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_80M:
        str = "80MHz";
        break;
    default:
        str = "20MHz";
        break;
    }
    ESP_LOGI(TAG, "SPI Speed      : %s", str );

    switch ( phdr->spi_mode ) {
    case ESP_IMAGE_SPI_MODE_QIO:
        str = "QIO";
        break;
    case ESP_IMAGE_SPI_MODE_QOUT:
        str = "QOUT";
        break;
    case ESP_IMAGE_SPI_MODE_DIO:
        str = "DIO";
        break;
    case ESP_IMAGE_SPI_MODE_DOUT:
        str = "DOUT";
        break;
    default:
        str = "QIO";
        break;
    }
    ESP_LOGI(TAG, "SPI Mode       : %s", str );

    switch ( phdr->spi_size ) {
    case ESP_IMAGE_FLASH_SIZE_1MB:
        str = "1MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_2MB:
    case ESP_IMAGE_FLASH_SIZE_2MB_C1:
        str = "2MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_4MB:
    case ESP_IMAGE_FLASH_SIZE_4MB_C1:
        str = "4MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_8MB:
        str = "8MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_16MB:
        str = "16MB";
        break;
    default:
        str = "2MB";
        break;
    }
    ESP_LOGI(TAG, "SPI Flash Size : %s", str );
#endif
}

