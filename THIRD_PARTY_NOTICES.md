# Third-party notices

My own code in this repo is under the MIT licence in [LICENSE](LICENSE). That covers `main/`, `test/`, `tools/`, `.github/` and the documentation. It does **not** cover the third-party code listed below, which keeps its own licence.

I'm not a lawyer. This file records where things come from and what their licences say, as best I can tell. If you plan to ship firmware built from this repo, read [Distributing binaries](#distributing-binaries) and check for yourself.

## Code included in this repo

### esp_cam_sensor 2.3.0 (Espressif), vendored and modified

- Location: [`components/esp_cam_sensor/`](components/esp_cam_sensor/)
- Source: <https://github.com/espressif/esp-video-components/tree/master/esp_cam_sensor>, registry release 2.3.0 from <https://components.espressif.com>
- Licence: Apache-2.0. The original [`LICENSE`](components/esp_cam_sensor/LICENSE) is kept in the folder.
- Why it's vendored: it needed an extra sensor driver and build changes that can't be added from outside the component.

Changes compared to the registry release, checked against the release's own `CHECKSUMS.json`:

| File | Change |
|---|---|
| `CMakeLists.txt` | builds `sensors/ov02c10/` when `CONFIG_CAMERA_OV02C10` is set |
| `Kconfig` | sources `sensors/ov02c10/Kconfig.ov02c10` |
| `project_include.cmake` | passes the OV02C10 IPA JSON path to `esp_ipa` |
| `src/esp_cam_sensor_detect.c` | includes `ov02c10.h` and declares its detect function |
| `sensors/ov02c10/` (whole folder) | new; see the next section |

Apache-2.0 asks that modified files carry a notice saying they were changed. This table is that notice for the four modified upstream files.

### OV02C10 sensor driver: register table derived from Intel's Linux driver (GPL-2.0)

