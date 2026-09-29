# Hardware

Everything in this repo was written for one board. If you have a different ESP32-P4 board, read [PORTING.md](PORTING.md) first.

## The board

**Guition JC8012P4A1C** (my unit is labelled `JC8012P4A1C-I-W-Y1`).

Guition is a third-party board maker. The board is not made by Espressif, but it uses Espressif's chips and the firmware uses Espressif's official software components (ESP-IDF, `esp_video`, `esp_cam_sensor`, `esp_ipa`, `esp_lcd_jd9365`).

| Part | What it is | Used by this firmware |
|---|---|---|
| Main SoC | ESP32-P4NRW32 (dual-core RISC-V, MIPI-CSI, MIPI-DSI, ISP, PPA) | yes |
| PSRAM | 32 MB, in-package | yes, for camera buffers and the framebuffer |
| Flash | 16 MB | yes |
| Radio co-processor | ESP32-C6-MINI-1U (Wi-Fi 6 / BLE) | no |
| Display | 10.1" IPS, 800x1280 portrait, JD9365 controller, 2-lane MIPI-DSI | yes |
| Touch | GSL3680 on the shared I2C bus | no |
| Camera | OV02C10, 2 MP, 2-lane MIPI-CSI, RAW10 Bayer | yes |
| Audio codec, RTC | ES8311 and an RTC on the shared I2C bus | no |

The chip on my board is pre-v3 ESP32-P4 silicon (revision below v3.0). The firmware is built for that with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. Newer batches of this board may ship with v3 silicon. See [PORTING.md](PORTING.md#chip-revision).

## Pin map (as used in `main/board_jc8012p4a1.h`)

| Signal | GPIO / value | Constant |
|---|---|---|
| I2C / SCCB SDA | GPIO7 | `SCCB_SDA_GPIO` |
| I2C / SCCB SCL | GPIO8 | `SCCB_SCL_GPIO` |
| I2C port for SCCB | 0 | `SCCB_I2C_PORT` |
| SCCB clock | 100 kHz | `SCCB_FREQ_HZ` |
| OV02C10 address (7-bit) | 0x36 | `CAM_SCCB_ADDR` |
| Camera XCLK | GPIO6, 19.2 MHz from LEDC | `XCLK_GPIO`, `XCLK_FREQ_HZ` |
| Camera reset / power-down | not connected (-1) | `CAM_RESET_GPIO`, `CAM_PWDN_GPIO` |
| LCD reset | not used (-1) | `LCD_RESET_GPIO` |
| Backlight PWM | GPIO23 | `BKLT_GPIO` |
| MIPI DSI PHY power | on-chip LDO channel 3, 2500 mV | `MIPI_LDO_CHAN`, `MIPI_LDO_VOLTAGE_MV` |

GPIO7/GPIO8 is one bus shared by the touch controller, the RTC, the ES8311 codec and the camera. That's why the firmware runs an I2C scan at boot (`diag_i2c_scan()` in `main/diagnostics.c`): it lists every device that ACKs, and marks 0x36 as the expected sensor. The scan runs on a temporary bus before `esp_video_init()` claims port 0.

The MIPI-CSI and MIPI-DSI lanes are dedicated pins on the ESP32-P4, so they don't appear in the pin table.

## Camera: OV02C10

- Output: RAW10 Bayer, 1920x1080, 2 MIPI-CSI lanes.
- Driver format table (`components/esp_cam_sensor/sensors/ov02c10/ov02c10.c`): `xclk = 19200000`, `mipi_clk = 800000000`, `lane_num = 2`, 30 fps.
- Chip ID: registers 0x300A/0x300B read back 0x5602.
- Bayer order in the driver: GRBG. This is an open question, see [ENGINEERING_LOG.md](ENGINEERING_LOG.md#bayer-order).

Espressif's `esp_cam_sensor` 2.3.0 doesn't support the OV02C10, so this repo carries its own driver. Its register table is derived from Intel's GPL-2.0 Linux driver. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Display: JD9365, 800x1280

- 2-lane MIPI-DSI, RGB565, one DPI framebuffer in PSRAM (800 x 1280 x 2 B = 2,048,000 B).
- Timing comes from `JD9365_800_1280_PANEL_60HZ_DPI_CONFIG()` in `esp_lcd_jd9365`, with the DPI clock overridden to 20 MHz (`LCD_DPI_CLOCK_MHZ`).
- The panel is portrait and the camera is landscape. There is no rotation in the firmware; it takes a full-height 800x1080 centre crop and puts 100 px black bars above and below.

## Sources

I didn't have an official schematic. The pin assignments and part list above come from these community sources, cross-checked against what worked on my board:

- sukesh-ak, JC8012P4A1 notes and examples: <https://github.com/sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6>
- profi-max, JC8012P4A1 BSP for ESP32-P4 (pin tables, shared I2C bus on GPIO7/GPIO8): <https://github.com/profi-max/JC8012P4A1_BSP_ESP32P4>
- LaskaKit product page (specs): <https://www.laskakit.cz/en/guition-jc8012p4a1c-esp32-p4-c6-m5-vyvojovy-modul-s-displejem-10-1--800x1280/>
- Home Assistant community thread on the board: <https://community.home-assistant.io/t/jc8012p4a1-guition-esp32-p4-esp32-c6/939971>
