/*
 * RGB565 drawing primitives for the motion overlay. Pure C99.
 *
 * Every function takes the target buffer as fb (fb_w x fb_h pixels, fb_w
 * pixels per line) and clips to it, so any coordinates are safe to pass.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVERLAY_GREEN     0x07E0
#define OVERLAY_BLACK     0x0000
#define OVERLAY_TRAIL_LEN 16

/* Filled rectangle, inclusive corners in any order. */
void overlay_fill_rect(uint16_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1,
                       uint16_t color);

/* Rectangle outline, inclusive corners, drawn inward with the given thickness. */
void overlay_rect(uint16_t *fb, int fb_w, int fb_h, int x0, int y0, int x1, int y1, uint16_t color,
                  int thickness);

/* Plus sign centred on (cx, cy), each arm half_len pixels long. */
void overlay_crosshair(uint16_t *fb, int fb_w, int fb_h, int cx, int cy, int half_len,
                       uint16_t color, int thickness);

/* Ring buffer of the last OVERLAY_TRAIL_LEN centroids. */
typedef struct {
    int16_t x[OVERLAY_TRAIL_LEN];
    int16_t y[OVERLAY_TRAIL_LEN];
    int head;  /* index of the next write */
    int count; /* valid points, <= OVERLAY_TRAIL_LEN */
} overlay_trail_t;

void overlay_trail_clear(overlay_trail_t *t);
void overlay_trail_push(overlay_trail_t *t, int x, int y);

/* Connects the points oldest to newest; older segments are drawn dimmer. */
void overlay_trail_draw(const overlay_trail_t *t, uint16_t *fb, int fb_w, int fb_h,
                        uint16_t color, int thickness);

#ifdef __cplusplus
}
#endif
