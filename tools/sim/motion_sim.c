/*
 * Host simulation of the firmware motion path.
 *
 * Reads raw little-endian RGB565 frames (W x H) from stdin, runs main/motion.c
 * over the whole frame (the firmware runs it over the 800x1080 crop), draws the
 * overlay with main/overlay.c using the same style as main/app_main.c, and
 * writes the annotated frames to stdout in the same format.
 *
 *   motion_sim -w 800 -h 1080 [options] < in.rgb565 > out
 *
 *   -p N -o FILE          4-panel PPM of frame N: input | |Y - B| | mask | overlay
 *   -l FILE               one CSV line per frame with the motion result
 *   -t, --threshold T     diff_threshold      (default: MOTION_CFG_DEFAULT)
 *   -e, --ema-shift S     centroid_ema_shift  (default: MOTION_CFG_DEFAULT)
 *   --panels-video        write the 4-panel view of every frame to stdout instead,
 *                         as raw RGB24 frames of 4W x H
 *   --truth FILE          CSV "frame,x,y" with the true object centre (x < 0: no
 *                         object); prints detection counts, centroid error, lag
 *                         and jitter
 *   --show-truth          draw the true centre as a small white cross
 *   --overlay-scale K     multiply overlay line width and crosshair size by K,
 *                         for small GIFs; display only, detection is unchanged
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion.h"
#include "overlay.h"

/* Same values as main/app_main.c. */
#define LINE_PX    3
#define CROSS_HALF 16

/* Display gain for the difference panel so values near the threshold are visible. */
#define DIFF_GAIN 4

#define OVERLAY_WHITE 0xFFFF
#define TRUTH_HALF    10
#define MAX_LAG       15

static void usage(void)
{
    fprintf(stderr, "usage: motion_sim -w W -h H [-p frame -o panels.ppm] [-l log.csv]\n"
                    "                  [-t|--threshold T] [-e|--ema-shift S] [--panels-video]\n"
                    "                  [--truth truth.csv] [--show-truth] [--overlay-scale K]\n");
    exit(2);
}

static void rgb565_to_rgb888(uint16_t p, uint8_t *o)
{
    uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    o[0] = (uint8_t)((r << 3) | (r >> 2));
    o[1] = (uint8_t)((g << 2) | (g >> 4));
    o[2] = (uint8_t)((b << 3) | (b >> 2));
}

static void panel_put(uint8_t *rgb, int pw, int panel, int w, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b)
{
    uint8_t *o = rgb + ((size_t)y * pw + (size_t)panel * w + x) * 3;
    o[0] = r;
    o[1] = g;
    o[2] = b;
}

/* Fills rgb (4w x h RGB24) with input | |Y - B| | mask | overlay. */
static void render_panels(uint8_t *rgb, const motion_ctx_t *ctx, int cell, const uint16_t *in,
                          const uint16_t *out, int w, int h)
{
    const int pw = 4 * w;
    const int gw = motion_grid_w(ctx), gh = motion_grid_h(ctx);
    const uint8_t *diff = motion_diff(ctx);
    const uint8_t *raw = motion_mask_raw(ctx);
    const uint8_t *mask = motion_mask(ctx);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t c[3];
            rgb565_to_rgb888(in[(size_t)y * w + x], c);
            panel_put(rgb, pw, 0, w, x, y, c[0], c[1], c[2]);
            rgb565_to_rgb888(out[(size_t)y * w + x], c);
            panel_put(rgb, pw, 3, w, x, y, c[0], c[1], c[2]);

            int gx = x / cell, gy = y / cell;
            uint8_t d = 0, m = 0;
            if (gx < gw && gy < gh) {
                size_t i = (size_t)gy * gw + gx;
                int v = diff[i] * DIFF_GAIN;
                d = (uint8_t)(v > 255 ? 255 : v);
                m = mask[i] ? 255 : (raw[i] ? 90 : 0);
            }
            panel_put(rgb, pw, 1, w, x, y, d, d, d);
            /* white: kept by the 3x3 filter, dark red: above threshold but removed */
            if (m == 90) {
                panel_put(rgb, pw, 2, w, x, y, 140, 30, 30);
            } else {
                panel_put(rgb, pw, 2, w, x, y, m, m, m);
            }
        }
    }
}

static int write_ppm(const char *path, const uint8_t *rgb, int pw, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", pw, h);
    size_t ok = fwrite(rgb, 3, (size_t)pw * h, f);
    fclose(f);
    return ok == (size_t)pw * h ? 0 : -1;
}

typedef struct {
    int n;
    int *x, *y; /* x < 0: no object in that frame */
} truth_t;

