#!/usr/bin/env bash
# Regenerates every host-simulation asset in docs/images/ and prints the
# measured numbers (also saved to tools/sim/out/results.txt).
#
#   tools/sim/make_all.sh                 all targets
#   tools/sim/make_all.sh lighting ema    only some
#
# Targets:
#   demo      demo.gif + pipeline_panels.png (runs make_demo.sh)
#   panels    pipeline_panels.gif   the 4 panels of the demo scene, animated
#   lighting  lighting_stress.gif   brightness steps and flicker, no object
#   absorb    stop_absorb.gif       object stops, is absorbed, then leaves
#   ema       ema_compare.gif       centroid_ema_shift 0 / 1 / 2 side by side
#   noise     numbers only          diff_threshold vs sensor noise
#
# Everything goes through motion_sim, i.e. the firmware's main/motion.c and
# main/overlay.c with MOTION_CFG_DEFAULT unless a target says otherwise.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

RESULTS="$OUT/results.txt"

# The lighting and absorb scenes run at 30 fps: the absorb timer is 120 frames,
# about 4 s at the ~30 fps the firmware aims for, so frame numbers and seconds
# line up with the docs. Their GIFs play at 2x speed to stay small.
FPS30=30
FAST_GIF="setpts=0.5*PTS,fps=12,scale=400:-2:flags=area,hqdn3d=7:7:25:25"

target_demo() {
    "$HERE/make_demo.sh"
    SIM_BUILT=1
    BG_RENDERED=1
}

target_panels() {
    build_motion_sim
    render_background
    echo "== pipeline_panels.gif: demo scene, all four panels, 2x2"
    local labels=""
    labels+="$(label 'input' 16 16 44)$(label '|Y - B| x4' 816 16 44)"
    labels+="$(label 'mask (red = removed by 3x3 filter)' 16 1096 44)$(label 'overlay' 816 1096 44)"
    labels+="$(label 'host simulation of firmware motion.c' '(w-tw)/2' 'h-th-20' 40)"
    render_scene "$DEMO_FPS" "$DEMO_SECONDS" "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" \
        "gte(t,$ENTER_S)" "$NOISE" |
        "$SIM" -w "$W" -h "$H" --panels-video --overlay-scale 2 |
        ff -f rawvideo -pix_fmt rgb24 -s "$((4 * W))x${H}" -framerate "$DEMO_FPS" -i - \
            -filter_complex "[0:v]split=4[a][b][c][d];
[a]crop=$W:$H:0:0[p0];[b]crop=$W:$H:$W:0[p1];[c]crop=$W:$H:$((2 * W)):0[p2];[d]crop=$W:$H:$((3 * W)):0[p3];
[p0][p1][p2][p3]xstack=inputs=4:layout=0_0|w0_0|0_h0|w0_h0${labels},
fps=10,scale=400:-2:flags=area,hqdn3d=7:7:25:25,$GIF_PALETTE_SMALL" \
            "$IMG/pipeline_panels.gif"
}

