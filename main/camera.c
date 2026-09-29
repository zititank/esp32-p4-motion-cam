#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#include "board_jc8012p4a1.h"
#include "camera.h"

static const char *TAG = "camera";

static int      s_video_fd = -1;
static uint8_t *s_v4l2_bufs[NUM_V4L2_BUFFERS];
static size_t   s_v4l2_buf_len = 0;
static int      s_stride = 0;            /* bytes per line after S_FMT */

void camera_start_xclk(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num       = XCLK_TIMER,
        .freq_hz         = XCLK_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num       = XCLK_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = XCLK_CHANNEL,
        .timer_sel      = XCLK_TIMER,
        .duty           = 1,
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_LOGI(TAG, "XCLK %d Hz started on GPIO%d", XCLK_FREQ_HZ, XCLK_GPIO);
}

esp_err_t camera_init(void)
{
    esp_video_init_csi_config_t csi_cfg = {};
    csi_cfg.sccb_config.init_sccb = true;
    csi_cfg.sccb_config.i2c_config.port = SCCB_I2C_PORT;
    csi_cfg.sccb_config.i2c_config.scl_pin = SCCB_SCL_GPIO;
    csi_cfg.sccb_config.i2c_config.sda_pin = SCCB_SDA_GPIO;
    csi_cfg.sccb_config.freq = SCCB_FREQ_HZ;
    csi_cfg.reset_pin = (gpio_num_t)(CAM_RESET_GPIO);
    csi_cfg.pwdn_pin  = (gpio_num_t)(CAM_PWDN_GPIO);

    esp_video_init_config_t vid_cfg = { .csi = &csi_cfg };

    esp_err_t ret = esp_video_init(&vid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "esp_video_init OK");

    /* The RGB565 capture node is not always /dev/video0. */
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
        ESP_LOGE(TAG, "No /dev/videoX with RGB565 found");
        return ESP_FAIL;
    }

    /* Set every field explicitly. Inheriting G_FMT and patching only the
     * pixel format left bytesperline=0 and sizeimage=0. */
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
        ESP_LOGE(TAG, "Driver reported zero stride/sizeimage,");
        ESP_LOGE(TAG, "falling back to packed stride %d.", CAM_WIDTH * 2);
        s_stride = CAM_WIDTH * 2;
    }
    if (fmt.fmt.pix.width != CAM_WIDTH || fmt.fmt.pix.height != CAM_HEIGHT) {
        ESP_LOGE(TAG, "Resolution mismatch: crop constants assume %dx%d", CAM_WIDTH, CAM_HEIGHT);
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
    ESP_LOGI(TAG, "Camera streaming started");
    return ESP_OK;
}

int camera_fd(void)
{
    return s_video_fd;
}

int camera_stride(void)
{
    return s_stride;
}

esp_err_t camera_dequeue(camera_frame_t *frame)
{
    struct v4l2_buffer *buf = &frame->buf;
    *buf = (struct v4l2_buffer){};
    buf->type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_video_fd, VIDIOC_DQBUF, buf) != 0) {
        if (errno != EAGAIN) ESP_LOGW(TAG, "VIDIOC_DQBUF errno=%d", errno);
        return ESP_FAIL;
    }
    frame->data = s_v4l2_bufs[buf->index];
    return ESP_OK;
}

esp_err_t camera_requeue(camera_frame_t *frame)
{
    if (ioctl(s_video_fd, VIDIOC_QBUF, &frame->buf) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void camera_neutralize_controls(void)
{
    struct v4l2_control c = {};
    c.id = V4L2_CID_BRIGHTNESS;
    c.value = 0;
    if (ioctl(s_video_fd, VIDIOC_S_CTRL, &c) == 0) {
        ESP_LOGI(TAG, "V4L2_CID_BRIGHTNESS -> 0");
    }
}
