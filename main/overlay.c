#include "overlay.h"

#include <stddef.h>

/* Keeps coordinate arithmetic far away from int overflow; anything beyond is off-screen anyway. */
#define COORD_LIMIT     (1 << 20)
#define THICKNESS_LIMIT 64

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void overlay_fill_rect(uint16_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1,
                       uint16_t color)
{
    if (!fb || fb_w <= 0 || fb_h <= 0) return;
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x1 < 0 || y1 < 0 || x0 >= fb_w || y0 >= fb_h) return;
    x0 = clampi(x0, 0, fb_w - 1);
    x1 = clampi(x1, 0, fb_w - 1);
    y0 = clampi(y0, 0, fb_h - 1);
    y1 = clampi(y1, 0, fb_h - 1);
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb + (size_t)y * (size_t)fb_w;
        for (int x = x0; x <= x1; x++) row[x] = color;
    }
}

void overlay_rect(uint16_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1, uint16_t color,
                  int thickness)
{
    x0 = clampi(x0, -COORD_LIMIT, COORD_LIMIT);
    x1 = clampi(x1, -COORD_LIMIT, COORD_LIMIT);
    y0 = clampi(y0, -COORD_LIMIT, COORD_LIMIT);
    y1 = clampi(y1, -COORD_LIMIT, COORD_LIMIT);
    int t = clampi(thickness, 1, THICKNESS_LIMIT);
    if (x0 > x1) {
        int s = x0;
        x0 = x1;
        x1 = s;
    }
    if (y0 > y1) {
        int s = y0;
        y0 = y1;
        y1 = s;
    }
    overlay_fill_rect(fb, fb_w, fb_h, x0, y0, x1, y0 + t - 1, color);
    overlay_fill_rect(fb, fb_w, fb_h, x0, y1 - t + 1, x1, y1, color);
    overlay_fill_rect(fb, fb_w, fb_h, x0, y0, x0 + t - 1, y1, color);
    overlay_fill_rect(fb, fb_w, fb_h, x1 - t + 1, y0, x1, y1, color);
}

void overlay_crosshair(uint16_t *fb, int fb_w, int fb_h, int cx, int cy, int half_len,
                       uint16_t color, int thickness)
{
    cx = clampi(cx, -COORD_LIMIT, COORD_LIMIT);
    cy = clampi(cy, -COORD_LIMIT, COORD_LIMIT);
    half_len = clampi(half_len, 0, COORD_LIMIT);
    int t = clampi(thickness, 1, THICKNESS_LIMIT);
    int lo = -(t / 2);
    int hi = lo + t - 1;
    overlay_fill_rect(fb, fb_w, fb_h, cx - half_len, cy + lo, cx + half_len, cy + hi, color);
    overlay_fill_rect(fb, fb_w, fb_h, cx + lo, cy - half_len, cx + hi, cy + half_len, color);
}

void overlay_trail_clear(overlay_trail_t *t)
{
    if (!t) return;
    t->head = 0;
    t->count = 0;
}

void overlay_trail_push(overlay_trail_t *t, int x, int y)
{
    if (!t) return;
    if (t->head < 0 || t->head >= OVERLAY_TRAIL_LEN) t->head = 0;
    t->x[t->head] = (int16_t)clampi(x, INT16_MIN, INT16_MAX);
    t->y[t->head] = (int16_t)clampi(y, INT16_MIN, INT16_MAX);
    t->head = (t->head + 1) % OVERLAY_TRAIL_LEN;
    if (t->count < OVERLAY_TRAIL_LEN) t->count++;
}

static uint16_t scale_color(uint16_t c, int num, int den)
{
    uint32_t r = ((c >> 11) & 0x1F) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 5) & 0x3F) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0x1F) * (uint32_t)num / (uint32_t)den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_line(uint16_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1,
                      uint16_t color, int t)
{
    int lo = -(t / 2);
    int hi = lo + t - 1;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        overlay_fill_rect(fb, fb_w, fb_h, x0 + lo, y0 + lo, x0 + hi, y0 + hi, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void overlay_trail_draw(const overlay_trail_t *t, uint16_t *fb, int fb_w, int fb_h,
                        uint16_t color, int thickness)
{
    if (!t || t->count <= 0 || t->count > OVERLAY_TRAIL_LEN) return;
    int th = clampi(thickness, 1, THICKNESS_LIMIT);
    int oldest = (t->head - t->count + OVERLAY_TRAIL_LEN) % OVERLAY_TRAIL_LEN;
    if (t->count == 1) {
        draw_line(fb, fb_w, fb_h, t->x[oldest], t->y[oldest], t->x[oldest], t->y[oldest], color,
                  th);
        return;
    }
    for (int k = 1; k < t->count; k++) {
        int a = (oldest + k - 1) % OVERLAY_TRAIL_LEN;
        int b = (oldest + k) % OVERLAY_TRAIL_LEN;
        uint16_t c = scale_color(color, k + 1, t->count);
        draw_line(fb, fb_w, fb_h, t->x[a], t->y[a], t->x[b], t->y[b], c, th);
    }
}
