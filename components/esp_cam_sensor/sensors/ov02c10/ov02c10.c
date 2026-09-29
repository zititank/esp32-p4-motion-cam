/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * OV02C10 driver for esp_cam_sensor (not part of upstream esp-video-components).
 *
 * The register init values in private_include/ov02c10_settings.h are derived
 * from Intel's OV02C10 Linux driver:
 *   Copyright (c) 2022 Intel Corporation, author Hao Yao, GPL-2.0
 *   https://github.com/intel/ipu6-drivers/blob/master/drivers/media/i2c/ov02c10.c
 *   (later upstreamed to mainline Linux as drivers/media/i2c/ov02c10.c)
 * They were adapted to the esp_cam_sensor API. Because of that origin this
 * driver is GPL-2.0-only, unlike the Apache-2.0 files around it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "ov02c10_settings.h"
#include "ov02c10_regs.h"

#define TAG "ov02c10"
#define OV02C10_SCCB_ADDR 0x36
#define OV02C10_PID       0x5602

static const esp_cam_sensor_format_t ov02c10_format_info[];
static const esp_cam_sensor_ops_t ov02c10_ops;

static esp_err_t ov02c10_read(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t *val)
{
    return esp_sccb_transmit_receive_reg_a16v8(sccb_handle, reg, val);
}

static esp_err_t ov02c10_write(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t val)
{
    return esp_sccb_transmit_reg_a16v8(sccb_handle, reg, val);
}

static esp_err_t ov02c10_write_array(esp_sccb_io_handle_t sccb_handle,
                                      const ov02c10_reginfo_t *regs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (regs[i].reg == OV02C10_REG_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(regs[i].val));
            continue;
        }
        esp_err_t ret = ov02c10_write(sccb_handle, regs[i].reg, regs[i].val);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Write reg 0x%04X fail", regs[i].reg);
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t ov02c10_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    esp_err_t ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_MODE_SELECT,
                                   enable ? OV02C10_MODE_STREAMING : OV02C10_MODE_STANDBY);
    if (ret == ESP_OK) {
        dev->stream_status = enable;
    }
    return ret;
}

