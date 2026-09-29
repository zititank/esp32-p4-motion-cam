# Engineering log

This is the bug-level log of getting the OV02C10 camera onto the JD9365 panel, plus the build fixes made while preparing this repo. Each entry has the same parts: symptom, evidence, root cause, fix, status.

The bigger, cross-cutting problems (watchdog, PSRAM, PPA rotation, clock mismatch, lighting) are in [LESSONS_AND_OPEN_PROBLEMS.md](LESSONS_AND_OPEN_PROBLEMS.md). Where an entry overlaps with those, I link there instead of repeating it.

**Where the evidence comes from.** The original single-file firmware from August 2026 is kept unchanged in [`docs/history/main_original_aug2026.c`](history/main_original_aug2026.c). Its comments recorded the boot-log lines I was working from. The build failure is from the `err` log I saved at the time. Entries marked "found while preparing this repo" are from September 2026 and are verified by reading code and building, not on the board.

Contents:

- [OV02C10 not supported upstream](#ov02c10-not-supported-upstream)
- [ISP not configured, zero V4L2 controls](#isp-not-configured-zero-v4l2-controls)
- [IPA JSON filename typo](#ipa-json-filename-typo)
- [IPA JSON crashed the esp_ipa config generator](#ipa-json-crashed-the-esp_ipa-config-generator)
- [Vignette and grey cast hidden by a x16 software gain](#vignette-and-grey-cast-hidden-by-a-x16-software-gain)
- [Grey bars around the image](#grey-bars-around-the-image)
- [bytesperline=0 after S_FMT](#bytesperline0-after-s_fmt)
- [Flicker from the bounce buffer](#flicker-from-the-bounce-buffer)
- [XCLK: 19.2 or 24 MHz](#xclk-192-or-24-mhz)
- [Kconfig rsource build failure](#kconfig-rsource-build-failure)
- [Stale sdkconfig option](#stale-sdkconfig-option)
- [Dead files in the sensor folder](#dead-files-in-the-sensor-folder)
- [Bayer order](#bayer-order)
- [Portrait panel, no hardware rotation](#portrait-panel-no-hardware-rotation)
- [Letterbox comment said 140 px](#letterbox-comment-said-140-px)
- [OV02C10 missing from the static detect table](#ov02c10-missing-from-the-static-detect-table)
- [What I'd do differently](#what-id-do-differently)

---

## OV02C10 not supported upstream

- **Symptom:** No sensor was detected at boot. `esp_cam_sensor` 2.3.0 has drivers for about thirty sensors, and the OV02C10 isn't one of them.
- **Evidence:** The sensor list in `components/esp_cam_sensor/sensors/` from the registry release. Once my driver existed, its chip-ID check passed at address 0x36, so the sensor had been alive on the bus all along.
- **Root cause:** Missing driver.
- **Fix:** I wrote `sensors/ov02c10/` for the `esp_cam_sensor` API. The register table (1920x1080, RAW10, 2 lanes, 19.2 MHz input) is derived from Intel's GPL-2.0 Linux driver. Detection reads registers 0x300A/0x300B and checks for chip ID 0x5602 at address 0x36. The driver is registered for MIPI-CSI auto-detection through `ESP_CAM_SENSOR_DETECT_FN`. The licence consequences are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
- **Status:** Fixed. The sensor was detected and streamed in August 2026.

## ISP not configured, zero V4L2 controls

- **Symptom:** The image reached the panel, but it was dark, with black corners and a washed-out grey cast.
- **Evidence:** From the boot log, as recorded in the original `main.c`:
  - `esp_video_init: failed to get configuration to initialize ISP controller`
  - `VIDIOC_QUERYCTRL` enumerated zero controls, so no exposure, gain or white-balance controls existed.
- **Root cause:** The ISP runs from a per-sensor tuning file (the IPA JSON). Without one, the RAW Bayer data goes through with no auto-exposure (too dark), no lens-shading correction (vignette), no white balance or colour matrix (grey cast) and no gamma. The tuning file never reached the build, for three separate reasons:
  1. `project_include.cmake` in `esp_cam_sensor` had no OV02C10 entry. That file is what passes each sensor's JSON path to `esp_ipa` (`ESP_IPA_JSON_CONFIG_FILE_PATH`), so the JSON was never registered.
  2. The file was named `ov02c10_default,json`, with a comma. See [IPA JSON filename typo](#ipa-json-filename-typo).
  3. Upstream `esp_ipa` 2.2.0 checks that each JSON exists with `message(FETAL_ERROR ...)`. `FETAL_ERROR` is a typo for `FATAL_ERROR`, and CMake treats an unknown mode word as part of an ordinary message. So a missing file prints a line and the build carries on. This is why a wrong path never failed the build.

  One more condition: the JSON's top-level key has to equal the name the driver registers (`dev->name = "ov02c10"`).
- **Fix (found while preparing this repo):** Added the OV02C10 block to `project_include.cmake`, renamed the file to `ov02c10_default.json`, added a "None" choice to `Kconfig.ov02c10`, and set `CONFIG_CAMERA_OV02C10_DEFAULT_IPA_JSON_CONFIGURATION_FILE=y` in `sdkconfig.defaults`. The key in the file is `ov02c10`, which matches the driver.
- **Status:** Fixed in the build, not re-tested on the board. This is the one intentional change to runtime behaviour compared to August. To get the August behaviour back, set `CONFIG_CAMERA_OV02C10_NO_IPA_JSON_CONFIGURATION_FILE=y`.

  The JSON is not a real calibration. It follows the `esp_ipa` schema, but its lens-shading tables are flat (all 1.0), its colour matrix is generic, not measured for this module, and part of its auto-exposure section comes from another sensor (see [IPA JSON crashed the esp_ipa config generator](#ipa-json-crashed-the-esp_ipa-config-generator)). With it, exposure and white balance should roughly work, and the vignette will likely stay.

## IPA JSON filename typo

- **Symptom:** Same as above: no tuning file loaded.
- **Evidence:** `sensors/ov02c10/cfg/ov02c10_default,json` in the original tree.
- **Root cause:** A comma instead of a dot. With the `esp_ipa` `FETAL_ERROR` typo, nothing complained.
- **Fix (found while preparing this repo):** Renamed to `ov02c10_default.json`.
- **Status:** Fixed.

## IPA JSON crashed the esp_ipa config generator

- **Symptom:** Once the JSON was actually registered (the two entries above), the build stopped. `esp_ipa` turns each tuning JSON into C code with a Python script at build time, and that script crashed on my file.
- **Evidence (found while preparing this repo):** A local build with native ESP-IDF v5.4.4 (Docker wasn't available on my PC). The generator failed while reading the `agc` (auto-exposure and gain) section of `ov02c10_default.json`.
- **Root cause:** `esp_ipa` 2.2 expects three things in `agc` that my file didn't have: the gain-step ratios `f_n0` and `f_m0`, and a metering `mode` with its settings. The generator reads them without a fallback. This never showed up in August because the file with the comma in its name was never embedded, so nothing ever parsed it.
- **Fix (found while preparing this repo):** Filled in those fields with the values from Espressif's own SC2336 file (`sensors/sc2336/cfg/sc2336_default_p4_eco4.json`, the one for pre-v3 chips): `f_n0` 0.32, `f_m0` 0.42, `mode` `high_light_priority`, and the threshold tables for the metering modes. After that the firmware builds with zero warnings.
- **Status:** Builds. These are Espressif's generic values for a different sensor, not tuned for the OV02C10. How auto-exposure behaves with them on the board is unknown until I flash it.

## Vignette and grey cast hidden by a x16 software gain

- **Symptom:** The picture was nearly black, so at some point I multiplied every RGB565 channel by 16 in software. That made something visible, but with clipped highlights, flat grey areas and strong noise.
- **Evidence:** The gain loop is still in the code as `APP_SW_GAIN` (in `main/app_config.h` and `display.c`). The original comments say the boost "only made an uncalibrated image visible".
- **Root cause:** The gain treated a symptom. The real problem was the missing ISP tuning (entry above). A x16 gain on 5- and 6-bit channels saturates almost everything, which is where the flat grey came from.
- **Fix:** `APP_SW_GAIN` defaults to 1 (off). It's only there for diagnosis now.
- **Status:** Fixed as a symptom. The cause is the IPA entry above.

## Grey bars around the image

- **Symptom:** Grey bars on the panel next to the image.
- **Evidence:** The original comments: the earlier code cropped 800x600 and filled the rest of the 800x1280 screen with 0x1010, a dark grey.
- **Root cause:** Part of the "grey bars" was my own fill colour. It made it hard to tell panel artefacts from image content.
- **Fix:** A full-height 800x1080 centre crop, and black bars (0x0000) above and below. The bars are cleared once at start-up and never drawn again.
- **Status:** Fixed.

## bytesperline=0 after S_FMT

- **Symptom:** The copy loop read garbage or only the first row.
- **Evidence:** From the boot log recorded in the original `main.c`: the active format reported `bytesperline=0` and `sizeimage=0`.
- **Root cause:** The old code read the format with `VIDIOC_G_FMT`, patched only the pixel format, and wrote it back. Width, height and field kept whatever the driver defaulted to, and the driver reported a zero stride.
- **Fix:** `camera_init()` in `main/camera.c` now fills every field of `VIDIOC_S_FMT` explicitly (1920x1080, RGB565, `V4L2_FIELD_NONE`), reads it back, and falls back to a packed stride of 3840 bytes if the driver still says 0. It also refuses to continue if the resolution doesn't match the crop constants. Short frames (`bytesused` smaller than the crop needs) are skipped in the frame loop.
- **Status:** Fixed.

## Flicker from the bounce buffer

- **Symptom:** Visible flicker and tearing.
- **Evidence:** The original comments name the full-screen copy as "a major source of the visible flicker/tearing alongside the ISP problems".
- **Root cause:** Each frame was built in a separate 2 MB buffer and then copied in full into the panel framebuffer. That's a lot of extra PSRAM traffic, and the copy could overlap with the panel reading the same memory.
- **Fix:** Draw the crop directly into the DPI framebuffer from `esp_lcd_dpi_panel_get_frame_buffer()`, then `esp_cache_msync()` once per frame.
- **Status:** Better, not perfect. There's one framebuffer (`num_fbs = 1`) and no sync to the panel refresh, so the CPU can still write while the panel reads. Tearing is still possible. Double buffering would fix it at the cost of another 2 MB of PSRAM. Also see [problem 2 in the lessons doc](LESSONS_AND_OPEN_PROBLEMS.md#2-psram-exhaustion).

## XCLK: 19.2 or 24 MHz

- **Symptom:** While I was bringing the sensor up, it was unclear whether the camera expected 19.2 MHz or 24 MHz. Many OmniVision modules expect 24 MHz, and a wrong clock gives a wrong MIPI lane rate.
- **Evidence:** The original XCLK code has a comment warning about exactly this.
- **Root cause and fix:** See [problem 6 in the lessons doc](LESSONS_AND_OPEN_PROBLEMS.md#6-sensor-clock-mismatch). In short, the register table comes from a driver written for 19.2 MHz, and the `esp_cam_sensor` format table keeps `.xclk` and the lane rate next to the registers.
- **Status:** Works at 19.2 MHz. Two open questions (LEDC jitter, and HTS units in the timing fields) are listed there.

## Kconfig rsource build failure

- **Symptom:** `idf.py build` failed during configuration, before compiling anything.
- **Evidence:** From the saved `err` log (paths shortened):

  ```
  kconfiglib.core.KconfigError: .../components/esp_cam_sensor/Kconfig:32:
  '.../components/esp_cam_sensor/sensors/sti2250/Kconfig.sti2250' not found
  (in 'rsource "sensors/sti2250/Kconfig.sti2250"')
  CMake Error at .../tools/cmake/kconfig.cmake:222 (message):
    Failed to run kconfgen
  ```

- **Root cause:** In my vendored copy, `sensors/sti2250/Kconfig.sti2250` had been renamed to `Kconfig.sti2250.sti`. I don't know when; most likely by accident. The top-level `Kconfig` still pointed at the old name. The registry checksums (`CHECKSUMS.json`) show that upstream 2.3.0 uses `Kconfig.sti2250`.
- **Fix (found while preparing this repo):** Restored the upstream filename.
- **Status:** Fixed.

## Stale sdkconfig option

- **Symptom:** None visible. The option just didn't do anything.
- **Evidence:** My old `sdkconfig.defaults` had `CONFIG_ESP_VIDEO_ENABLE_CSI_DRIVER=y`. That symbol doesn't exist in `esp_video` 2.3.0.
- **Root cause:** Probably copied from an older example or guide. Kconfig ignores assignments to unknown symbols in `sdkconfig.defaults` (at most with a warning), so nothing failed.
- **Fix (found while preparing this repo):** Replaced with `CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y`, which is the 2.3.0 name. It was already on by default, which is why the old file still worked.
- **Status:** Fixed.

## Dead files in the sensor folder

- **Symptom:** Confusion about where the IPA fix belonged. The original comment says "make sure the ov02c10 CMakeLists EMBED_FILES embeds it".
- **Evidence:** The OV02C10 folder had its own `CMakeLists.txt`, `idf_component.yml`, `Kconfig` and duplicate copies of `ov02c10_regs.h` / `ov02c10_settings.h` next to `private_include/`.
- **Root cause:** Leftovers from treating the driver as a separate component. `esp_cam_sensor` builds all its sensors from its own top-level `CMakeLists.txt` and `Kconfig`, so the files in the sensor folder were never used. The JSON path goes through `project_include.cmake`, not `EMBED_FILES`.
- **Fix (found while preparing this repo):** Removed the dead files. The driver is wired in through the top-level `CMakeLists.txt`, `Kconfig` and `project_include.cmake` of `esp_cam_sensor`.
- **Status:** Fixed.

## Bayer order

- **Symptom:** None that I could pin on it, but three sources disagree.
- **Evidence:**
  - The driver in this repo says GRBG (`ESP_CAM_SENSOR_BAYER_GRBG` in `ov02c10_settings.h`).
  - The original `main.c` header comment said BGGR.
  - The independent OV02C10 port by csvke for `esp-video-components` uses GBRG.
  - Mainline Linux reports `MEDIA_BUS_FMT_SGRBG10_1X10`, so GRBG, with the same mirror/flip register value (0x3820 = 0xa8).
- **Root cause:** Not known. On OmniVision sensors, mirror/flip settings and odd crop offsets move the Bayer phase, so two ports with different window settings can both be right for their own register tables. This table outputs 1920x1080 with window offsets of 8 (0x3811, 0x3813), where Linux outputs 1928x1092. Even offsets shouldn't change the phase, so GRBG is the best guess.
- **Fix:** None yet. With a wrong Bayer order, colours come out swapped (red and blue exchanged, or a green/magenta cast). I couldn't tell that apart from the uncalibrated grey cast in August.
- **How to settle it:** With the tuning file loaded, point the camera at something strongly red and something strongly blue. If they swap, try the other three orders in `bayer_type`. Better still, capture a RAW frame from the CSI node and look at the 2x2 pattern directly.
- **Status:** Open.

## Portrait panel, no hardware rotation

- **Symptom:** The sensor is landscape and the panel is portrait.
- **Evidence:** 1920x1080 against 800x1280. Neither the panel driver nor the DPI framebuffer path rotates the image for me.
- **Root cause:** Nothing is wrong; it's a mismatch in the hardware.
- **Fix:** A centre crop instead of rotation. I tried the PPA, the hardware way to rotate, and it didn't work. See [problem 3 in the lessons doc](LESSONS_AND_OPEN_PROBLEMS.md#3-ppa-90-degree-rotation-not-working).
- **Status:** Workaround in place. About 42% of the horizontal field of view is shown.

## Letterbox comment said 140 px

- **Symptom:** None at runtime. A misleading comment.
- **Evidence:** The original code said the black bars were 140 px. `LETTERBOX_TOP` is `(1280 - 1080) / 2 = 100`.
- **Root cause:** Left over from an earlier crop height.
- **Fix (found while preparing this repo):** The comment in `main/app_config.h` now says 100.
- **Status:** Fixed.

## OV02C10 missing from the static detect table

- **Symptom:** None with the default settings. A trap for anyone who changes them.
- **Evidence (found while preparing this repo):** `components/esp_cam_sensor/src/esp_cam_sensor_detect.c` is the one upstream source file I modified. It includes `ov02c10.h` and declares the detect function, but the OV02C10 is not in the `__esp_cam_sensor_detect_fn_array_start[]` table.
- **Root cause:** That table is only used when `CONFIG_CAMERA_SENSOR_MOTOR_DETECT_METHOD_STATIC_STORE` is selected. The default, `DYNAMIC_LINK`, finds detect functions through a linker section, which is how this driver gets found.
- **Fix:** None yet. Keep the default detect method, or add an `ESP_CAM_SENSOR_DETECT_ENTRY` line for the OV02C10 to the table.
- **Status:** Open, harmless with the defaults.

---

## What I'd do differently

- **Diagnostics first.** The I2C scan, the chip-ID readback and the V4L2 control count told me more in one boot than a week of guessing. I'd write them before any feature code.
- **Don't treat symptoms.** The x16 software gain cost me time because it made the picture "better" while hiding the real problem. If a fix makes things look better without explaining why, I now treat that as a warning sign.
- **Make the build fail loudly.** Two of the IPA problems were silent: a filename typo and an upstream CMake typo. A one-line check at boot ("zero V4L2 controls means no tuning file") would have caught both on the first run. The firmware now does that check.
- **Change one thing at a time.** For the watchdog I changed memory layout, ESP-IDF version, chip revision and timeout together. It works, but I can't say which change fixed it.
- **Keep logs.** I kept source and config but not every boot log. Several entries here rely on comments I wrote at the time instead of raw logs. Next time I'd save each boot log next to the commit that produced it.
- **Start from a supported sensor.** Copying the closest supported driver and swapping the register table is much faster than writing from scratch. See [problem 4 in the lessons doc](LESSONS_AND_OPEN_PROBLEMS.md#4-lesson-dont-hand-roll-low-level-bring-up).
- **Check licences on day one.** Taking the register table from a GPL driver decided the licence of the driver folder. I'd rather have known that before writing it. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
