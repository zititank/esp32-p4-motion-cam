#pragma once

#include <stdbool.h>

/*
 * Probes 0x08..0x77 on a temporary I2C bus on the SCCB pins. Must run before
 * camera_init(), because esp_video_init claims the bus.
 */
void diag_i2c_scan(void);

/*
 * Logs the V4L2 capability, formats, active format and controls of fd.
 * Returns true if any control was enumerated, which means the ISP tuning
 * (IPA JSON) was loaded.
 */
bool diag_camera(int fd);

/* Logs free and largest free block for PSRAM and internal RAM. */
void diag_heap_report(void);
