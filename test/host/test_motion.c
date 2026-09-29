/*
 * Host unit tests for main/motion.c and main/overlay.c.
 * Build and run: make -C test/host test   (or: make -C test/host asan)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion.h"
#include "overlay.h"

#define W     320
#define H     240
#define GUARD 64
#define GUARD_BYTE 0xA5

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                               \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Static textured background: luma roughly 60..140, varies at every pixel. */
static int texture(int x, int y)
{
    return 60 + (int)(hash32((uint32_t)(y * 7919 + x)) % 81);
}

static uint32_t g_rng = 12345;
static int rng_range(int lo, int hi)
{
    g_rng = g_rng * 1664525U + 1013904223U;
    return lo + (int)((g_rng >> 8) % (uint32_t)(hi - lo + 1));
}

static void render(uint16_t *img, int noise_amp, int sq_x, int sq_y, int sq_size, int sq_luma)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int v = texture(x, y);
            if (sq_size > 0 && x >= sq_x && x < sq_x + sq_size && y >= sq_y &&
                y < sq_y + sq_size) {
                v = sq_luma;
            }
            if (noise_amp > 0) v += rng_range(-noise_amp, noise_amp);
            img[y * W + x] = rgb565(v, v, v);
        }
    }
}

typedef struct {
    motion_ctx_t ctx;
    uint8_t *raw;  /* allocation including guards */
    uint8_t *work; /* start of the usable work buffer */
    size_t len;
} fixture_t;

static void fixture_init(fixture_t *f, const motion_cfg_t *cfg)
{
    f->len = motion_buffer_size(W, H, cfg->cell);
    f->raw = malloc(f->len + 2 * GUARD);
    memset(f->raw, GUARD_BYTE, f->len + 2 * GUARD);
    f->work = f->raw + GUARD;
    int rc = motion_init(&f->ctx, cfg, W, H, f->work, f->len);
    CHECK(rc == 0, "motion_init returned %d", rc);
}

static int guards_intact(const uint8_t *base, size_t len)
{
    for (size_t i = 0; i < GUARD; i++) {
        if (base[i] != GUARD_BYTE || base[GUARD + len + i] != GUARD_BYTE) return 0;
    }
    return 1;
}

static void fixture_free(fixture_t *f)
{
    CHECK(guards_intact(f->raw, f->len), "motion wrote outside its work buffer");
    free(f->raw);
}

