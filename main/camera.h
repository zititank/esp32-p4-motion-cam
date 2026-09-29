#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "linux/videodev2.h"

typedef struct {
    const uint8_t *data;     /* RGB565, CAM_WIDTH x CAM_HEIGHT, camera_stride() bytes per line */
    struct v4l2_buffer buf;  /* as returned by VIDIOC_DQBUF; bytesused may be 0 */
} camera_frame_t;

/* Starts the sensor input clock (XCLK_FREQ_HZ on XCLK_GPIO). */
void camera_start_xclk(void);

/*
 * Runs esp_video_init (which probes the sensor over SCCB), opens the first
 * V4L2 node that offers RGB565, sets CAM_WIDTH x CAM_HEIGHT RGB565, maps
 * NUM_V4L2_BUFFERS MMAP buffers, queues them and starts streaming.
 */
esp_err_t camera_init(void);

/* File descriptor of the open V4L2 capture node, or -1 before camera_init(). */
int camera_fd(void);

/* Bytes per line of a captured frame. Falls back to CAM_WIDTH * 2 if the driver reports 0. */
int camera_stride(void);

/* Takes the next filled buffer. Returns ESP_FAIL if none is ready (errno EAGAIN) or on error. */
esp_err_t camera_dequeue(camera_frame_t *frame);

/* Hands a buffer from camera_dequeue() back to the driver. */
esp_err_t camera_requeue(camera_frame_t *frame);

/* Sets V4L2_CID_BRIGHTNESS to 0 and leaves AE/AWB on auto. Only useful once ISP controls exist. */
void camera_neutralize_controls(void);