target_lighting() {
    build_motion_sim
    render_background
    echo "== lighting_stress.gif: no object; brightness step at 1 s and 3 s, 2 Hz flicker 9-12 s"
    local secs=14
    # eq's brightness moves in coarse steps; 0.04 and +0.09 come out as about +8 and +28 luma.
    local bright="0.04*gte(t,1)+0.09*gte(t,3)+if(between(t,9,12),0.08*sin(2*PI*2*(t-9)),0)"
    write_truth "$OUT/lighting_truth.csv" "$FPS30" "$secs" 0 0 0
    local labels=""
    labels+="$(label 'steady scene' 16 16 40 'lt(t,1)')"
    labels+="$(label 'small brightness step' 16 16 40 'between(t,1,2.999)')"
    labels+="$(label 'large brightness step' 16 16 40 'between(t,3,8.999)')"
    labels+="$(label 'flicker (2 Hz brightness wave)' 16 16 40 'between(t,9,11.999)')"
    labels+="$(label 'steady again' 16 16 40 'gte(t,12)')"
    labels+="$(label 'frame %{frame_num}' 16 80 40)"
    labels+="$(label 'host simulation - no object - 2x speed' '(w-tw)/2' 'h-th-20' 36)"
    render_scene "$FPS30" "$secs" 0 0 0 "$NOISE" ",format=yuv444p,eq=brightness='$bright':eval=frame" |
        "$SIM" -w "$W" -h "$H" -l "$OUT/lighting.csv" --truth "$OUT/lighting_truth.csv" |
        ff -f rawvideo -pix_fmt rgb565le -s "${W}x${H}" -framerate "$FPS30" -i - \
            -vf "format=rgb24${labels},$FAST_GIF,$GIF_PALETTE_SMALL" "$IMG/lighting_stress.gif"
    segment_stats "$OUT/lighting.csv" "steady 0-1 s:0:29" "small step 1-3 s:30:89" \
        "large step 3-9 s:90:269" "flicker 9-12 s:270:359" "steady again 12-14 s:360:419"
    awk -F, 'NR > 1 && ($1 == 29 || $1 == 89 || $1 == 269) { printf "  mean luma at frame %d: %s\n", $1, $11 }' \
        "$OUT/lighting.csv"
    # Same scene up to 9 s with a slightly smaller second step (+0.08); numbers only.
    echo "variant, second step +0.08 instead of +0.09:"
    render_scene "$FPS30" 9 0 0 0 "$NOISE" \
        ",format=yuv444p,eq=brightness='0.04*gte(t,1)+0.08*gte(t,3)':eval=frame" |
        "$SIM" -w "$W" -h "$H" -l "$OUT/lighting_small.csv" >/dev/null
    segment_stats "$OUT/lighting_small.csv" "large step 3-9 s:90:269"
    awk -F, 'NR > 1 && ($1 == 89 || $1 == 269) { printf "  mean luma at frame %d: %s\n", $1, $11 }' \
        "$OUT/lighting_small.csv"
}

target_absorb() {
    build_motion_sim
    render_background
    echo "== stop_absorb.gif: object enters, stops at 2 s, leaves at 8 s"
    local secs=14 y=470
    # x of the top-left corner: slide in over 0.5-2 s, stand still until 8 s, slide out by 9.5 s.
    local fx="if(lt(t,2),-$OBJ+($((330 + OBJ)))*(t-0.5)/1.5,if(lt(t,8),330,330+620*(t-8)/1.5))"
    local ax="(t < 2 ? -$OBJ + $((330 + OBJ)) * (t - 0.5) / 1.5 : (t < 8 ? 330 : 330 + 620 * (t - 8) / 1.5))"
    write_truth "$OUT/absorb_truth.csv" "$FPS30" "$secs" "$ax" "$y" "t >= 0.5 && $ax < $W"
    local labels=""
    labels+="$(label 'moving in' 16 16 40 'lt(t,2)')"
    labels+="$(label 'stopped (absorbed after 120 frames)' 16 16 40 'between(t,2,7.999)')"
    labels+="$(label 'leaving - old spot now differs' 16 16 40 'between(t,8,9.499)')"
    labels+="$(label 'object gone - ghost at old spot' 16 16 40 'gte(t,9.5)')"
    labels+="$(label 'frame %{frame_num}' 16 80 40)"
    labels+="$(label 'host simulation - 2x speed' '(w-tw)/2' 'h-th-20' 36)"
    render_scene "$FPS30" "$secs" "$fx" "$y" "gte(t,0.5)" "$NOISE" |
        "$SIM" -w "$W" -h "$H" -l "$OUT/absorb.csv" --truth "$OUT/absorb_truth.csv" |
        ff -f rawvideo -pix_fmt rgb565le -s "${W}x${H}" -framerate "$FPS30" -i - \
            -vf "format=rgb24${labels},$FAST_GIF,$GIF_PALETTE_SMALL" "$IMG/stop_absorb.gif"
    segment_stats "$OUT/absorb.csv" "empty 0-0.5 s:0:14" "moving in 0.5-2 s:15:59" \
        "stopped 2-8 s:60:239" "leaving 8-9.5 s:240:284" "gone 9.5-14 s:285:419"
}

