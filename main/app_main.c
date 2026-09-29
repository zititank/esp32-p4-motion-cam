/*
 * OV02C10 (MIPI-CSI) -> ESP32-P4 ISP -> V4L2 RGB565 -> JD9365 (MIPI-DSI) panel.
 * Each frame is centre-cropped to CROP_W x CROP_H and written straight into
 * the panel framebuffer.
 */

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "app_config.h"
#include "camera.h"
#include "diagnostics.h"
#include "display.h"

static const char *TAG = "app";

#if APP_MOTION_ENABLE
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "motion.h"
#include "overlay.h"

_Static_assert(CROP_W == LCD_H_RES, "overlay view assumes the crop spans the full panel width");

/* Internal RAM that must stay free after taking the motion buffer; otherwise it goes to PSRAM. */
#define MOTION_INTERNAL_HEADROOM (64 * 1024)
#define MOTION_LOG_EVERY         60
/* Average motion + overlay time above which motion switches itself off to protect the frame rate. */
#define MOTION_BUDGET_US         15000
#define MOTION_LINE_PX           3
#define MOTION_CROSS_HALF        16
#define MOTION_INDICATOR_PX      24
#define MOTION_INDICATOR_MARGIN  16

static motion_ctx_t s_motion;
static overlay_trail_t s_trail;
static bool s_motion_on;
static bool s_indicator_on;
static int64_t s_motion_us_acc;
static int s_motion_frames;
static int64_t s_motion_t_last;

static void motion_indicator(uint16_t *fb, bool on)
{
    if (on == s_indicator_on) return;
    /* Lives in the top letterbox bar, which the crop copy never rewrites. */
    overlay_fill_rect(fb, LCD_H_RES, LETTERBOX_TOP, MOTION_INDICATOR_MARGIN,
                      MOTION_INDICATOR_MARGIN, MOTION_INDICATOR_MARGIN + MOTION_INDICATOR_PX - 1,
                      MOTION_INDICATOR_MARGIN + MOTION_INDICATOR_PX - 1,
                      on ? OVERLAY_GREEN : OVERLAY_BLACK);
    s_indicator_on = on;
}

static void motion_setup(int stride)
{
    if (stride < (CROP_SRC_X + CROP_W) * (int)sizeof(uint16_t)) {
        ESP_LOGE(TAG, "motion disabled: stride %d too small for the crop", stride);
        return;
    }
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    size_t len = motion_buffer_size(CROP_W, CROP_H, cfg.cell);
    if (len == 0) {
        ESP_LOGE(TAG, "motion disabled: invalid grid for %dx%d", CROP_W, CROP_H);
        return;
    }

    const char *where = "internal RAM";
    void *buf = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MOTION_INTERNAL_HEADROOM) {
        heap_caps_free(buf);
        buf = NULL;
    }
    if (!buf) {
        buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        where = "PSRAM";
    }
    if (!buf) {
        ESP_LOGE(TAG, "motion disabled: cannot allocate %u B work buffer", (unsigned)len);
        return;
    }

    int rc = motion_init(&s_motion, &cfg, CROP_W, CROP_H, buf, len);
    if (rc != 0) {
        ESP_LOGE(TAG, "motion disabled: motion_init returned %d", rc);
        heap_caps_free(buf);
        return;
    }
    overlay_trail_clear(&s_trail);
    s_motion_on = true;
    s_motion_t_last = esp_timer_get_time();
    ESP_LOGI(TAG, "motion: %dx%d grid, cell %d, %u B work buffer in %s", motion_grid_w(&s_motion),
             motion_grid_h(&s_motion), cfg.cell, (unsigned)len, where);
}

