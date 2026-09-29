/*
 * OV02C10 MIPI-CSI  -->  ESP32-P4 ISP  -->  V4L2 RGB565  -->  JD9365 MIPI-DSI LCD
 * ESP-IDF 5.4 | JC8012P4A1C_I_W_Y1
 *
 * ------------------------------ ARCHITECTURE ------------------------------
 *
 *  OV02C10 (RAW10 BGGR, 1920x1080)
 *     |  MIPI-CSI-2 lanes
 *     v
 *  MIPI-CSI controller ---------------> /dev/video0 (RAW formats available here)
 *     |  (RAW Bayer frames)
 *     v
 *  ESP32-P4 ISP  <---- esp_ipa pipeline, calibrated by a per-sensor JSON
 *     |                 (sensors/<name>/cfg/<name>_default.json, matched by
 *     v                  the sensor's REGISTERED NAME string)
 *  /dev/videoX V4L2 capture node, RGB565 1920x1080, MMAP buffers x4
 *     |  DQBUF -> center-crop 800x1080 -> letterbox into 800x1280
 *     v
 *  JD9365 MIPI-DSI panel framebuffer (drawn in place, no extra copy)
 *
 * ------------------------------ ROOT CAUSE --------------------------------
 *
 *  The "black vignette + grey bars + dirty desaturated center" is NOT caused
 *  by the x16 software boost. The boost only made an uncalibrated image
 *  visible. Boot-log proof:
 *    - "esp_video_init: failed to get configuration to initialize ISP controller"
 *    - VIDIOC_QUERYCTRL enumerates ZERO controls (no AE/AGC/AWB at all)
 *    - active format reports bytesperline=0 / sizeimage=0
 *  OV02C10 outputs RAW Bayer. With no IPA JSON, the ISP runs with no AE/AGC
 *  (too dark), no LSC (vignette), no AWB/CCM (grey/desaturated), no gamma.
 *  FIX (outside this file, in the esp_cam_sensor tree):
 *    1. Put ov02c10_default.json into  sensors/ov02c10/cfg/
 *    2. Make sure the ov02c10 CMakeLists EMBED_FILES embeds it.
 *    3. The JSON's top-level key MUST equal the name string the modified
 *       driver registers in its esp_cam_sensor_device_t (e.g. "OV02C10").
 *    4. Best starting point: copy sensors/ov5647/cfg/ov5647_default.json
 *       (a real calibrated 1080p-class RAW sensor) and rename the key,
 *       because the AGC/AE section schema differs between esp_ipa versions
 *       and is not fully documented. The JSON shipped next to this file is
 *       a documented-schema skeleton with flat (identity) LSC tables.
 *
 *  This application is written so that it VERIFIES the fix at runtime:
 *  after ISP repair you will see AE/AWB/gain controls enumerated and a
 *  correctly exposed, white-balanced image with NO software gain applied.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_jd9365.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

static const char *TAG = "CAM_LCD";

/* ------------------------------- tuning --------------------------------- */

#define LCD_H_RES           800
#define LCD_V_RES           1280
#define LCD_BITS_PER_PIXEL  16
#define LCD_COLOR_FORMAT    LCD_COLOR_PIXEL_FORMAT_RGB565

#define CAM_WIDTH           1920
#define CAM_HEIGHT          1080
#define NUM_V4L2_BUFFERS    4

/* Full-height center crop: 800x1080 out of 1920x1080, letterboxed into the
 * 800x1280 portrait panel with BLACK bars (140 px top/bottom). The old code
 * cropped 800x600 and filled the rest of the screen with 0x1010 grey - that
 * grey fill was itself one of the "bars of grey" you saw. */
#define CROP_W              800
#define CROP_H              1080
#define CROP_SRC_X          ((CAM_WIDTH - CROP_W) / 2)   /* 560 */
#define CROP_SRC_Y          0
#define LETTERBOX_TOP       ((LCD_V_RES - CROP_H) / 2)   /* 140 */

