/**
 * @file ov3660_s3_n16r8.h
 * @brief Pin map for ESP32-S3-WROOM N16R8 + OV3660 camera board.
 *
 * !! IMPORTANT !!
 * This mapping follows the Espressif ESP32-S3-EYE reference schematic.
 * VERIFY every pin against your board's actual schematic before flashing.
 * Incorrect XCLK or SIOC/SIOD will prevent camera init.
 * Incorrect data pins will produce corrupted frames silently.
 *
 * Board: ESP32-S3-WROOM-1 N16R8
 *   - Flash:  16 MB (QSPI)
 *   - PSRAM:  8 MB (Octal OPI)
 *   - Camera: OV3660
 *   - USB-UART: CH343P (CDC ACM, /dev/ttyACM0)
 *   - PCB: V1695 127294G_Y1127_231016
 */
#pragma once

/* ── Camera clock ─────────────────────────────────────────────────────────── */
#define BOARD_CAM_XCLK_GPIO    15
#define BOARD_CAM_XCLK_MHZ     20

/* ── SCCB (I2C-like control bus for OV3660 only) ─────────────────────────── */
#define BOARD_CAM_SIOD_GPIO     4   /* SDA */
#define BOARD_CAM_SIOC_GPIO     5   /* SCL */

/* ── Parallel data bus (D0-D7) ────────────────────────────────────────────── */
#define BOARD_CAM_D0_GPIO      11
#define BOARD_CAM_D1_GPIO       9
#define BOARD_CAM_D2_GPIO       8
#define BOARD_CAM_D3_GPIO      10
#define BOARD_CAM_D4_GPIO      12
#define BOARD_CAM_D5_GPIO      18
#define BOARD_CAM_D6_GPIO      17
#define BOARD_CAM_D7_GPIO      16

/* ── Sync signals ─────────────────────────────────────────────────────────── */
#define BOARD_CAM_VSYNC_GPIO    6
#define BOARD_CAM_HREF_GPIO     7
#define BOARD_CAM_PCLK_GPIO    13

/* ── Power / reset ────────────────────────────────────────────────────────── */
#define BOARD_CAM_PWDN_GPIO    -1   /* not used / unknown */
#define BOARD_CAM_RESET_GPIO   -1   /* not used */

/* ── On-board LED (active low on ESP32-S3-EYE) ───────────────────────────── */
#define BOARD_LED_GPIO          2

/* ── Shared sensor I2C bus (AHT20 / BMP280 / VEML7700 / SSD1306) ─────────── */
#define BOARD_SENSOR_I2C_PORT   0           /* I2C_NUM_0 */
#define BOARD_SENSOR_SDA_GPIO  21
#define BOARD_SENSOR_SCL_GPIO  47
#define BOARD_SENSOR_I2C_FREQ  400000       /* 400 kHz fast mode */

/* I2C device addresses */
#define BOARD_AHT20_I2C_ADDR   0x38
#define BOARD_BMP280_I2C_ADDR  0x76        /* SDO=GND */
#define BOARD_VEML7700_I2C_ADDR 0x10
#define BOARD_SSD1306_I2C_ADDR 0x3C

/* ── MQ7 CO sensor (ADC1) ─────────────────────────────────────────────────── */
#define BOARD_MQ7_ADC_CHANNEL   ADC_CHANNEL_0   /* GPIO1, ADC1_CH0 */
#define BOARD_MQ7_ADC_UNIT      ADC_UNIT_1
#define BOARD_MQ7_DO_GPIO      38              /* Digital threshold output */

/* ── MQ137 gas sensor (ADC1) ─────────────────────────────────────────────── */
#define BOARD_MQ137_ADC_CHANNEL ADC_CHANNEL_1  /* GPIO2, ADC1_CH1 */
#define BOARD_MQ137_ADC_UNIT    ADC_UNIT_1
#define BOARD_MQ137_DO_GPIO    14              /* Digital threshold output */

/* ── W25Q64 SPI NOR flash (SPI2 / HSPI) ──────────────────────────────────── */
#define BOARD_FLASH_SPI_HOST    SPI2_HOST
#define BOARD_FLASH_CS_GPIO    42
#define BOARD_FLASH_MOSI_GPIO  40
#define BOARD_FLASH_MISO_GPIO  39
#define BOARD_FLASH_CLK_GPIO   41
#define BOARD_FLASH_SPI_MHZ    10             /* Conservative: 10 MHz */
