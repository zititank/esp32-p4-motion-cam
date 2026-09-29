#include <stdint.h>
#include <sys/ioctl.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "driver/i2c_master.h"

#include "board_jc8012p4a1.h"
#include "diagnostics.h"

static const char *TAG = "diag";

void diag_i2c_scan(void)
{
    ESP_LOGI(TAG, "==================== I2C/SCCB BUS SCAN (SDA=%d SCL=%d) ====================",
             SCCB_SDA_GPIO, SCCB_SCL_GPIO);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                    = -1,
        .sda_io_num                  = SCCB_SDA_GPIO,
        .scl_io_num                  = SCCB_SCL_GPIO,
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
            ESP_LOGI(TAG, "  found ACK at 0x%02X%s", addr,
                     (addr == CAM_SCCB_ADDR) ? "  <-- expected sensor addr" : "");
            found++;
        }
    }
    ESP_LOGI(TAG, "==================== scan done: %d device(s) found ====================", found);
    i2c_del_master_bus(bus);
}

bool diag_camera(int fd)
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

    /* AE/AWB/gain controls only exist when esp_ipa loaded a tuning JSON for
     * the sensor. Without them the ISP runs uncalibrated. */
    ESP_LOGI(TAG, "-- V4L2 controls --");
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
    bool ipa_alive = (ctrl_count > 0);
    if (!ipa_alive) {
        ESP_LOGE(TAG, "  No controls: esp_ipa found no tuning JSON for this sensor.");
        ESP_LOGE(TAG, "  Expected sensors/ov02c10/cfg/ov02c10_default.json with a");
        ESP_LOGE(TAG, "  top-level key equal to the driver's registered name.");
    }

    ESP_LOGI(TAG, "=====================================================================");
    return ipa_alive;
}

void diag_heap_report(void)
{
    ESP_LOGI(TAG, "heap: psram free=%u largest=%u | internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