- Location: [`components/esp_cam_sensor/sensors/ov02c10/`](components/esp_cam_sensor/sensors/ov02c10/)
- Licence of these files: **GPL-2.0-only**. The source files carry `SPDX-License-Identifier: GPL-2.0-only` headers.
- Origin: the sensor register values in `private_include/ov02c10_settings.h` are derived from the OV02C10 Linux driver:
  - Copyright (c) 2022 Intel Corporation. Author: Hao Yao. Licence: GPL-2.0.
  - Intel's out-of-tree version: <https://github.com/intel/ipu6-drivers/blob/master/drivers/media/i2c/ov02c10.c>
  - Later upstreamed to mainline Linux as `drivers/media/i2c/ov02c10.c`, with work by the mainline upstreaming contributors (for example the patch series on the linux-media list, <https://www.spinics.net/lists/linux-media/msg271986.html>).
- What I did: adapted the register table to the `esp_cam_sensor` driver API (format table, chip-ID detection, stream on/off, SCCB access) and wrote the Kconfig and IPA hooks.
- The ISP tuning file `cfg/ov02c10_default.json` follows Espressif's `esp_ipa` JSON schema. It isn't derived from the Linux driver, and it isn't a real calibration. Some values in its `agc` section (`f_n0`, `f_m0`, the metering `mode` and its threshold tables) are copied from Espressif's `sensors/sc2336/cfg/sc2336_default_p4_eco4.json` in the same `esp_cam_sensor` 2.3.0 release (Apache-2.0, Copyright Espressif Systems). `esp_ipa` 2.2 needs these fields to build, and I had no OV02C10-specific values for them.

Because the register table comes from a GPL-2.0 work, I treat the driver source in that folder as GPL-2.0-only, separate from the Apache-2.0 files around it and from my MIT code. The one exception is `cfg/ov02c10_default.json`: it has nothing from the Linux driver in it and borrows values from an Apache-2.0 Espressif file, so I treat it as Apache-2.0.

## Code downloaded at build time (not in this repo)

The ESP-IDF component manager downloads these into `managed_components/`, which is not committed. Versions are the ones in my last dependency lock file for this project.

| Component | Version | Source | Licence |
|---|---|---|---|
| ESP-IDF | 5.4.4 | <https://github.com/espressif/esp-idf> | Apache-2.0 |
| `esp_video` | 2.3.0 | <https://github.com/espressif/esp-video-components/tree/master/esp_video> | ESPRESSIF MIT License (see below) |
| `esp_ipa` | 2.2.0 | <https://github.com/espressif/esp-video-components/tree/master/esp_ipa> | ESPRESSIF MIT License (see below); includes prebuilt libraries |
| `esp_sccb_intf` | 0.0.8 | <https://github.com/espressif/esp-video-components/tree/master/esp_sccb_intf> | Apache-2.0 |
| `esp_lcd_jd9365` | 2.0.2 | <https://github.com/espressif/esp-iot-solution/tree/master/components/display/lcd/esp_lcd_jd9365> | Apache-2.0 |
| `cmake_utilities` | 0.5.3 | <https://github.com/espressif/esp-iot-solution/tree/master/tools/cmake_utilities> | Apache-2.0 |
| `esp_h264` | 1.3.0 | <https://github.com/espressif/esp-h264-component> | Apache-2.0 |
| `usb_host_uvc` | 2.5.1 | <https://github.com/espressif/esp-usb/tree/master/host/class/uvc/usb_host_uvc> | Apache-2.0 |

`esp_h264` and `usb_host_uvc` are pulled in by `esp_video`. This firmware doesn't use them directly.

**About the ESPRESSIF MIT License.** `esp_video` and `esp_ipa` are not under plain MIT or Apache-2.0. Their `LICENSE` file is the MIT text with one added condition: permission is granted "for use on all ESPRESSIF SYSTEMS products". In practice: fine on an ESP32-P4, not for reuse on other vendors' chips. `esp_ipa` also ships its core as prebuilt binary libraries rather than source.

## Distributing binaries

This repo only distributes source code. A firmware binary built from it would link together:

- GPL-2.0-only code (the OV02C10 driver),
- Apache-2.0 code (ESP-IDF, `esp_cam_sensor` and others),
- ESPRESSIF MIT code, including `esp_ipa`'s prebuilt libraries.

The Free Software Foundation considers Apache-2.0 incompatible with GPL-2.0-only. And GPL-2.0 expects the corresponding source for the whole binary, which a prebuilt library doesn't provide. So I don't think a firmware binary built from this repo can be redistributed cleanly as it stands. Building it for your own use is a different situation from shipping it.

If that matters for your project, the clean fix is a replacement register table written from the sensor datasheet or from your own measurements, not from the GPL driver. Then the driver could be Apache-2.0 like the rest of the component.

## Board references

No code was copied from these. They were my sources for the pin map and part list in [docs/HARDWARE.md](docs/HARDWARE.md):

- sukesh-ak, JC8012P4A1 notes and examples: <https://github.com/sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6>
- profi-max, JC8012P4A1 BSP for ESP32-P4: <https://github.com/profi-max/JC8012P4A1_BSP_ESP32P4>
- LaskaKit product page: <https://www.laskakit.cz/en/guition-jc8012p4a1c-esp32-p4-c6-m5-vyvojovy-modul-s-displejem-10-1--800x1280/>
- Home Assistant community thread: <https://community.home-assistant.io/t/jc8012p4a1-guition-esp32-p4-esp32-c6/939971>

## Related work

- csvke's OV02C10 port for `esp-video-components`: pull request [espressif/esp-video-components#46](https://github.com/espressif/esp-video-components/pull/46), fork at <https://github.com/csvke/esp-video-components>. It's a separate, independent port; I didn't use its code. I mention it because anyone looking for OV02C10 support on the ESP32-P4 should know about it, and because it disagrees with this driver on the Bayer order (see [docs/ENGINEERING_LOG.md](docs/ENGINEERING_LOG.md#bayer-order)).

## Algorithm references

The motion detector is textbook background subtraction. The papers and docs behind each step are cited in [docs/ALGORITHM.md](docs/ALGORITHM.md#references). No code was taken from them.
