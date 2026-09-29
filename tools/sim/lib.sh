# Shared by make_demo.sh and make_all.sh. Source it, don't run it.
#
# Every scene is the same static textured background, optionally one round
# object, a per-scene filter (e.g. a brightness change), and sensor-like
# temporal noise, rendered by ffmpeg as raw RGB565 and fed to motion_sim, which
# runs the firmware's main/motion.c and main/overlay.c unchanged.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"
IMG="$ROOT/docs/images"
mkdir -p "$OUT" "$IMG"

command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
CC="${CC:-gcc}"
SIM="$OUT/motion_sim"

W=800
H=1080
OBJ=140              # object diameter in pixels
R=$((OBJ / 2))
NOISE=6              # ffmpeg noise strength used by the demo scene

# Demo scene: object top-left corner as a function of t (seconds). Kept in one
# place so the ffmpeg expression and the ground truth use the same path.
DEMO_FPS=15
DEMO_SECONDS=8
ENTER_S=1            # the object appears after this many seconds of empty scene
X0=330; AX=280; FX=0.08
Y0=470; AY=380; FY=0.06
demo_px() { echo "$X0+$AX*sin(2*PI*$FX*(t-$1))"; }
demo_py() { echo "$Y0+$AY*sin(2*PI*$FY*(t-$1))"; }

FONT="$(fc-match -f '%{file}' 'DejaVu Sans:style=Bold' 2>/dev/null || true)"
if [ -z "$FONT" ] || [ ! -f "$FONT" ]; then
    FONT=""
    echo "no font found for drawtext; GIFs get no text labels" >&2
fi

GIF_PALETTE="split[a][b];
[a]palettegen=max_colors=64:stats_mode=diff[p];
[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle"
# Used by the make_all.sh GIFs: no dither and fewer colours keep them smaller.
GIF_PALETTE_SMALL="split[a][b];
[a]palettegen=max_colors=48:stats_mode=diff[p];
[b][p]paletteuse=dither=none:diff_mode=rectangle"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

build_motion_sim() {
    [ -n "${SIM_BUILT:-}" ] && return
    echo "building motion_sim"
    "$CC" -std=c99 -O2 -Wall -Wextra -Werror -I"$ROOT/main" \
        -o "$SIM" "$HERE/motion_sim.c" "$ROOT/main/motion.c" "$ROOT/main/overlay.c" -lm
    SIM_BUILT=1
}

render_background() {
    [ -n "${BG_RENDERED:-}" ] && return
    echo "rendering background"
    ff -f lavfi \
        -i "color=c=0x506070:s=${W}x${H}:d=1,noise=alls=90:allf=u,gblur=sigma=7,eq=contrast=2.2:saturation=1.6,
drawbox=x=40:y=110:w=240:h=330:color=0x8a6a4a:t=fill,
drawbox=x=60:y=130:w=200:h=120:color=0xc8b890:t=fill,
drawbox=x=500:y=640:w=240:h=360:color=0x3f5f45:t=fill,
drawbox=x=0:y=1000:w=${W}:h=80:color=0x2a2a30:t=fill,
drawgrid=w=100:h=100:t=1:c=white@0.12" \
        -frames:v 1 "$OUT/background.png"
    BG_RENDERED=1
}

# render_scene FPS SECONDS X_EXPR Y_EXPR ENABLE_EXPR NOISE [FILTER]
# Writes raw RGB565 frames to stdout. FILTER (starting with a comma) runs on the
# composited scene before the noise, e.g. a brightness change.
render_scene() {
    local fps=$1 secs=$2 px=$3 py=$4 enable=$5 noise=$6 extra=${7:-}
    ff -loop 1 -framerate "$fps" -t "$secs" -i "$OUT/background.png" \
        -f lavfi -i "color=c=0xe8b040:s=${OBJ}x${OBJ}:r=${fps}:d=${secs},format=rgba,
geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='if(lte(hypot(X-$R,Y-$R),$((R - 2))),255,0)'" \
        -filter_complex "[0:v]format=rgba[bg];
[bg][1:v]overlay=x='$px':y='$py':enable='$enable':eval=frame${extra},
noise=alls=$noise:allf=t,format=rgb565le" \
        -f rawvideo -pix_fmt rgb565le -
}

# write_truth FILE FPS SECONDS AWK_X AWK_Y AWK_PRESENT
# One "frame,x,y" line per frame with the true object centre, or -1,-1 when the
# object isn't in view. The AWK_* expressions use t (seconds) and mirror the
# ffmpeg expressions; overlay truncates positions to whole pixels, so does int().
write_truth() {
    awk -v fps="$2" -v secs="$3" -v r="$R" -v w="$W" -v PI=3.14159265358979 "
        BEGIN {
            for (f = 0; f < fps * secs; f++) {
                t = f / fps
                if ($6) printf \"%d,%d,%d\\n\", f, int($4) + r, int($5) + r
                else printf \"%d,-1,-1\\n\", f
            }
        }" > "$1"
}

# label TEXT X Y SIZE [ENABLE_EXPR] -> ",drawtext=..." or nothing without a font.
# Avoid ':' ',' and quotes in TEXT.
label() {
    [ -n "$FONT" ] || return 0
    local en=""
    [ -n "${5:-}" ] && en=":enable='$5'"
    printf ",drawtext=fontfile='%s':text='%s':fontsize=%s:fontcolor=white:box=1:boxcolor=black@0.65:boxborderw=8:x=%s:y=%s%s" \
        "$FONT" "$1" "$4" "$2" "$3" "$en"
}

# segment_stats CSV "name:first_frame:last_frame" ...
# Active frames per segment of a motion_sim log, with the first and last active frame.
segment_stats() {
    local csv=$1 seg name s e
    shift
    for seg in "$@"; do
        IFS=: read -r name s e <<<"$seg"
        awk -F, -v s="$s" -v e="$e" -v name="$name" '
            NR > 1 && $1 >= s && $1 <= e {
                n++; if ($12 > peak) peak = $12
                if ($2) { a++; last = $1; if (first == "") first = $1 }
            }
            END {
                printf "  %-34s frames %3d-%3d: %3d of %3d active", name, s, e, a, n
                if (a) printf " (first %d, last %d)", first, last
                printf ", peak mean |Y-B| %.1f\n", peak
            }' "$csv"
    done
}