/* Diagnostic-only software gain. 1 = OFF (correct value once the ISP/IPA
 * pipeline is actually calibrated). Anything >1 is a symptom-masker; it
 * saturates noise and is why highlights clipped into flat grey bars. */
#define APP_SW_GAIN         1

/* Set to 1 to run the RGB solid-color LCD selftest at boot (3 x 1 s). */
#define LCD_SELFTEST        0

/* Set to 1 to re-send the manual DSI wake-up sequence after the JD9365
 * driver's own init. Normally unnecessary and can fight the driver. */
#define LCD_MANUAL_DSI_SEQ  0

#define BKLT_GPIO           23
#define BKLT_TIMER          LEDC_TIMER_1
#define BKLT_CHANNEL        LEDC_CHANNEL_0

#define MIPI_LDO_CHAN       3
#define MIPI_LDO_VOLTAGE_MV 2500

/* ------------------------------- globals -------------------------------- */

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_mipi_io = NULL;
static int      s_video_fd = -1;
static uint8_t *s_v4l2_bufs[NUM_V4L2_BUFFERS];
static size_t   s_v4l2_buf_len = 0;
static int      s_stride = 0;            /* real bytes-per-line after S_FMT */
static bool     s_ipa_alive = false;     /* true once ISP controls enumerate */

/* ------------------------------ backlight ------------------------------- */

static void init_backlight(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .timer_num        = BKLT_TIMER,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num       = BKLT_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = BKLT_CHANNEL,
        .timer_sel      = BKLT_TIMER,
        .duty           = 1023,
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

/* -------------------------------- XCLK ---------------------------------- */

static void init_xclk(void)
{
    /* 19.2 MHz on GPIO6. NOTE: if your modified ov02c10 driver uses a
     * different input clock (many OmniVision modules expect 24 MHz), the
     * MIPI lane timing derived from it will be off - check the driver's
     * esp_cam_sensor_format_t .xclk field against this value. */
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num       = LEDC_TIMER_2,
        .freq_hz         = 19200000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num       = 6,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_1,
        .timer_sel      = LEDC_TIMER_2,
        .duty           = 1,
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_LOGI(TAG, "XCLK 19.2 MHz started on GPIO6");
}

/* --------------------------------- LCD ---------------------------------- */

static esp_err_t init_lcd(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_LDO_CHAN,
        .voltage_mv = MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_mipi_io));

    esp_lcd_dpi_panel_config_t dpi_config = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FORMAT);
    dpi_config.num_fbs = 1;
    dpi_config.dpi_clock_freq_mhz = 20;

    jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(s_mipi_io, &panel_cfg, &s_lcd_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));

#if LCD_MANUAL_DSI_SEQ
    ESP_LOGI(TAG, "Sending manual DSI wake-up sequence...");
    esp_lcd_panel_io_tx_param(s_mipi_io, 0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_io_tx_param(s_mipi_io, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_io_tx_param(s_mipi_io, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t inv_on = 0x00;
    esp_lcd_panel_io_tx_param(s_mipi_io, 0x21, &inv_on, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd_panel, true));
    ESP_LOGI(TAG, "LCD init OK: %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

/* --------------------------- early I2C scan ------------------------------ */
/* Runs BEFORE esp_video_init claims the bus. OV02C10 default SCCB address
 * is 0x36 (7-bit). */

static void early_i2c_bus_scan(void)
{
    ESP_LOGI(TAG, "==================== I2C/SCCB BUS SCAN (SDA=7 SCL=8) ====================");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                    = -1,
        .sda_io_num                  = GPIO_NUM_7,
        .scl_io_num                  = GPIO_NUM_8,
        .clk_source                  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt           = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "bus scan: could not create temp I2C bus (%s) - skipping", esp_err_to_name(ret));
        return;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  found ACK at 0x%02X%s", addr, (addr == 0x36) ? "  <-- expected OV02C10 addr" : "");
            found++;
        }
    }
    ESP_LOGI(TAG, "==================== scan done: %d device(s) found ====================", found);
    i2c_del_master_bus(bus);
}