static esp_err_t ov02c10_query_para_desc(esp_cam_sensor_device_t *dev,
                                          esp_cam_sensor_param_desc_t *qdesc)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ov02c10_get_para_value(esp_cam_sensor_device_t *dev,
                                         uint32_t id, void *arg, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ov02c10_set_para_value(esp_cam_sensor_device_t *dev,
                                         uint32_t id, const void *arg, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ov02c10_query_support_formats(esp_cam_sensor_device_t *dev,
                                                esp_cam_sensor_format_array_t *formats)
{
    formats->count = 1;
    formats->format_array = (esp_cam_sensor_format_t *)ov02c10_format_info;
    return ESP_OK;
}

static esp_err_t ov02c10_query_support_capability(esp_cam_sensor_device_t *dev,
                                                   esp_cam_sensor_capability_t *sensor_cap)
{
    sensor_cap->fmt_raw = 1;
    return ESP_OK;
}

static esp_err_t ov02c10_set_format(esp_cam_sensor_device_t *dev,
                                     const esp_cam_sensor_format_t *format)
{
    if (format == NULL) {
        format = &ov02c10_format_info[0];
    }

    esp_err_t ret = ov02c10_write_array(dev->sccb_handle, format->regs, format->regs_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set format regs fail");
        return ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT;
    }

    dev->cur_format = format;
    return ESP_OK;
}

static esp_err_t ov02c10_get_format(esp_cam_sensor_device_t *dev,
                                     esp_cam_sensor_format_t *format)
{
    if (dev->cur_format) {
        memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ov02c10_priv_ioctl(esp_cam_sensor_device_t *dev,
                                     uint32_t cmd, void *arg)
{
    esp_err_t ret = ESP_OK;
    esp_cam_sensor_reg_val_t *sensor_reg;

    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ESP_LOGI(TAG, "IOC_S_STREAM: %d", *(int *)arg);
        ret = ov02c10_set_stream(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID: {
        ESP_LOGI(TAG, "IOC_G_CHIP_ID: reading 0x300A/0x300B...");
        esp_cam_sensor_id_t *id = (esp_cam_sensor_id_t *)arg;
        uint8_t h = 0, l = 0;
        ret = ov02c10_read(dev->sccb_handle, 0x300A, &h);
        ESP_LOGI(TAG, "read 0x300A: ret=%s val=0x%02X", esp_err_to_name(ret), h);
        if (ret == ESP_OK) {
            ret = ov02c10_read(dev->sccb_handle, 0x300B, &l);
            ESP_LOGI(TAG, "read 0x300B: ret=%s val=0x%02X", esp_err_to_name(ret), l);
        }
        if (ret == ESP_OK) {
            id->pid = (h << 8) | l;
        }
        break;
    }
    case ESP_CAM_SENSOR_IOC_S_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = ov02c10_write(dev->sccb_handle, sensor_reg->regaddr, (uint8_t)sensor_reg->value);
        break;
    case ESP_CAM_SENSOR_IOC_G_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        {
            uint8_t val = 0;
            ret = ov02c10_read(dev->sccb_handle, sensor_reg->regaddr, &val);
            sensor_reg->value = val;
        }
        break;
    default:
        break;
    }
    return ret;
}

static esp_err_t ov02c10_delete(esp_cam_sensor_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return ESP_OK;
}

static const esp_cam_sensor_ops_t ov02c10_ops = {
    .query_para_desc          = ov02c10_query_para_desc,
    .get_para_value           = ov02c10_get_para_value,
    .set_para_value           = ov02c10_set_para_value,
    .query_support_formats    = ov02c10_query_support_formats,
    .query_support_capability = ov02c10_query_support_capability,
    .set_format               = ov02c10_set_format,
    .get_format               = ov02c10_get_format,
    .priv_ioctl               = ov02c10_priv_ioctl,
    .del                      = ov02c10_delete,
};

static const esp_cam_sensor_format_t ov02c10_format_info[] = {
    {
        .name      = "MIPI_2lane_19M2input_RAW10_1920x1080_30fps",
        .format    = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port      = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk      = 19200000,
        .width     = 1920,
        .height    = 1080,
        .regs      = ov02c10_init_1920x1080_raw10_2lane,
        .regs_size = sizeof(ov02c10_init_1920x1080_raw10_2lane) /
                     sizeof(ov02c10_init_1920x1080_raw10_2lane[0]),
        .fps       = 30,
        .isp_info  = &ov02c10_isp_info,
        .mipi_info = {
            .mipi_clk     = 800000000,
            .lane_num     = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
};

esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config)
{
    ESP_LOGI(TAG, ">>> OV02C10 detect called! <<<");

    esp_cam_sensor_device_t *dev = NULL;

    if (config == NULL) {
        ESP_LOGE(TAG, "config is NULL");
        return NULL;
    }

    dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "No memory for camera");
        return NULL;
    }

    dev->name        = "ov02c10";
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin    = config->xclk_pin;
    dev->reset_pin   = config->reset_pin;
    dev->pwdn_pin    = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops         = &ov02c10_ops;
    dev->cur_format  = &ov02c10_format_info[0];

    ESP_LOGI(TAG, "xclk_pin=%d reset_pin=%d pwdn_pin=%d port=%d",
             dev->xclk_pin, dev->reset_pin, dev->pwdn_pin, dev->sensor_port);

    esp_cam_sensor_id_t id;
    esp_err_t ret = ov02c10_priv_ioctl(dev, ESP_CAM_SENSOR_IOC_G_CHIP_ID, &id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get sensor ID failed: %s", esp_err_to_name(ret));
        goto err_free;
    }
    ESP_LOGI(TAG, "Sensor ID readback: 0x%04X (expected 0x%04X)", id.pid, OV02C10_PID);

    if (id.pid != OV02C10_PID) {
        ESP_LOGE(TAG, "Camera sensor is not OV02C10, PID=0x%x", id.pid);
        goto err_free;
    }
    ESP_LOGI(TAG, "Detected Camera sensor PID=0x%x", id.pid);

    return dev;

err_free:
    free(dev);
    return NULL;
}

#if CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR
ESP_CAM_SENSOR_DETECT_FN(ov02c10_detect, ESP_CAM_SENSOR_MIPI_CSI, OV02C10_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return ov02c10_detect(config);
}
#endif