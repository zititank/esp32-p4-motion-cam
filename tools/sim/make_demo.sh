#!/usr/bin/env bash
# Synthesises a reproducible 800x1080 scene with ffmpeg (static textured
# background, sensor-like temporal noise, one moving object), runs it through
# the firmware's main/motion.c + main/overlay.c via motion_sim, and writes
#   docs/images/demo.gif             annotated output, downscaled
#   docs/images/pipeline_panels.png  input | |Y - B| | mask | overlay at one frame
# Intermediates go to tools/sim/out/. The other simulations are in make_all.sh.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

PANEL_FRAME=66       # frame used for the 4-panel figure
GIF_W=400
# Extra motion_sim flags for experiments, e.g. SIM_ARGS="-e 0" to disable centroid smoothing.
SIM_ARGS="${SIM_ARGS:-}"

build_motion_sim
render_background
write_truth "$OUT/demo_truth.csv" "$DEMO_FPS" "$DEMO_SECONDS" \
    "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" "t >= $ENTER_S"

CAPTION=""
PANEL_LABELS=""
if [ -n "$FONT" ]; then
    CAPTION=",drawtext=fontfile='$FONT':text='host simulation of firmware motion.c':fontsize=16:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=6:x=(w-tw)/2:y=h-th-12"
    L="fontfile='$FONT':fontsize=22:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=6:y=10"
    PANEL_LABELS=",drawtext=$L:x=10:text='input',drawtext=$L:x=410:text='|Y - B| x$((4))',drawtext=$L:x=810:text='mask (red\: removed by 3x3)',drawtext=$L:x=1210:text='overlay'"
fi

# The GIF is only a display copy: 10 fps and a denoise pass on the already
# annotated frames keep it small. motion_sim saw the full-rate noisy input.
echo "rendering scene, running motion_sim, writing demo.gif"
render_scene "$DEMO_FPS" "$DEMO_SECONDS" "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" \
    "gte(t,$ENTER_S)" "$NOISE" |
    "$SIM" $SIM_ARGS -w "$W" -h "$H" -p "$PANEL_FRAME" -o "$OUT/panels.ppm" -l "$OUT/motion.csv" \
        --truth "$OUT/demo_truth.csv" |
    ff -f rawvideo -pix_fmt rgb565le -s "${W}x${H}" -framerate "$DEMO_FPS" -i - \
        -vf "fps=10,scale=${GIF_W}:-2:flags=area,hqdn3d=7:7:25:25${CAPTION},$GIF_PALETTE" \
        "$IMG/demo.gif"

echo "writing pipeline_panels.png"
ff -i "$OUT/panels.ppm" -vf "scale=1600:-2:flags=area${PANEL_LABELS}" "$IMG/pipeline_panels.png"

ls -l "$IMG/demo.gif" "$IMG/pipeline_panels.png"