static void motion_frame(const uint8_t *frame, int stride, uint16_t *fb)
{
    if (!s_motion_on) return;
    int64_t t0 = esp_timer_get_time();

    motion_result_t res;
    motion_process(&s_motion, (const uint16_t *)frame, stride, CROP_SRC_X, CROP_SRC_Y, &res);

    /* The crop sits at x 0, row LETTERBOX_TOP of the panel, so this view takes crop
     * coordinates as-is (fb y = crop y + LETTERBOX_TOP) and clips drawing to the area
     * that the next crop copy overwrites. */
    uint16_t *view = fb + (size_t)LETTERBOX_TOP * LCD_H_RES;
    if (res.active) {
        overlay_trail_push(&s_trail, res.cx, res.cy);
        overlay_trail_draw(&s_trail, view, LCD_H_RES, CROP_H, OVERLAY_GREEN, MOTION_LINE_PX);
        overlay_rect(view, LCD_H_RES, CROP_H, res.x0, res.y0, res.x1, res.y1, OVERLAY_GREEN,
                     MOTION_LINE_PX);
        overlay_crosshair(view, LCD_H_RES, CROP_H, res.cx, res.cy, MOTION_CROSS_HALF,
                          OVERLAY_GREEN, MOTION_LINE_PX);
    } else {
        overlay_trail_clear(&s_trail);
    }
    motion_indicator(fb, res.active);

    int64_t t1 = esp_timer_get_time();
    s_motion_us_acc += t1 - t0;
    if (++s_motion_frames >= MOTION_LOG_EVERY) {
        int avg_us = (int)(s_motion_us_acc / s_motion_frames);
        int64_t span = t1 - s_motion_t_last;
        int fps_x10 = span > 0 ? (int)((int64_t)s_motion_frames * 10000000 / span) : 0;
        ESP_LOGI(TAG, "motion: avg %d us/frame, %d.%d FPS, active=%d cells=%d", avg_us,
                 fps_x10 / 10, fps_x10 % 10, res.active, res.changed_cells);
        s_motion_us_acc = 0;
        s_motion_frames = 0;
        s_motion_t_last = t1;
        if (avg_us > MOTION_BUDGET_US) {
            ESP_LOGW(TAG, "motion disabled: %d us/frame exceeds the %d us budget", avg_us,
                     MOTION_BUDGET_US);
            motion_indicator(fb, false);
            s_motion_on = false;
        }
    }
}
#endif /* APP_MOTION_ENABLE */

void app_main(void)
{
    ESP_LOGI(TAG, "========== CAM+LCD START ==========");

    uint16_t *lcd_fb = NULL;
    ESP_ERROR_CHECK(display_init(&lcd_fb, NULL));

#if LCD_SELFTEST
    display_selftest(lcd_fb);
#endif

    camera_start_xclk();
    diag_i2c_scan();

    ESP_ERROR_CHECK(camera_init());
    bool ipa_alive = diag_camera(camera_fd());
    if (ipa_alive) {
        camera_neutralize_controls();
    }

    if (!ipa_alive) {
        ESP_LOGW(TAG, "ISP is uncalibrated: expect dark corners and a grey cast.");
        ESP_LOGW(TAG, "APP_SW_GAIN=%d (1=off).", APP_SW_GAIN);
    }

    ESP_LOGI(TAG, "Streaming: %dx%d center-crop -> %dx%d + black letterbox",
             CROP_W, CROP_H, LCD_H_RES, LCD_V_RES);

    const int stride = camera_stride();
#if APP_MOTION_ENABLE
    motion_setup(stride);
#endif
    int frame_count = 0;
    while (1) {
        camera_frame_t frame;
        if (camera_dequeue(&frame) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (frame.buf.bytesused &&
            frame.buf.bytesused < (unsigned)(CROP_SRC_Y + CROP_H) * stride) {
            ESP_LOGW(TAG, "short frame: bytesused=%u - skipping", (unsigned)frame.buf.bytesused);
            camera_requeue(&frame);
            continue;
        }

        if ((frame_count % 60) == 0) {
            const uint16_t *cam = (const uint16_t *)frame.data;
            ESP_LOGI(TAG, "frame %d: %04X %04X %04X %04X", frame_count,
                     cam[0], cam[1], cam[CAM_WIDTH], cam[CAM_WIDTH + 1]);
        }
#if APP_HEAP_REPORT
        if ((frame_count % APP_HEAP_REPORT_EVERY) == 0) {
            diag_heap_report();
        }
#endif
        frame_count++;

        display_draw_crop(frame.data, stride, lcd_fb);

#if APP_MOTION_ENABLE
        motion_frame(frame.data, stride, lcd_fb);
#endif

        display_flush(lcd_fb);

        camera_requeue(&frame);
    }
}
