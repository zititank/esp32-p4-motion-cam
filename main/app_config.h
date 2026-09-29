/*
 * Application-level layout and build switches.
 */
#pragma once

#include "board_jc8012p4a1.h"

/* Full-height centre crop of the landscape camera frame, letterboxed into the
 * portrait panel with black bars top and bottom. There is no rotation. */
#define CROP_W              800
#define CROP_H              1080
#define CROP_SRC_X          ((CAM_WIDTH - CROP_W) / 2)   /* 560 */
#define CROP_SRC_Y          0
#define LETTERBOX_TOP       ((LCD_V_RES - CROP_H) / 2)   /* 100 */

/* Diagnostic software gain on the crop copy. 1 = off. Values above 1 only make
 * an uncalibrated image visible and clip highlights; leave at 1 once the ISP
 * tuning is loaded. */
#define APP_SW_GAIN         1

/* 1 = show solid red, green, blue for 1 s each at boot. */
#define LCD_SELFTEST        0

/* 1 = re-send a manual DSI wake-up sequence after the JD9365 driver init.
 * Normally unnecessary and can conflict with the driver. */
#define LCD_MANUAL_DSI_SEQ  0

/* Additions on top of the Aug 2026 bring-up frame path. Setting both to 0
 * gives exactly the original loop: DQBUF, draw_crop, msync, QBUF. */

/* 1 = log PSRAM / internal heap every APP_HEAP_REPORT_EVERY frames. */
#define APP_HEAP_REPORT         1
#define APP_HEAP_REPORT_EVERY   300

/* 1 = run motion detection and draw the overlay into the framebuffer. */
#define APP_MOTION_ENABLE       1
