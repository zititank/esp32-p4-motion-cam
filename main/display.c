#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_jd9365.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "driver/ledc.h"

#include "app_config.h"
#include "display.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_mipi_io = NULL;
static size_t s_fb_size = 0;

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

static esp_err_t init_lcd(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_LDO_CHAN,
        .voltage_mv = MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = LCD_DSI_BUS_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_mipi_io));

    esp_lcd_dpi_panel_config_t dpi_config = LCD_DPI_CONFIG(LCD_COLOR_FORMAT);
    dpi_config.num_fbs = 1;
    dpi_config.dpi_clock_freq_mhz = LCD_DPI_CLOCK_MHZ;

    jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RESET_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(s_mipi_io, &panel_cfg, &s_lcd_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));

#if LCD_MANUAL_DSI_SEQ
    ESP_LOGI(TAG, "Sending manual DSI wake-up sequence");
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

esp_err_t display_init(uint16_t **fb, size_t *fb_size)
{
    init_backlight();
    esp_err_t ret = init_lcd();
    if (ret != ESP_OK) {
        return ret;
    }

    /* Drawing into the DPI framebuffer directly avoids a 2 MB bounce buffer
     * and a full-screen copy per frame. */
    void *fb_void = NULL;
    ret = esp_lcd_dpi_panel_get_frame_buffer(s_lcd_panel, 1, &fb_void);
    if (ret != ESP_OK) {
        return ret;
    }
    s_fb_size = (size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    memset(fb_void, 0x00, s_fb_size);
    esp_cache_msync(fb_void, s_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    *fb = (uint16_t *)fb_void;
    if (fb_size) *fb_size = s_fb_size;
    return ESP_OK;
}

void display_flush(uint16_t *fb)
{
    esp_cache_msync(fb, s_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void display_selftest(uint16_t *fb)
{
    static const uint16_t test_colors[3] = {0xF800, 0x07E0, 0x001F};
    static const char *test_names[3] = {"RED", "GREEN", "BLUE"};
    for (int t = 0; t < 3; t++) {
        ESP_LOGI(TAG, "selftest: solid %s", test_names[t]);
        for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) fb[i] = test_colors[t];
        display_flush(fb);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    memset(fb, 0x00, s_fb_size);
    display_flush(fb);
}

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

void display_draw_crop(const uint8_t *src, int src_stride, uint16_t *fb)
{
    for (int y = 0; y < CROP_H; y++) {
        const uint16_t *src_row = (const uint16_t *)(src + (size_t)(CROP_SRC_Y + y) * src_stride)
                                  + CROP_SRC_X;
        uint16_t *dst_row = fb + (LETTERBOX_TOP + y) * LCD_H_RES;
#if APP_SW_GAIN > 1
        for (int x = 0; x < CROP_W; x++) dst_row[x] = apply_sw_gain(src_row[x]);
#else
        memcpy(dst_row, src_row, CROP_W * sizeof(uint16_t));
#endif
    }
}
