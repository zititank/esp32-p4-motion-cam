# AI usage

I used AI tools a lot on this project. This file says where, so you can judge the code with that in mind.

## Tools

Cursor, with Claude-family models and some other models, used as a coding assistant in the editor.

## What the AI did

- Drafted most of the code, both during the August 2026 bring-up and afterwards.
- Wrote the diagnostics scaffolding: the I2C scan, V4L2 format and control enumeration, and heap reports.
- Helped me read boot logs and suggested possible causes.
- Did the September 2026 refactor: splitting `main.c` into modules, the build fixes, the motion detector and overlay, the host tests, the simulation and the CI workflow.
- Drafted this documentation, which I then edited.

## What I did

- Chose the board and the camera, and did the physical bring-up.
- Built, flashed and ran every version that went onto the board, and read the boot logs and the screen.
- Decided which suggested fixes to try, and threw out the ones that didn't match what the hardware did. The x16 software gain is an example of a fix that "worked" and that I later removed.
- Reviewed the code and docs before publishing, and decided what goes in this repo and how it's licensed.

## How it was checked

- **On the board (August 2026):** the single-file firmware in `docs/history/main_original_aug2026.c`. The live image reached the panel, uncalibrated.
- **Host unit tests:** `test/host/` runs the motion detector and overlay code against synthetic frames: static scene, noise below the threshold, a moving square, the square leaving, a stopped object, API guards and overlay clipping. There's also an AddressSanitizer build.
- **Host simulation:** `tools/sim/` runs the same `motion.c` on synthetic videos and produces the GIFs and the panel image in the README, plus the measured numbers in [docs/ALGORITHM.md](docs/ALGORITHM.md#simulation-results). These say what the code does on made-up scenes, not how the board behaves.
- **Local firmware build:** ESP-IDF v5.4.4 for the ESP32-P4, zero warnings, with motion on and off.
- **CI:** the GitHub Actions workflow runs the same firmware build and the host tests. It hasn't run on GitHub yet.

## What is not verified

- Nothing written after August 2026 has run on the board yet. That includes the module split, the IPA tuning file being enabled, the auto-exposure values borrowed from Espressif's SC2336 file, the motion detector, the overlay and the heap report.
- Motion timing and frame rate on the ESP32-P4 are unmeasured.
- The watchdog hypothesis and the timing-field question in [docs/LESSONS_AND_OPEN_PROBLEMS.md](docs/LESSONS_AND_OPEN_PROBLEMS.md) come from reading code, not from measurements.

If something in this repo looks wrong, it may well be. Please open an issue.
