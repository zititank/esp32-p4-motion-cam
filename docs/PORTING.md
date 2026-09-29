# Porting to another board

This repo is a reference example, not a drop-in firmware. I wrote it for one board, the Guition JC8012P4A1C. ESP32-P4 boards that look similar on paper differ in exactly the places firmware cares about: which pins the I2C bus uses, how the camera gets its clock, what panel is fitted and how it's timed, and which chip revision is on the board. If any of those are wrong, you'll usually get a black screen and very little to go on.

This page lists what to check and where each value lives. Most of them are in one header, [`main/board_jc8012p4a1.h`](../main/board_jc8012p4a1.h). A few live in the sensor driver or in `sdkconfig.defaults`, and the table says so.

## Checklist

| What | On this board | Where it lives |
|---|---|---|
| **I2C / SCCB bus** | | |
| I2C port | 0 | `SCCB_I2C_PORT` in `board_jc8012p4a1.h` |
| SDA / SCL pins | GPIO7 / GPIO8 | `SCCB_SDA_GPIO`, `SCCB_SCL_GPIO` |
| Bus speed | 100 kHz | `SCCB_FREQ_HZ` |
| Sensor 7-bit address | 0x36 | `CAM_SCCB_ADDR` (used by the boot I2C scan) **and** `OV02C10_SCCB_ADDR` in `components/esp_cam_sensor/sensors/ov02c10/ov02c10.c` (used for detection). Change both. |
| **Camera clock and control pins** | | |
| XCLK pin | GPIO6 | `XCLK_GPIO` |
| XCLK frequency | 19.2 MHz | `XCLK_FREQ_HZ` **and** `.xclk` in the format table in `ov02c10.c`. They must match, and the register table must be written for that clock. |
| LEDC timer/channel for XCLK | `LEDC_TIMER_2` / `LEDC_CHANNEL_1` | `XCLK_TIMER`, `XCLK_CHANNEL` |
| Sensor reset / power-down | not connected (-1) | `CAM_RESET_GPIO`, `CAM_PWDN_GPIO` |
| **MIPI-CSI link** | | |
| Lane count | 2 | `.mipi_info.lane_num` in the format table in `ov02c10.c`, plus the lane registers in `private_include/ov02c10_settings.h` |
| Lane rate | 800 Mbps | `.mipi_info.mipi_clk` in `ov02c10.c`; follows from XCLK and the sensor PLL registers |
| Sensor output size | 1920x1080 RAW10 | the format table in `ov02c10.c`, and `CAM_WIDTH` / `CAM_HEIGHT` in `board_jc8012p4a1.h` (must match; `camera_init()` refuses to start otherwise) |
| V4L2 capture buffers | 4 | `NUM_V4L2_BUFFERS` |
| **Panel** | | |
| Controller | JD9365 | `esp_lcd_new_panel_jd9365()` in `main/display.c`, component in `main/idf_component.yml` |
| Resolution and orientation | 800x1280 portrait | `LCD_H_RES`, `LCD_V_RES` |
| DSI bus (lanes) | 2 lanes | `LCD_DSI_BUS_CONFIG()` = `JD9365_PANEL_BUS_DSI_2CH_CONFIG()` |
| Timing / refresh | 60 Hz preset, DPI clock 20 MHz | `LCD_DPI_CONFIG()` = `JD9365_800_1280_PANEL_60HZ_DPI_CONFIG()`, `LCD_DPI_CLOCK_MHZ` |
| Pixel format | RGB565 | `LCD_COLOR_FORMAT`, `LCD_BITS_PER_PIXEL` |
| Panel reset pin | not used (-1) | `LCD_RESET_GPIO` |
| DSI PHY power | LDO channel 3, 2500 mV | `MIPI_LDO_CHAN`, `MIPI_LDO_VOLTAGE_MV` |
| Backlight | GPIO23, LEDC 5 kHz, full duty | `BKLT_GPIO`, `BKLT_TIMER`, `BKLT_CHANNEL`; frequency and duty are in `init_backlight()` in `main/display.c` |
| **Layout on screen** | | |
| Crop and letterbox | 800x1080 centre crop, 100 px bars | `CROP_W`, `CROP_H`, `CROP_SRC_X`, `CROP_SRC_Y`, `LETTERBOX_TOP` in `main/app_config.h` (derived from the values above) |
| **Chip and memory** | | |
| Chip revision | pre-v3 (below v3.0) | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_1=y` in `sdkconfig.defaults` |
| CPU clock | 360 MHz | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y` |
| Flash size | 16 MB | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` |
| PSRAM | 32 MB, hex mode, 200 MHz | `CONFIG_SPIRAM_MODE_HEX=y`, `CONFIG_SPIRAM_SPEED_200M=y` |
| Malloc placement | up to 16 KB internal, 32 KB internal reserve | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` |
| Task watchdog | 20 s, no panic | `CONFIG_ESP_TASK_WDT_TIMEOUT_S`, `CONFIG_ESP_TASK_WDT_PANIC` (see [lessons, problem 1](LESSONS_AND_OPEN_PROBLEMS.md#1-watchdog-resets-stalling-the-pipeline)) |
| **Software versions** | | |
| ESP-IDF | 5.4.4 | CI workflow in `.github/workflows/build.yml`; `main/idf_component.yml` only says `>=5.4.0` |
| Components | `esp_video` 2.3.0, `esp_lcd_jd9365` 2.0.2, `esp_cam_sensor` 2.3.0 (vendored) | `main/idf_component.yml`, `components/esp_cam_sensor/` |
| ISP tuning file | `ov02c10_default.json` (not calibrated) | `CONFIG_CAMERA_OV02C10_*_IPA_JSON_CONFIGURATION_FILE` in `sdkconfig.defaults` |
| Sensor detect method | dynamic link (default) | `CONFIG_CAMERA_SENSOR_MOTOR_DETECT_METHOD_*`. Static store won't find the OV02C10, see the [engineering log](ENGINEERING_LOG.md#ov02c10-missing-from-the-static-detect-table). |

## Notes on the harder items

### Chip revision

ESP32-P4 silicon before v3.0 and v3.x need different builds. My board has pre-v3 silicon, so `sdkconfig.defaults` selects `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. On a board with v3 silicon, remove that line and the `REV_MIN` line and let ESP-IDF use its defaults.

To see what you have, look at the chip revision that `esptool` prints when it connects, or the one the bootloader prints at boot.

The revision matters for more than the build. For some sensors, `esp_cam_sensor` ships separate ISP tuning files for the two revisions (`*_p4_eco4.json` and `*_p4_eco5.json`) and picks one based on this same option in `project_include.cmake`. So the ISP behaves differently between revisions, and a tuning file for one may not suit the other.

I also haven't checked whether 360 MHz is the right CPU clock for other revisions. It's just the value that worked here.

### Camera clock

The sensor turns XCLK into its internal pixel clock and MIPI bit rate with a PLL. The register table decides the multipliers. That gives three values that have to agree:

1. the clock the board actually feeds the sensor (`XCLK_FREQ_HZ`, or a crystal on the camera module if your board has one);
2. the `.xclk` field in the driver format table;
3. the PLL registers in the register table.

If you move this driver to a board that supplies 24 MHz, the 19.2 MHz register table will give the wrong lane rate. You'd need a table written for 24 MHz. This took me a long time; see [lessons, problem 6](LESSONS_AND_OPEN_PROBLEMS.md#6-sensor-clock-mismatch).

### Panel

`esp_lcd_jd9365` sends a default init sequence when `vendor_config.init_cmds` is not set, and that's what this firmware relies on. JD9365 panels from other vendors often need their own init commands (gamma, power, timing), which the board vendor usually supplies. If the backlight comes on but the screen stays black or shows noise, check the init sequence and the DPI timing first.

Set `LCD_SELFTEST` to 1 in `main/app_config.h` to show solid red, green and blue for a second each at boot. If that doesn't work, don't bother with the camera yet.

### ISP tuning

The ISP tuning file (`sensors/ov02c10/cfg/ov02c10_default.json`) is specific to a sensor and even to a module: lens, IR filter, and the sensor's own response. The one in this repo isn't a real calibration even for my module (flat lens-shading tables, generic colour matrix, and auto-exposure fields copied from Espressif's SC2336 file because `esp_ipa` 2.2 won't build without them). So even when the code builds and streams on your board, don't expect the image quality to carry over. A proper tuning file needs calibration shots of test charts under known lights.

### I2C bus sharing

On this board the camera shares GPIO7/GPIO8 with touch, RTC and audio. On other boards the camera may be on its own bus or a different port. The boot I2C scan (`diag_i2c_scan()`) prints every address that answers, so run it first and check that the sensor shows up where you expect.

## Suggested bring-up order

This is what I'd do on a new board, one step at a time:

1. **Panel only.** Set `LCD_SELFTEST=1`. Get solid colours on screen.
2. **I2C.** Check that the boot scan finds the sensor address.
3. **Chip ID.** The driver logs the chip-ID readback. It has to match before anything else can work.
4. **Stream without tuning.** Set `CONFIG_CAMERA_OV02C10_NO_IPA_JSON_CONFIGURATION_FILE=y` and `APP_MOTION_ENABLE=0`. A dark, grey, uncalibrated image means the CSI link and the ISP are working.
5. **Tuning.** Switch the IPA JSON back on. The boot log should list V4L2 controls; zero controls means the tuning file didn't load.
6. **Motion.** Set `APP_MOTION_ENABLE=1`.

## Using a different sensor

If your board has a different camera, check whether `esp_cam_sensor` already supports it (`components/esp_cam_sensor/sensors/`). If it does, drop the vendored component, depend on the registry version in `main/idf_component.yml`, select your sensor in `sdkconfig.defaults`, and update `CAM_WIDTH`, `CAM_HEIGHT` and the XCLK settings. The application code only talks V4L2 and RGB565, so it doesn't care which sensor is behind it.

If it isn't supported, the OV02C10 driver here is a small example of the parts you need: a register table, a chip-ID check, a format table, and a detect function.