static int truth_load(truth_t *t, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int cap = 0, fr, x, y;
    char line[128];
    memset(t, 0, sizeof(*t));
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%d,%d,%d", &fr, &x, &y) != 3 || fr < 0) continue;
        if (fr >= cap) {
            int ncap = cap ? cap : 256;
            while (fr >= ncap) ncap *= 2;
            int *nx = realloc(t->x, (size_t)ncap * sizeof(int));
            if (!nx) break;
            t->x = nx;
            int *ny = realloc(t->y, (size_t)ncap * sizeof(int));
            if (!ny) break;
            t->y = ny;
            for (int i = cap; i < ncap; i++) t->x[i] = -1;
            cap = ncap;
        }
        t->x[fr] = x;
        t->y[fr] = y;
        if (fr + 1 > t->n) t->n = fr + 1;
    }
    fclose(f);
    return t->n > 0 ? 0 : -1;
}

static int truth_at(const truth_t *t, int frame, int *x, int *y)
{
    if (frame < 0 || frame >= t->n || t->x[frame] < 0) return 0;
    *x = t->x[frame];
    *y = t->y[frame];
    return 1;
}

/* Detection counts, centroid error against the true centre, the frame delay that
 * best explains the error (lag), and the mean |second difference| of the error
 * (jitter; the smooth true path cancels out, so this is the frame-to-frame wobble). */
static void truth_report(const truth_t *t, const int *act, const int *cx, const int *cy, int frames)
{
    int with_obj = 0, hit = 0, missed = 0, fp = 0;
    double sum = 0, worst = 0;
    for (int f = 0; f < frames; f++) {
        int tx, ty;
        int present = truth_at(t, f, &tx, &ty);
        if (!present) {
            fp += act[f];
            continue;
        }
        with_obj++;
        if (!act[f]) {
            missed++;
            continue;
        }
        hit++;
        double e = hypot(cx[f] - tx, cy[f] - ty);
        sum += e;
        if (e > worst) worst = e;
    }
    fprintf(stderr, "truth: %d frames with object, %d active, %d missed, %d false positive\n",
            with_obj, hit, missed, fp);
    if (!hit) return;

    int best_k = 0;
    double best = -1;
    for (int k = 0; k <= MAX_LAG; k++) {
        double s = 0;
        int n = 0;
        for (int f = 0; f < frames; f++) {
            int tx, ty;
            if (!act[f] || !truth_at(t, f, &tx, &ty) || !truth_at(t, f - k, &tx, &ty)) continue;
            s += hypot(cx[f] - tx, cy[f] - ty);
            n++;
        }
        if (n && (best < 0 || s / n < best)) {
            best = s / n;
            best_k = k;
        }
    }

    double jsum = 0;
    int jn = 0;
    for (int f = 1; f + 1 < frames; f++) {
        int ax, ay, bx, by, qx, qy;
        if (!act[f - 1] || !act[f] || !act[f + 1] || !truth_at(t, f - 1, &ax, &ay) ||
            !truth_at(t, f, &bx, &by) || !truth_at(t, f + 1, &qx, &qy)) {
            continue;
        }
        double ex = (cx[f + 1] - qx) - 2.0 * (cx[f] - bx) + (cx[f - 1] - ax);
        double ey = (cy[f + 1] - qy) - 2.0 * (cy[f] - by) + (cy[f - 1] - ay);
        jsum += hypot(ex, ey);
        jn++;
    }
    fprintf(stderr,
            "centroid: mean error %.1f px, worst %.1f px, best-fit lag %d frames "
            "(%.1f px left at that lag), jitter %.2f px\n",
            sum / hit, worst, best_k, best, jn ? jsum / jn : 0.0);
}

static int push_int(int **a, int *cap, int i, int v)
{
    if (i >= *cap) {
        int ncap = *cap ? *cap * 2 : 256;
        int *n = realloc(*a, (size_t)ncap * sizeof(int));
        if (!n) return -1;
        *a = n;
        *cap = ncap;
    }
    (*a)[i] = v;
    return 0;
}

