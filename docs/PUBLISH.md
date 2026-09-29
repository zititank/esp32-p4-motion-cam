# How this repo was published

My working copy lives inside another git repository, so this repo was made from an exported copy instead of running `git init` in place. This note records how, so it can be repeated.

## Export

Copy the project to a clean directory and leave out everything `.gitignore` already covers (build output, downloaded components, generated config, simulation intermediates):

```bash
EXPORT=~/src/esp32-p4-motion-cam
rsync -a --exclude .git --exclude build --exclude managed_components --exclude sdkconfig \
  --exclude sdkconfig.old --exclude dependencies.lock --exclude .cache --exclude err \
  --exclude test/host/build --exclude tools/sim/out \
  <path-to-working-copy>/ "$EXPORT/"
cd "$EXPORT" && git init -b main
```

## Commit history

Four commits, with real dates and no backdating, so the history shows what was the original bring-up and what came later:

1. `import original Aug 2026 bring-up snapshot`: the original bring-up tree unchanged (`main/main.c`, the vendored `esp_cam_sensor` with the OV02C10 driver, `CMakeLists.txt`, `sdkconfig.defaults`), without build output, `sdkconfig` or logs. It doesn't build as it is; commit 2 fixes that.
2. `fix build (Kconfig, IPA filename, IPA JSON fields)`: build fixes only.
3. `split into modules + add motion tracker and tests`: the `app_main` / camera / display / diagnostics split, together with `motion.c`, `overlay.c`, the host tests and the simulation. They're one commit because the split `app_main.c` and `main/CMakeLists.txt` already reference the motion code.
4. `docs + CI`: README, `docs/`, `THIRD_PARTY_NOTICES`, `AI_USAGE`, `.github/workflows`, the simulation images.

From commit 2 on, every commit builds.

## Create the GitHub repo

```bash
gh auth status
gh auth setup-git
gh repo create esp32-p4-motion-cam --public --description "..."
cd "$EXPORT"
git remote add origin https://github.com/<account>/esp32-p4-motion-cam.git
git push -u origin main
gh repo edit <account>/esp32-p4-motion-cam --add-topic esp32-p4,esp-idf,mipi-csi,mipi-dsi,ov02c10,jd9365,motion-detection,embedded-vision,firmware
```

The README badge and clone URL point at `github.com/<account>/esp32-p4-motion-cam`; if you fork or re-host it, change them. For the social preview, upload `docs/images/pipeline_panels.png` under Settings → General → Social preview (GitHub's limit is 1 MB, so the GIFs are too big).

## Binaries and licensing

Don't redistribute a built `.bin` without reading [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md#distributing-binaries). A firmware image links:

- Espressif code under Apache-2.0 and the **ESPRESSIF MIT** licence (`esp_video`, `esp_ipa`; `esp_ipa` also ships **prebuilt** libraries),
- the OV02C10 driver folder under **GPL-2.0-only** (register table derived from Intel's Linux driver).

Publishing the source is fine. Shipping a binary is a separate licence problem, so CI only builds and never uploads the image.

## Still to do

- When the board is available again: flash, replace the simulation GIF with a real recording, and add a real boot log to `docs/`.