/* ------------------------- V4L2 / ISP diagnostics ------------------------- */

static void diagnose_camera_hardware(int fd)
{
    ESP_LOGI(TAG, "==================== CAMERA / V4L2 DIAGNOSTICS ====================");

    struct v4l2_capability cap = {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "driver   : %s", cap.driver);
        ESP_LOGI(TAG, "card     : %s", cap.card);
        ESP_LOGI(TAG, "bus_info : %s", cap.bus_info);
        ESP_LOGI(TAG, "version  : %u.%u.%u",
                 (unsigned)(cap.version >> 16) & 0xFF,
                 (unsigned)(cap.version >> 8) & 0xFF,
                 (unsigned)cap.version & 0xFF);
    }

    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc fdesc = {};
        fdesc.index = i;
        fdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fdesc) != 0) break;

        char fourcc[5] = {
            (char)(fdesc.pixelformat >> 0), (char)(fdesc.pixelformat >> 8),
            (char)(fdesc.pixelformat >> 16), (char)(fdesc.pixelformat >> 24), 0
        };
        ESP_LOGI(TAG, "  fmt[%d] %-24s '%s'", i, fdesc.description, fourcc);
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
        ESP_LOGI(TAG, "active: %ux%u bytesperline=%u sizeimage=%u",
                 (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
                 (unsigned)fmt.fmt.pix.bytesperline, (unsigned)fmt.fmt.pix.sizeimage);
    }

    /* The decisive check: if the IPA pipeline loaded its JSON, AE/AWB/gain
     * style controls exist. If this loop finds nothing, the ISP is running
     * uncalibrated and NO application-level gain hack will fix the image. */
    ESP_LOGI(TAG, "-- V4L2 controls (AE/AWB/gain here => ISP/IPA alive) --");
    struct v4l2_queryctrl qc = {};
    qc.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    int ctrl_count = 0;
    while (ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
        ESP_LOGI(TAG, "  id=0x%08X '%s' range=[%d..%d] default=%d",
                 (unsigned)qc.id, qc.name,
                 (int)qc.minimum, (int)qc.maximum, (int)qc.default_value);
        ctrl_count++;
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    s_ipa_alive = (ctrl_count > 0);
    if (!s_ipa_alive) {
        ESP_LOGE(TAG, "  ZERO controls => esp_ipa found no JSON for this sensor.");
        ESP_LOGE(TAG, "  Fix: sensors/ov02c10/cfg/ov02c10_default.json, embedded via");
        ESP_LOGE(TAG, "  EMBED_FILES, top-level key == the driver's registered name.");
    }

    ESP_LOGI(TAG, "=====================================================================");
}

/* If controls exist (post-fix), leave AE/AWB on auto; this only nudges the
 * brightness control to neutral so nothing inherits a stale offset. */
static void neutralize_controls_if_present(int fd)
{
    if (!s_ipa_alive) return;
    struct v4l2_control c = {};
    c.id = V4L2_CID_BRIGHTNESS;
    c.value = 0;
    if (ioctl(fd, VIDIOC_S_CTRL, &c) == 0) {
        ESP_LOGI(TAG, "V4L2_CID_BRIGHTNESS -> 0");
    }
}

/* -------------------------------- camera -------------------------------- */