int main(int argc, char **argv)
{
    int w = 0, h = 0, panel_frame = -1, threshold = -1, ema_shift = -1, scale = 1;
    int panels_video = 0, show_truth = 0;
    const char *panel_path = NULL, *log_path = NULL, *truth_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--panels-video")) {
            panels_video = 1;
            continue;
        }
        if (!strcmp(a, "--show-truth")) {
            show_truth = 1;
            continue;
        }
        if (i + 1 >= argc) usage();
        const char *v = argv[++i];
        if (!strcmp(a, "-w")) w = atoi(v);
        else if (!strcmp(a, "-h")) h = atoi(v);
        else if (!strcmp(a, "-p")) panel_frame = atoi(v);
        else if (!strcmp(a, "-o")) panel_path = v;
        else if (!strcmp(a, "-l")) log_path = v;
        else if (!strcmp(a, "-t") || !strcmp(a, "--threshold")) threshold = atoi(v);
        else if (!strcmp(a, "-e") || !strcmp(a, "--ema-shift")) ema_shift = atoi(v);
        else if (!strcmp(a, "--truth")) truth_path = v;
        else if (!strcmp(a, "--overlay-scale")) scale = atoi(v);
        else usage();
    }
    if (w <= 0 || h <= 0 || (panel_frame >= 0) != (panel_path != NULL)) usage();
    if (threshold > 255 || ema_shift > 7 || scale < 1 || scale > 8) usage();
    if (show_truth && !truth_path) usage();

    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    if (threshold >= 0) cfg.diff_threshold = (uint8_t)threshold;
    if (ema_shift >= 0) cfg.centroid_ema_shift = (uint8_t)ema_shift;
    size_t work_len = motion_buffer_size(w, h, cfg.cell);
    void *work = malloc(work_len ? work_len : 1);
    motion_ctx_t ctx;
    if (!work_len || !work || motion_init(&ctx, &cfg, w, h, work, work_len) != 0) {
        fprintf(stderr, "motion_sim: motion_init failed for %dx%d\n", w, h);
        return 1;
    }

    truth_t truth = {0};
    if (truth_path && truth_load(&truth, truth_path) != 0) {
        fprintf(stderr, "motion_sim: cannot read %s\n", truth_path);
        free(work);
        return 1;
    }

    const size_t npix = (size_t)w * h;
    uint8_t *bytes = malloc(npix * 2);
    uint16_t *in = malloc(npix * 2);
    uint16_t *out = malloc(npix * 2);
    uint8_t *panels = (panels_video || panel_path) ? malloc(npix * 4 * 3) : NULL;
    if (!bytes || !in || !out || ((panels_video || panel_path) && !panels)) return 1;

    FILE *log = NULL;
    if (log_path) {
        log = fopen(log_path, "w");
        if (!log) return 1;
        fprintf(log, "frame,active,x0,y0,x1,y1,cx,cy,changed_cells,raw_cells,mean_luma,mean_diff\n");
    }

    const int line_px = LINE_PX * scale, cross_half = CROSS_HALF * scale;
    const int gw = motion_grid_w(&ctx), gh = motion_grid_h(&ctx);
    const size_t ncell = (size_t)gw * gh;
    int *act = NULL, *acx = NULL, *acy = NULL, cap_a = 0, cap_x = 0, cap_y = 0;

    overlay_trail_t trail;
    overlay_trail_clear(&trail);
    int frame = 0, active_frames = 0;
    while (fread(bytes, 1, npix * 2, stdin) == npix * 2) {
        for (size_t i = 0; i < npix; i++) {
            in[i] = (uint16_t)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        }
        memcpy(out, in, npix * 2);

        motion_result_t r;
        motion_process(&ctx, in, w * 2, 0, 0, &r);
        if (r.active) {
            active_frames++;
            overlay_trail_push(&trail, r.cx, r.cy);
            overlay_trail_draw(&trail, out, w, h, OVERLAY_GREEN, line_px);
            overlay_rect(out, w, h, r.x0, r.y0, r.x1, r.y1, OVERLAY_GREEN, line_px);
            overlay_crosshair(out, w, h, r.cx, r.cy, cross_half, OVERLAY_GREEN, line_px);
        } else {
            overlay_trail_clear(&trail);
        }
        int tx, ty;
        if (show_truth && truth_at(&truth, frame, &tx, &ty)) {
            overlay_crosshair(out, w, h, tx, ty, TRUTH_HALF * scale, OVERLAY_WHITE, scale);
        }
        if (truth_path &&
            (push_int(&act, &cap_a, frame, r.active) || push_int(&acx, &cap_x, frame, r.cx) ||
             push_int(&acy, &cap_y, frame, r.cy))) {
            return 1;
        }
        if (log) {
            const uint8_t *luma = motion_luma(&ctx), *diff = motion_diff(&ctx);
            const uint8_t *raw = motion_mask_raw(&ctx);
            unsigned long sl = 0, sd = 0;
            int nraw = 0;
            for (size_t i = 0; i < ncell; i++) {
                sl += luma[i];
                sd += diff[i];
                nraw += raw[i];
            }
            fprintf(log, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.2f\n", frame, r.active, r.x0, r.y0,
                    r.x1, r.y1, r.cx, r.cy, r.changed_cells, nraw, (double)sl / ncell,
                    (double)sd / ncell);
        }
        if (panels_video || frame == panel_frame) {
            render_panels(panels, &ctx, cfg.cell, in, out, w, h);
        }
        if (frame == panel_frame && write_ppm(panel_path, panels, 4 * w, h) != 0) {
            fprintf(stderr, "motion_sim: cannot write %s\n", panel_path);
            return 1;
        }

        if (panels_video) {
            if (fwrite(panels, 3, npix * 4, stdout) != npix * 4) return 1;
        } else {
            for (size_t i = 0; i < npix; i++) {
                bytes[2 * i] = (uint8_t)(out[i] & 0xFF);
                bytes[2 * i + 1] = (uint8_t)(out[i] >> 8);
            }
            if (fwrite(bytes, 1, npix * 2, stdout) != npix * 2) return 1;
        }
        frame++;
    }
    if (log) fclose(log);
    fprintf(stderr, "motion_sim: %d frames, %d with motion active\n", frame, active_frames);
    if (truth_path) truth_report(&truth, act, acx, acy, frame);
    free(act);
    free(acx);
    free(acy);
    free(truth.x);
    free(truth.y);
    free(panels);
    free(bytes);
    free(in);
    free(out);
    free(work);
    return 0;
}
