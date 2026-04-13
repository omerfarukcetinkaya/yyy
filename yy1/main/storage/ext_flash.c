/**
 * @file ext_flash.c
 * @brief W25Q64 8 MB SPI NOR flash driver using ESP-IDF spi_master.
 *
 * Full-duplex SPI polling mode. CS managed by the driver.
 * All operations block until completion.
 */
#include "ext_flash.h"
#include "ov3660_s3_n16r8.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ext_flash";

/* W25Q64 opcodes */
#define W25_CMD_JEDEC_ID   0x9F
#define W25_CMD_READ       0x03
#define W25_CMD_PP         0x02  /* Page Program */
#define W25_CMD_SE         0x20  /* Sector Erase 4 KB */
#define W25_CMD_WREN       0x06
#define W25_CMD_RDSR1      0x05

#define W25_JEDEC_EXPECTED 0xEF4017U
#define W25_PAGE_SIZE      256
#define W25_SECTOR_SIZE    4096
#define W25_POLL_MAX_MS    500

static spi_device_handle_t s_spi = NULL;

/* ── Low-level helpers ────────────────────────────────────────────────────── */

static esp_err_t spi_send(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static uint8_t read_sr1(void)
{
    uint8_t tx[2] = {W25_CMD_RDSR1, 0xFF};
    uint8_t rx[2] = {0};
    spi_send(tx, rx, 2);
    return rx[1];
}

static esp_err_t wait_not_busy(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(W25_POLL_MAX_MS);
    while (read_sr1() & 0x01) {
        if (xTaskGetTickCount() > deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

static void wren(void)
{
    uint8_t cmd = W25_CMD_WREN;
    spi_send(&cmd, NULL, 1);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t ext_flash_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = BOARD_FLASH_MOSI_GPIO,
        .miso_io_num     = BOARD_FLASH_MISO_GPIO,
        .sclk_io_num     = BOARD_FLASH_CLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = W25_PAGE_SIZE + 4,
    };
    esp_err_t ret = spi_bus_initialize(BOARD_FLASH_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_FLASH_SPI_MHZ * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = BOARD_FLASH_CS_GPIO,
        .queue_size     = 1,
    };
    ret = spi_bus_add_device(BOARD_FLASH_SPI_HOST, &dev, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t id = 0;
    ext_flash_read_id(&id);
    if (id != W25_JEDEC_EXPECTED) {
        ESP_LOGW(TAG, "W25Q64 not found (JEDEC=0x%06lX, expected 0x%06X). Flash disabled.",
                 (unsigned long)id, W25_JEDEC_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "W25Q64 ready (JEDEC=0x%06lX).", (unsigned long)id);
    return ESP_OK;
}

esp_err_t ext_flash_read_id(uint32_t *id)
{
    if (!s_spi || !id) return ESP_ERR_INVALID_STATE;
    uint8_t tx[4] = {W25_CMD_JEDEC_ID, 0, 0, 0};
    uint8_t rx[4] = {0};
    esp_err_t ret = spi_send(tx, rx, 4);
    if (ret == ESP_OK) *id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
    return ret;
}

esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len)
{
    if (!s_spi || !buf || len == 0) return ESP_ERR_INVALID_STATE;

    /* Allocate combined TX/RX buffer (4-byte cmd+addr prefix + data) */
    size_t total = 4 + len;
    uint8_t *tx = calloc(total, 1);
    uint8_t *rx = calloc(total, 1);
    if (!tx || !rx) { free(tx); free(rx); return ESP_ERR_NO_MEM; }

    tx[0] = W25_CMD_READ;
    tx[1] = (addr >> 16) & 0xFF;
    tx[2] = (addr >>  8) & 0xFF;
    tx[3] = (addr      ) & 0xFF;

    esp_err_t ret = spi_send(tx, rx, total);
    if (ret == ESP_OK) memcpy(buf, rx + 4, len);

    free(tx);
    free(rx);
    return ret;
}

esp_err_t ext_flash_write_page(uint32_t addr, const void *data, size_t len)
{
    if (!s_spi || !data || len == 0 || len > W25_PAGE_SIZE) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = wait_not_busy();
    if (ret != ESP_OK) return ret;
    wren();

    size_t total = 4 + len;
    uint8_t *tx = calloc(total, 1);
    if (!tx) return ESP_ERR_NO_MEM;

    tx[0] = W25_CMD_PP;
    tx[1] = (addr >> 16) & 0xFF;
    tx[2] = (addr >>  8) & 0xFF;
    tx[3] = (addr      ) & 0xFF;
    memcpy(tx + 4, data, len);

    ret = spi_send(tx, NULL, total);
    free(tx);
    if (ret != ESP_OK) return ret;

    return wait_not_busy();
}

esp_err_t ext_flash_erase_sector(uint32_t addr)
{
    if (!s_spi) return ESP_ERR_INVALID_STATE;
    if (addr % W25_SECTOR_SIZE != 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = wait_not_busy();
    if (ret != ESP_OK) return ret;
    wren();

    uint8_t cmd[4] = {
        W25_CMD_SE,
        (addr >> 16) & 0xFF,
        (addr >>  8) & 0xFF,
        (addr      ) & 0xFF,
    };
    ret = spi_send(cmd, NULL, 4);
    if (ret != ESP_OK) return ret;

    return wait_not_busy();
}