static esp_err_t init_camera(void)
{
    esp_video_init_csi_config_t csi_cfg = {};
    csi_cfg.sccb_config.init_sccb = true;
    csi_cfg.sccb_config.i2c_config.port = 0;
    csi_cfg.sccb_config.i2c_config.scl_pin = GPIO_NUM_8;
    csi_cfg.sccb_config.i2c_config.sda_pin = GPIO_NUM_7;
    csi_cfg.sccb_config.freq = 100000;
    csi_cfg.reset_pin = (gpio_num_t)(-1);
    csi_cfg.pwdn_pin  = (gpio_num_t)(-1);

    esp_video_init_config_t vid_cfg = { .csi = &csi_cfg };

    esp_err_t ret = esp_video_init(&vid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "esp_video_init OK");

    /* Pick the first video node that actually offers RGB565 capture instead
     * of blindly opening video0. */
    const char *paths[] = {"/dev/video0", "/dev/video1", "/dev/video2"};
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK, 0);
        if (fd < 0) continue;
        bool has_rgb565 = false;
        for (int k = 0; ; k++) {
            struct v4l2_fmtdesc fdesc = {};
            fdesc.index = k;
            fdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(fd, VIDIOC_ENUM_FMT, &fdesc) != 0) break;
            if (fdesc.pixelformat == V4L2_PIX_FMT_RGB565) { has_rgb565 = true; break; }
        }
        if (has_rgb565) {
            s_video_fd = fd;
            ESP_LOGI(TAG, "V4L2 device: %s (fd=%d)", paths[i], s_video_fd);
            break;
        }
        close(fd);
    }
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "No /dev/videoX with RGB565 found!");
        return ESP_FAIL;
    }

    /* Explicit, complete S_FMT. The old code inherited G_FMT then only
     * patched pixelformat - leaving width/height/field at whatever the
     * driver defaulted to, which is how bytesperline=0/sizeimage=0 slipped
     * through silently. */
    struct v4l2_format fmt = {};
    fmt.type                 = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width        = CAM_WIDTH;
    fmt.fmt.pix.height       = CAM_HEIGHT;
    fmt.fmt.pix.pixelformat  = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field        = V4L2_FIELD_NONE;
    if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565 %dx%d) failed errno=%d", CAM_WIDTH, CAM_HEIGHT, errno);
        return ESP_FAIL;
    }
    ioctl(s_video_fd, VIDIOC_G_FMT, &fmt);
    s_stride = fmt.fmt.pix.bytesperline;
    ESP_LOGI(TAG, "Negotiated: %ux%u bytesperline=%u sizeimage=%u",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned)fmt.fmt.pix.bytesperline, (unsigned)fmt.fmt.pix.sizeimage);
    if (s_stride == 0 || fmt.fmt.pix.sizeimage == 0) {
        ESP_LOGE(TAG, "Driver reported zero stride/sizeimage - upstream driver bug,");
        ESP_LOGE(TAG, "falling back to packed stride %d for the copy loop.", CAM_WIDTH * 2);
        s_stride = CAM_WIDTH * 2;
    }
    if (fmt.fmt.pix.width != CAM_WIDTH || fmt.fmt.pix.height != CAM_HEIGHT) {
        ESP_LOGE(TAG, "Resolution mismatch: crop constants assume %dx%d!", CAM_WIDTH, CAM_HEIGHT);
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req = {};
    req.count  = NUM_V4L2_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed for buf %d", i);
            return ESP_FAIL;
        }
        s_v4l2_bufs[i] = (uint8_t *)mmap(NULL, buf.length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         s_video_fd, buf.m.offset);
        s_v4l2_buf_len = buf.length;
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed for buf %d", i);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Mapped %u buffers, %zu bytes each", (unsigned)req.count, s_v4l2_buf_len);

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Camera streaming STARTED");
    return ESP_OK;
}

/* ----------------------------- frame path ------------------------------- */

/* Optional diagnostic-only gain. Compiled to a straight copy at APP_SW_GAIN=1. */
static inline uint16_t apply_sw_gain(uint16_t p)
{
#if APP_SW_GAIN > 1
    uint32_t r = (p >> 11) & 0x1F;
    uint32_t g = (p >> 5)  & 0x3F;
    uint32_t b = p & 0x1F;
    r *= APP_SW_GAIN; g *= APP_SW_GAIN; b *= APP_SW_GAIN;
    if (r > 0x1F) r = 0x1F;
    if (g > 0x3F) g = 0x3F;
    if (b > 0x1F) b = 0x1F;
    return (uint16_t)((r << 11) | (g << 5) | b);
#else
    return p;
#endif
}