target_ema() {
    build_motion_sim
    render_background
    echo "== ema_compare.gif: demo scene, centroid_ema_shift 0 / 1 / 2, white cross = true centre"
    write_truth "$OUT/demo_truth.csv" "$DEMO_FPS" "$DEMO_SECONDS" \
        "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" "t >= $ENTER_S"
    local s noise
    for s in 0 1 2; do
        echo "centroid_ema_shift $s, noise $NOISE (demo scene):"
        render_scene "$DEMO_FPS" "$DEMO_SECONDS" "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" \
            "gte(t,$ENTER_S)" "$NOISE" |
            "$SIM" -w "$W" -h "$H" -e "$s" --truth "$OUT/demo_truth.csv" --show-truth --overlay-scale 3 |
            ff -f rawvideo -pix_fmt rgb565le -s "${W}x${H}" -framerate "$DEMO_FPS" -i - \
                -vf "format=rgb24$(label "EMA shift $s" 24 24 72),scale=200:270:flags=area" \
                -f rawvideo -pix_fmt rgb24 "$OUT/ema_$s.rgb24"
    done
    ff -f rawvideo -pix_fmt rgb24 -s 200x270 -framerate "$DEMO_FPS" -i "$OUT/ema_0.rgb24" \
        -f rawvideo -pix_fmt rgb24 -s 200x270 -framerate "$DEMO_FPS" -i "$OUT/ema_1.rgb24" \
        -f rawvideo -pix_fmt rgb24 -s 200x270 -framerate "$DEMO_FPS" -i "$OUT/ema_2.rgb24" \
        -filter_complex "[0:v][1:v][2:v]hstack=inputs=3,hqdn3d=7:7:25:25$(label 'host simulation - white cross = true centre' '(w-tw)/2' 'h-th-6' 13),
fps=10,$GIF_PALETTE_SMALL" \
        "$IMG/ema_compare.gif"
    rm -f "$OUT"/ema_?.rgb24
    # Same comparison with more sensor noise; numbers only. At 14 stray noise
    # clusters start to pull the centroid (see the noise target).
    for noise in 12 14; do
        for s in 0 1 2; do
            echo "centroid_ema_shift $s, noise $noise:"
            render_scene "$DEMO_FPS" "$DEMO_SECONDS" "$(demo_px "$ENTER_S")" "$(demo_py "$ENTER_S")" \
                "gte(t,$ENTER_S)" "$noise" |
                "$SIM" -w "$W" -h "$H" -e "$s" --truth "$OUT/demo_truth.csv" >/dev/null
        done
    done
}

target_noise() {
    build_motion_sim
    render_background
    echo "== noise sweep: 10 s empty scene, then the demo object for 7 s (15 fps)"
    local enter=10 secs=17 noise thr log
    write_truth "$OUT/noise_truth.csv" "$DEMO_FPS" "$secs" \
        "$(demo_px "$enter")" "$(demo_py "$enter")" "t >= $enter"
    echo "  noise  mean|Y-B|  threshold  false-positive frames (of 150 empty)  missed (of 105)  mean error px"
    for noise in 6 12 14 20 30; do
        for thr in 12 18 24; do
            log="$OUT/noise_${noise}_${thr}.csv"
            local stats
            stats="$(render_scene "$DEMO_FPS" "$secs" "$(demo_px "$enter")" "$(demo_py "$enter")" \
                "gte(t,$enter)" "$noise" |
                "$SIM" -w "$W" -h "$H" -t "$thr" -l "$log" --truth "$OUT/noise_truth.csv" 2>&1 >/dev/null)"
            awk -F, -v noise="$noise" -v thr="$thr" -v stats="$stats" '
                NR > 1 && $1 >= 5 && $1 < 150 { s += $12; n++ }
                END {
                    match(stats, /[0-9]+ missed/); missed = substr(stats, RSTART, RLENGTH - 7)
                    match(stats, /[0-9]+ false positive/); fp = substr(stats, RSTART, RLENGTH - 15)
                    err = "-"
                    if (match(stats, /mean error [0-9.]+/)) err = substr(stats, RSTART + 11, RLENGTH - 11)
                    printf "  %5d  %9.1f  %9d  %37d  %15d  %13s\n", noise, s / n, thr, fp, missed, err
                }' "$log"
        done
    done
}

main() {
    local targets=("$@")
    [ ${#targets[@]} -eq 0 ] && targets=(demo panels lighting absorb ema noise)
    for t in "${targets[@]}"; do
        declare -F "target_$t" >/dev/null || { echo "unknown target: $t" >&2; exit 2; }
    done
    for t in "${targets[@]}"; do "target_$t"; done
    ls -l "$IMG"
}

main "$@" 2>&1 | tee "$RESULTS"
