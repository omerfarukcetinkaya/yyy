/**
 * @file ext_flash.h
 * @brief W25Q64 SPI NOR flash driver (8 MB, SPI2/HSPI).
 *
 * W25Q64 key specs:
 *   - 64 Mb = 8 MB total
 *   - Page: 256 bytes, Sector: 4 KB, Block: 64 KB
 *   - JEDEC ID: 0xEF4017
 *   - Max clock: 104 MHz (we use 10 MHz for safety)
 *
 * All operations are synchronous and use SPI polling mode.
 * DMA is not needed at this size; polling is simpler and more reliable.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize SPI bus and W25Q64 device.
 * Verifies JEDEC ID. Returns ESP_ERR_NOT_FOUND if device not detected.
 */
esp_err_t ext_flash_init(void);

/**
 * @brief Read bytes from flash.
 * @param addr   24-bit byte address.
 * @param buf    Output buffer.
 * @param len    Number of bytes to read.
 */
esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len);

/**
 * @brief Write a page (up to 256 bytes). Address must be page-aligned.
 * Automatically issues WREN before write and polls WIP bit until done.
 */
esp_err_t ext_flash_write_page(uint32_t addr, const void *buf, size_t len);

/**
 * @brief Erase a 4 KB sector. Address must be sector-aligned (4096 bytes).
 * Blocks until erase completes (~100 ms typical).
 */
esp_err_t ext_flash_erase_sector(uint32_t addr);

/**
 * @brief Read the 3-byte JEDEC ID.
 * Expected: 0xEF4017 for W25Q64.
 */
esp_err_t ext_flash_read_id(uint32_t *id);
