#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Starts the backlight and the JD9365 panel, then returns the panel's single
 * DPI framebuffer (LCD_H_RES x LCD_V_RES RGB565), cleared to black and flushed.
 * The application draws into this buffer directly. fb_size may be NULL.
 */
esp_err_t display_init(uint16_t **fb, size_t *fb_size);

/* Writes the CPU cache back so the DPI DMA sees the latest framebuffer contents. */
void display_flush(uint16_t *fb);

/* Fills the screen with red, green, blue for 1 s each, then clears it. */
void display_selftest(uint16_t *fb);

/*
 * Copies the CROP_W x CROP_H centre crop of an RGB565 camera frame into the
 * framebuffer at row LETTERBOX_TOP. src_stride is the camera's bytes per line.
 * The letterbox bars are not touched.
 */
void display_draw_crop(const uint8_t *src, int src_stride, uint16_t *fb);