/* Center-crop CROP_W x CROP_H out of the camera frame into the letterboxed
 * LCD framebuffer. Uses the REAL negotiated stride, and black (not grey)
 * bars so panel artifacts are distinguishable from image content. */
static void draw_crop(const uint8_t *src, uint16_t *fb)
{
    /* bars are already black from the one-time clear; only write image rows */
    for (int y = 0; y < CROP_H; y++) {
        const uint16_t *src_row = (const uint16_t *)(src + (size_t)(CROP_SRC_Y + y) * s_stride)
                                  + CROP_SRC_X;
        uint16_t *dst_row = fb + (LETTERBOX_TOP + y) * LCD_H_RES;
#if APP_SW_GAIN > 1
        for (int x = 0; x < CROP_W; x++) dst_row[x] = apply_sw_gain(src_row[x]);
#else
        memcpy(dst_row, src_row, CROP_W * sizeof(uint16_t));
#endif
    }
}

/* --------------------------------- main --------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "========== CAM+LCD START ==========");

    init_backlight();
    ESP_ERROR_CHECK(init_lcd());

    /* Draw directly into the DPI panel framebuffer: no 2 MB bounce buffer,
     * no full-screen copy per frame. That copy was a major source of the
     * visible flicker/tearing alongside the ISP problems. */
    void *fb_void = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_lcd_panel, 1, &fb_void));
    uint16_t *lcd_fb = (uint16_t *)fb_void;
    const size_t fb_size = (size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    memset(lcd_fb, 0x00, fb_size);   /* black: letterbox bars + idle screen */
    esp_cache_msync(lcd_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

#if LCD_SELFTEST
    static const uint16_t test_colors[3] = {0xF800, 0x07E0, 0x001F};
    static const char *test_names[3] = {"RED", "GREEN", "BLUE"};
    for (int t = 0; t < 3; t++) {
        ESP_LOGI(TAG, "TEST: solid %s", test_names[t]);
        for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) lcd_fb[i] = test_colors[t];
        esp_cache_msync(lcd_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    memset(lcd_fb, 0x00, fb_size);
    esp_cache_msync(lcd_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif

    init_xclk();
    early_i2c_bus_scan();

    ESP_ERROR_CHECK(init_camera());
    diagnose_camera_hardware(s_video_fd);
    neutralize_controls_if_present(s_video_fd);

    if (!s_ipa_alive) {
        ESP_LOGW(TAG, "Running with UNCALIBRATED ISP - expect dark corners, grey cast.");
        ESP_LOGW(TAG, "This is the known pre-fix state. APP_SW_GAIN=%d (1=off).", APP_SW_GAIN);
    }

    ESP_LOGI(TAG, "Streaming: %dx%d center-crop -> %dx%d + black letterbox",
             CROP_W, CROP_H, LCD_H_RES, LCD_V_RES);

    int frame_count = 0;
    while (1) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            if (errno != EAGAIN) ESP_LOGW(TAG, "VIDIOC_DQBUF errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (buf.bytesused && buf.bytesused < (unsigned)(CROP_SRC_Y + CROP_H) * s_stride) {
            ESP_LOGW(TAG, "short frame: bytesused=%u - skipping", (unsigned)buf.bytesused);
            ioctl(s_video_fd, VIDIOC_QBUF, &buf);
            continue;
        }

        if ((frame_count % 60) == 0) {
            uint16_t *cam = (uint16_t *)s_v4l2_bufs[buf.index];
            ESP_LOGI(TAG, "frame %d: %04X %04X %04X %04X", frame_count,
                     cam[0], cam[1], cam[CAM_WIDTH], cam[CAM_WIDTH + 1]);
        }
        frame_count++;

        draw_crop(s_v4l2_bufs[buf.index], lcd_fb);
        esp_cache_msync(lcd_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF failed");
        }
        /* no artificial delay: DQBUF pacing + DPI vsync cadence the loop */
    }
}
