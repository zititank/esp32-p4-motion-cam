# Host simulation

Everything here runs the firmware's own `main/motion.c` and `main/overlay.c` on a PC, on synthetic video made with ffmpeg. None of it is a camera recording. It shows what the detector code does on scenes where the ground truth is known, not how the board behaves.

## Regenerate

Needs `gcc`, `ffmpeg` and `awk`. Text labels need a font `fc-match` can find; without one the GIFs have no text.

```bash
tools/sim/make_all.sh               # everything, about 80 s
tools/sim/make_all.sh lighting ema  # some targets
tools/sim/make_demo.sh              # only demo.gif + pipeline_panels.png
```

Outputs go to `docs/images/`; intermediates and the measured numbers go to `tools/sim/out/` (`results.txt`, one CSV per run). The runs are deterministic: the same ffmpeg build gives byte-identical GIFs and the same numbers. A different ffmpeg version may shift the numbers slightly, because the noise and scaling filters can change between versions.

| Target | Output | Scene |
|---|---|---|
| `demo` | `demo.gif`, `pipeline_panels.png` | 15 fps, 8 s: one round object on a slow Lissajous path, entering after 1 s |
| `panels` | `pipeline_panels.gif` | the demo scene as a 2x2 view: input, $\lvert Y - B\rvert$ x4, mask, overlay |
| `lighting` | `lighting_stress.gif` | 30 fps, 14 s, no object: +8 luma step at 1 s, +28 luma step at 3 s, 2 Hz brightness wave 9-12 s |
| `absorb` | `stop_absorb.gif` | 30 fps, 14 s: object slides in, stands still from 2 s to 8 s, slides out |
| `ema` | `ema_compare.gif` | the demo scene three times, `centroid_ema_shift` 0 / 1 / 2, white cross = true centre |
| `noise` | numbers only | 10 s empty scene then the demo object, 5 noise levels x 3 thresholds |

The lighting and absorb scenes run at 30 fps so that 120 frames (`MOTION_FG_ABSORB_FRAMES`) are about 4 s, as in the firmware docs. Their GIFs play at 2x speed; the frame counter in the corner is the frame number `motion.c` saw.

The GIFs are display copies: downscaled, 10-12 fps and denoised after the overlay is drawn. `motion_sim` always sees the full-resolution, full-rate, noisy frames.

Results and what they mean: [docs/ALGORITHM.md, Simulation results](../../docs/ALGORITHM.md#simulation-results).

## Files

- `lib.sh`: shared scene definition (background, object, noise), ground-truth writer, labels.
- `make_demo.sh`: the README hero GIF and the one-frame panel PNG.
- `make_all.sh`: all targets.
- `motion_sim.c`: reads raw RGB565 frames on stdin, runs `motion_process()` and the overlay, writes annotated frames on stdout.

## motion_sim

```text
motion_sim -w W -h H [options] < in.rgb565 > out
  -p N -o FILE          4-panel PPM of frame N
  -l FILE               per-frame CSV: frame, active, box, centroid, kept cells,
                        cells above threshold, mean luma, mean |Y - B|
  -t, --threshold T     diff_threshold
  -e, --ema-shift S     centroid_ema_shift
  --panels-video        write the 4-panel view of every frame (RGB24, 4W x H) instead
  --truth FILE          "frame,x,y" true object centre per frame (x < 0: none);
                        prints detections, misses, false positives, centroid error,
                        best-fit lag and jitter
  --show-truth          draw the true centre as a white cross
  --overlay-scale K     thicker overlay lines for small GIFs (drawing only)
```

Everything not listed uses `MOTION_CFG_DEFAULT` from `main/motion.h`. For a quick experiment on the demo scene, pass flags through `SIM_ARGS`, e.g. `SIM_ARGS="-e 1" tools/sim/make_demo.sh` (this rewrites `docs/images/`; run it again without `SIM_ARGS` before committing).

Metrics printed with `--truth`:

- **mean / worst error**: distance between the smoothed centroid and the true centre, over frames where motion is active and the object is in view.
- **best-fit lag**: the delay k (0-15 frames) for which the centroid best matches where the object was k frames earlier. The px value next to it is the error left at that delay.
- **jitter**: mean length of the second difference of (centroid - true centre). The true path is smooth, so this is the frame-to-frame wobble.
