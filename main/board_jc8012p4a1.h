/*
 * Board constants for the Guition JC8012P4A1C (ESP32-P4, OV02C10, JD9365 800x1280).
 * Other ESP32-P4 boards differ in exactly these values; see docs/PORTING.md.
 */
#pragma once

#include "driver/ledc.h"

/* ---- LCD: JD9365, 800x1280 portrait, RGB565 over 2-lane MIPI-DSI ---- */
#define LCD_H_RES              800
#define LCD_V_RES              1280
#define LCD_BITS_PER_PIXEL     16
#define LCD_COLOR_FORMAT       LCD_COLOR_PIXEL_FORMAT_RGB565
#define LCD_RESET_GPIO         -1
#define LCD_DPI_CLOCK_MHZ      20
#define LCD_DSI_BUS_CONFIG()   JD9365_PANEL_BUS_DSI_2CH_CONFIG()
#define LCD_DPI_CONFIG(fmt)    JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(fmt)

/* The DSI PHY is powered from an on-chip LDO channel. */
#define MIPI_LDO_CHAN          3
#define MIPI_LDO_VOLTAGE_MV    2500

/* ---- Backlight (LEDC PWM) ---- */
#define BKLT_GPIO              23
#define BKLT_TIMER             LEDC_TIMER_1
#define BKLT_CHANNEL           LEDC_CHANNEL_0

/* ---- Camera: OV02C10 on 2-lane MIPI-CSI ---- */
/* XCLK is generated with LEDC and must match the .xclk field of the driver's format table. */
#define XCLK_GPIO              6
#define XCLK_FREQ_HZ           19200000
#define XCLK_TIMER             LEDC_TIMER_2
#define XCLK_CHANNEL           LEDC_CHANNEL_1

#define SCCB_I2C_PORT          0
#define SCCB_SDA_GPIO          7
#define SCCB_SCL_GPIO          8
#define SCCB_FREQ_HZ           100000
#define CAM_SCCB_ADDR          0x36
#define CAM_RESET_GPIO         -1
#define CAM_PWDN_GPIO          -1

#define CAM_WIDTH              1920
#define CAM_HEIGHT             1080
#define NUM_V4L2_BUFFERS       4