static void test_api_guards(void)
{
    printf("api guards\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    motion_ctx_t ctx;
    size_t len = motion_buffer_size(W, H, cfg.cell);
    CHECK(len == (size_t)(W / 8) * (H / 8) * 8, "buffer size %zu", len);
    CHECK(motion_buffer_size(0, H, 8) == 0, "zero width accepted");
    CHECK(motion_buffer_size(W, H, 0) == 0, "zero cell accepted");
    CHECK(motion_buffer_size(4, 4, 8) == 0, "roi smaller than a cell accepted");

    uint16_t *buf = malloc(len + 2);
    CHECK(motion_init(&ctx, &cfg, W, H, buf, len - 1) < 0, "short buffer accepted");
    CHECK(motion_init(&ctx, &cfg, W, H, (uint8_t *)buf + 1, len) < 0, "odd buffer accepted");
    CHECK(motion_init(&ctx, NULL, W, H, buf, len) < 0, "NULL cfg accepted");

    /* A failed init leaves a context that reports nothing and does not crash. */
    uint16_t *img = calloc((size_t)W * H, sizeof(uint16_t));
    motion_result_t r;
    motion_process(&ctx, img, W * 2, 0, 0, &r);
    CHECK(!r.active && r.changed_cells == 0, "uninitialised context reported motion");
    CHECK(motion_mask(&ctx) == NULL, "accessor on failed context returned data");

    CHECK(motion_init(&ctx, &cfg, W, H, buf, len) == 0, "valid init failed");
    motion_process(&ctx, img, W * 2 - 2, 0, 0, &r); /* stride too small for the ROI */
    CHECK(!r.active, "short stride processed");
    free(img);
    free(buf);
}

static void test_static_scene(void)
{
    printf("static scene -> never active\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    fixture_t f;
    fixture_init(&f, &cfg);
    uint16_t *img = malloc((size_t)W * H * sizeof(uint16_t));
    render(img, 0, 0, 0, 0, 0);
    int ever_active = 0, max_cells = 0;
    for (int i = 0; i < 100; i++) {
        motion_result_t r;
        motion_process(&f.ctx, img, W * 2, 0, 0, &r);
        ever_active |= r.active;
        if (r.changed_cells > max_cells) max_cells = r.changed_cells;
    }
    CHECK(!ever_active, "static scene became active");
    CHECK(max_cells == 0, "static scene changed %d cells", max_cells);
    free(img);
    fixture_free(&f);
}

static void test_noise_below_threshold(void)
{
    printf("noise below threshold -> never active\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    fixture_t f;
    fixture_init(&f, &cfg);
    uint16_t *img = malloc((size_t)W * H * sizeof(uint16_t));
    /* +-5 per pixel plus RGB565 quantisation keeps |Y - B| under the threshold of 18. */
    int ever_active = 0, max_cells = 0;
    for (int i = 0; i < 200; i++) {
        render(img, 5, 0, 0, 0, 0);
        motion_result_t r;
        motion_process(&f.ctx, img, W * 2, 0, 0, &r);
        ever_active |= r.active;
        if (r.changed_cells > max_cells) max_cells = r.changed_cells;
    }
    CHECK(!ever_active, "noise below threshold became active");
    CHECK(max_cells == 0, "noise changed %d cells", max_cells);
    free(img);
    fixture_free(&f);
}

static void test_moving_square(void)
{
    printf("moving 64x64 square -> active after hysteresis, bbox and centroid\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    fixture_t f;
    fixture_init(&f, &cfg);
    uint16_t *img = malloc((size_t)W * H * sizeof(uint16_t));
    const int size = 64, vx = 2, vy = 1, frames = 90;
    int first_active = -1;
    int worst_err = 0;
    int checked = 0;

    render(img, 2, 0, 0, 0, 0); /* frame 0: background only, seeds the model */
    motion_result_t r;
    motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    CHECK(!r.active && r.changed_cells == 0, "seed frame reported motion");

    for (int i = 1; i <= frames; i++) {
        int sx = 20 + vx * i, sy = 40 + vy * i;
        render(img, 2, sx, sy, size, 240);
        motion_process(&f.ctx, img, W * 2, 0, 0, &r);
        if (r.active && first_active < 0) first_active = i;
        if (i < cfg.hysteresis_frames) {
            CHECK(!r.active, "active at frame %d before hysteresis", i);
        } else {
            CHECK(r.active, "not active at frame %d", i);
        }
        if (!r.active) continue;
        CHECK(r.x0 <= sx && r.y0 <= sy && r.x1 >= sx + size - 1 && r.y1 >= sy + size - 1,
              "frame %d: bbox (%d,%d)-(%d,%d) does not contain square (%d,%d)-(%d,%d)", i, r.x0,
              r.y0, r.x1, r.y1, sx, sy, sx + size - 1, sy + size - 1);
        CHECK(r.x1 - r.x0 < size + 2 * cfg.cell && r.y1 - r.y0 < size + 2 * cfg.cell,
              "frame %d: bbox %dx%d too large (ghosting?)", i, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
        /* The centroid EMA needs a few frames to catch up after the square appears. */
        if (i >= cfg.hysteresis_frames + 8) {
            int ex = r.cx - (sx + size / 2), ey = r.cy - (sy + size / 2);
            int err = abs(ex) > abs(ey) ? abs(ex) : abs(ey);
            if (err > worst_err) worst_err = err;
            checked++;
            CHECK(err <= cfg.cell, "frame %d: centroid (%d,%d) vs square centre (%d,%d)", i, r.cx,
                  r.cy, sx + size / 2, sy + size / 2);
        }
    }
    CHECK(first_active == cfg.hysteresis_frames, "first active frame %d", first_active);
    printf("  first active frame %d, worst centroid error %d px over %d frames\n", first_active,
           worst_err, checked);
    free(img);
    fixture_free(&f);
}

static void test_square_leaves(void)
{
    printf("square leaves the scene -> motion stops, no lasting ghost\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    fixture_t f;
    fixture_init(&f, &cfg);
    uint16_t *img = malloc((size_t)W * H * sizeof(uint16_t));
    motion_result_t r;
    render(img, 0, 0, 0, 0, 0);
    motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    for (int i = 1; i <= 30; i++) {
        render(img, 0, 20 + 4 * i, 60, 64, 240);
        motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    }
    CHECK(r.active, "not active while moving");
    render(img, 0, 0, 0, 0, 0);
    motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    CHECK(!r.active, "still active one frame after the square left (cells=%d)", r.changed_cells);
    free(img);
    fixture_free(&f);
}

static void test_stopped_object_absorbed(void)
{
    printf("object that stops -> absorbed into the background\n");
    motion_cfg_t cfg = MOTION_CFG_DEFAULT;
    fixture_t f;
    fixture_init(&f, &cfg);
    uint16_t *img = malloc((size_t)W * H * sizeof(uint16_t));
    motion_result_t r;
    render(img, 0, 0, 0, 0, 0);
    motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    render(img, 0, 100, 80, 64, 240);
    int active_frames = 0;
    for (int i = 0; i < MOTION_FG_ABSORB_FRAMES + 20; i++) {
        motion_process(&f.ctx, img, W * 2, 0, 0, &r);
        active_frames += r.active;
    }
    CHECK(!r.active, "stopped object still active after %d frames", MOTION_FG_ABSORB_FRAMES + 20);
    CHECK(active_frames >= MOTION_FG_ABSORB_FRAMES - cfg.hysteresis_frames,
          "stopped object active for only %d frames", active_frames);
    /* Once absorbed, removing it again is a change in the opposite direction. */
    render(img, 0, 0, 0, 0, 0);
    motion_process(&f.ctx, img, W * 2, 0, 0, &r);
    CHECK(r.changed_cells > 0, "removal of an absorbed object not seen");
    free(img);
    fixture_free(&f);
}

/* ---- overlay ---- */

#define FB_W 64
#define FB_H 48
#define PIX_GUARD 256
#define BG    0x1234
#define GUARD_PIX 0xA5A5

static uint16_t g_fbmem[PIX_GUARD + FB_W * FB_H + PIX_GUARD];
static uint16_t *const g_fb = g_fbmem + PIX_GUARD;

static void fb_reset(void)
{
    for (size_t i = 0; i < sizeof(g_fbmem) / sizeof(g_fbmem[0]); i++) g_fbmem[i] = GUARD_PIX;
    for (int i = 0; i < FB_W * FB_H; i++) g_fb[i] = BG;
}

static int fb_guards_intact(void)
{
    for (int i = 0; i < PIX_GUARD; i++) {
        if (g_fbmem[i] != GUARD_PIX || g_fb[FB_W * FB_H + i] != GUARD_PIX) return 0;
    }
    return 1;
}

static void test_overlay_clipping(void)
{
    printf("overlay clipping\n");
    const uint16_t c = OVERLAY_GREEN;

    /* Rectangle hanging off the top-left and bottom: compare with a per-pixel reference. */
    fb_reset();
    const int x0 = -10, y0 = -7, x1 = 30, y1 = 70, t = 3;
    overlay_rect(g_fb, FB_W, FB_H, x0, y0, x1, y1, c, t);
    int mismatches = 0;
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            int outer = x >= x0 && x <= x1 && y >= y0 && y <= y1;
            int inner = x >= x0 + t && x <= x1 - t && y >= y0 + t && y <= y1 - t;
            uint16_t want = (outer && !inner) ? c : BG;
            if (g_fb[y * FB_W + x] != want) mismatches++;
        }
    }
    CHECK(mismatches == 0, "rect differs from reference at %d pixels", mismatches);
    CHECK(fb_guards_intact(), "rect wrote outside the framebuffer");

    /* Shapes entirely or mostly off-screen, huge coordinates and thickness. */
    fb_reset();
    overlay_rect(g_fb, FB_W, FB_H, -500, -500, -100, -100, c, 5);
    overlay_rect(g_fb, FB_W, FB_H, 1000, 1000, 2000, 2000, c, 5);
    overlay_fill_rect(g_fb, FB_W, FB_H, FB_W, 0, FB_W + 10, FB_H - 1, c);
    overlay_fill_rect(g_fb, FB_W, FB_H, 0, FB_H, FB_W - 1, FB_H + 10, c);
    int drawn = 0;
    for (int i = 0; i < FB_W * FB_H; i++) drawn += g_fb[i] != BG;
    CHECK(drawn == 0, "off-screen shapes drew %d pixels", drawn);
    overlay_rect(g_fb, FB_W, FB_H, -2000000000, -2000000000, 2000000000, 2000000000, c, 1000000);
    overlay_crosshair(g_fb, FB_W, FB_H, FB_W - 1, 0, 1000000000, c, 1000000);
    overlay_crosshair(g_fb, FB_W, FB_H, -2000000000, 2000000000, 2000000000, c, -5);
    CHECK(fb_guards_intact(), "extreme coordinates wrote outside the framebuffer");

    /* A crosshair near the right edge must not wrap into the next row. */
    fb_reset();
    overlay_crosshair(g_fb, FB_W, FB_H, FB_W - 2, 10, 8, c, 1);
    CHECK(g_fb[11 * FB_W + 0] == BG && g_fb[10 * FB_W + 0] == BG, "crosshair wrapped a row");
    CHECK(g_fb[10 * FB_W + FB_W - 1] == c && g_fb[10 * FB_W + FB_W - 10] == c,
          "crosshair arm missing");
    CHECK(fb_guards_intact(), "crosshair wrote outside the framebuffer");

    /* Trail through and beyond the corners. */
    fb_reset();
    overlay_trail_t trail;
    overlay_trail_clear(&trail);
    const int pts[][2] = {{-100, -100}, {10, 10}, {FB_W + 50, 20}, {30, FB_H + 80}, {5, 5}};
    for (int k = 0; k < 40; k++) {
        overlay_trail_push(&trail, pts[k % 5][0], pts[k % 5][1]);
        overlay_trail_draw(&trail, g_fb, FB_W, FB_H, c, 4);
    }
    CHECK(trail.count == OVERLAY_TRAIL_LEN, "trail count %d", trail.count);
    CHECK(fb_guards_intact(), "trail wrote outside the framebuffer");

    /* NULL / empty targets are ignored. */
    overlay_rect(NULL, FB_W, FB_H, 0, 0, 10, 10, c, 1);
    overlay_rect(g_fb, 0, FB_H, 0, 0, 10, 10, c, 1);
    overlay_trail_draw(NULL, g_fb, FB_W, FB_H, c, 1);
    CHECK(fb_guards_intact(), "degenerate calls wrote outside the framebuffer");
}

int main(void)
{
    test_api_guards();
    test_static_scene();
    test_noise_below_threshold();
    test_moving_square();
    test_square_leaves();
    test_stopped_object_absorbed();
    test_overlay_clipping();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
