#include "motion.h"

#include <string.h>

#define MOTION_MAX_GRID 4096

static inline uint8_t rgb565_luma(uint16_t p)
{
    uint32_t r5 = (p >> 11) & 0x1F;
    uint32_t g6 = (p >> 5) & 0x3F;
    uint32_t b5 = p & 0x1F;
    uint32_t r8 = (r5 << 3) | (r5 >> 2);
    uint32_t g8 = (g6 << 2) | (g6 >> 4);
    uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
}

/* delta / 2^shift rounded toward zero, so the EMA converges from both sides. */
static inline int32_t ema_step(int32_t delta, int shift)
{
    return delta >= 0 ? (delta >> shift) : -((-delta) >> shift);
}

static bool grid_dims(int roi_w, int roi_h, int cell, int *gw, int *gh)
{
    if (roi_w <= 0 || roi_h <= 0 || cell <= 0) return false;
    int w = roi_w / cell;
    int h = roi_h / cell;
    if (w < 1 || h < 1 || w > MOTION_MAX_GRID || h > MOTION_MAX_GRID) return false;
    *gw = w;
    *gh = h;
    return true;
}

size_t motion_buffer_size(int roi_w, int roi_h, int cell)
{
    int gw, gh;
    if (!grid_dims(roi_w, roi_h, cell, &gw, &gh)) return 0;
    size_t n = (size_t)gw * (size_t)gh;
    return n * sizeof(uint16_t) + 6 * n;
}

static bool cfg_valid(const motion_cfg_t *c)
{
    return c->cell >= 1 && c->bg_shift <= 7 && c->min_neighbors <= 8 && c->min_cells >= 1 &&
           c->centroid_ema_shift <= 7;
}

int motion_init(motion_ctx_t *ctx, const motion_cfg_t *cfg, int roi_w, int roi_h, void *work_buf,
                size_t work_len)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    if (!cfg || !work_buf || !cfg_valid(cfg)) return -1;
    if (((uintptr_t)work_buf & 1u) != 0) return -2;

    int gw, gh;
    if (!grid_dims(roi_w, roi_h, cfg->cell, &gw, &gh)) return -3;
    size_t need = motion_buffer_size(roi_w, roi_h, cfg->cell);
    if (need == 0 || work_len < need) return -4;

    size_t n = (size_t)gw * (size_t)gh;
    ctx->cfg = *cfg;
    if (ctx->cfg.hysteresis_frames == 0) ctx->cfg.hysteresis_frames = 1;
    ctx->roi_w = roi_w;
    ctx->roi_h = roi_h;
    ctx->gw = gw;
    ctx->gh = gh;

    uint8_t *p = (uint8_t *)work_buf;
    ctx->bg_q8 = (uint16_t *)p;
    p += n * sizeof(uint16_t);
    ctx->bg = p;
    p += n;
    ctx->luma = p;
    p += n;
    ctx->diff = p;
    p += n;
    ctx->mask_raw = p;
    p += n;
    ctx->mask = p;
    p += n;
    ctx->fg_run = p;

    memset(work_buf, 0, need);
    ctx->ready = true;
    return 0;
}

void motion_reset(motion_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->seeded = false;
    ctx->run = 0;
    ctx->ema_valid = false;
    ctx->ema_x_q8 = 0;
    ctx->ema_y_q8 = 0;
}

static void sample_luma(motion_ctx_t *ctx, const uint16_t *rgb565, int stride_bytes, int roi_x,
                        int roi_y)
{
    const int cell = ctx->cfg.cell;
    const int half = cell / 2;
    const uint8_t *base = (const uint8_t *)rgb565;
    uint8_t *luma = ctx->luma;

    for (int gy = 0; gy < ctx->gh; gy++) {
        const uint16_t *row =
            (const uint16_t *)(base + (size_t)(roi_y + gy * cell + half) * (size_t)stride_bytes) +
            roi_x + half;
        for (int gx = 0; gx < ctx->gw; gx++) {
            *luma++ = rgb565_luma(row[gx * cell]);
        }
    }
}

void motion_process(motion_ctx_t *ctx, const uint16_t *rgb565, int stride_bytes, int roi_x,
                    int roi_y, motion_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->ready || !rgb565 || roi_x < 0 || roi_y < 0 || (stride_bytes & 1) ||
        stride_bytes < (roi_x + ctx->roi_w) * (int)sizeof(uint16_t)) {
        return;
    }

    const motion_cfg_t *cfg = &ctx->cfg;
    const int gw = ctx->gw, gh = ctx->gh;
    const size_t n = (size_t)gw * (size_t)gh;

    sample_luma(ctx, rgb565, stride_bytes, roi_x, roi_y);

    if (!ctx->seeded) {
        for (size_t i = 0; i < n; i++) {
            ctx->bg_q8[i] = (uint16_t)(ctx->luma[i] << 8);
            ctx->bg[i] = ctx->luma[i];
        }
        memset(ctx->diff, 0, n);
        memset(ctx->mask_raw, 0, n);
        memset(ctx->mask, 0, n);
        memset(ctx->fg_run, 0, n);
        ctx->seeded = true;
        ctx->run = 0;
        ctx->ema_valid = false;
        return;
    }

    for (size_t i = 0; i < n; i++) {
        int y = ctx->luma[i];
        int d = y - (int)ctx->bg[i];
        if (d < 0) d = -d;
        ctx->diff[i] = (uint8_t)d;
        uint8_t raw = d > cfg->diff_threshold;
        ctx->mask_raw[i] = raw;

        int32_t cur = ctx->bg_q8[i];
        if (!raw) {
            ctx->fg_run[i] = 0;
            cur += ema_step(((int32_t)y << 8) - cur, cfg->bg_shift);
        } else if (++ctx->fg_run[i] >= MOTION_FG_ABSORB_FRAMES) {
            ctx->fg_run[i] = 0;
            cur = (int32_t)y << 8;
        }
        ctx->bg_q8[i] = (uint16_t)cur;
        ctx->bg[i] = (uint8_t)((cur + 128) >> 8);
    }

    const int cell = cfg->cell;
    const int half = cell / 2;
    int count = 0;
    int32_t m10 = 0, m01 = 0;
    int gx0 = gw, gy0 = gh, gx1 = -1, gy1 = -1;
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            size_t i = (size_t)gy * gw + gx;
            if (!ctx->mask_raw[i]) {
                ctx->mask[i] = 0;
                continue;
            }
            int nb = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = gy + dy;
                if (yy < 0 || yy >= gh) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = gx + dx;
                    if ((dx == 0 && dy == 0) || xx < 0 || xx >= gw) continue;
                    nb += ctx->mask_raw[(size_t)yy * gw + xx];
                }
            }
            uint8_t keep = nb >= cfg->min_neighbors;
            ctx->mask[i] = keep;
            if (!keep) continue;
            count++;
            m10 += gx * cell + half;
            m01 += gy * cell + half;
            if (gx < gx0) gx0 = gx;
            if (gx > gx1) gx1 = gx;
            if (gy < gy0) gy0 = gy;
            if (gy > gy1) gy1 = gy;
        }
    }
    out->changed_cells = count;

    if (count < cfg->min_cells) {
        ctx->run = 0;
        ctx->ema_valid = false;
        return;
    }

    int32_t cx_q8 = (int32_t)(((int64_t)m10 << 8) / count);
    int32_t cy_q8 = (int32_t)(((int64_t)m01 << 8) / count);
    if (!ctx->ema_valid) {
        ctx->ema_x_q8 = cx_q8;
        ctx->ema_y_q8 = cy_q8;
        ctx->ema_valid = true;
    } else {
        ctx->ema_x_q8 += ema_step(cx_q8 - ctx->ema_x_q8, cfg->centroid_ema_shift);
        ctx->ema_y_q8 += ema_step(cy_q8 - ctx->ema_y_q8, cfg->centroid_ema_shift);
    }
    if (ctx->run < 255) ctx->run++;
    if (ctx->run < cfg->hysteresis_frames) return;

    /* Only cell centres are sampled, so a change can extend up to cell - 1 pixels past the
     * outermost changed sample before the next (unchanged) sample would have caught it. */
    int x0 = gx0 * cell + half - (cell - 1);
    int y0 = gy0 * cell + half - (cell - 1);
    int x1 = gx1 * cell + half + (cell - 1);
    int y1 = gy1 * cell + half + (cell - 1);
    out->active = true;
    out->x0 = x0 < 0 ? 0 : x0;
    out->y0 = y0 < 0 ? 0 : y0;
    out->x1 = x1 > ctx->roi_w - 1 ? ctx->roi_w - 1 : x1;
    out->y1 = y1 > ctx->roi_h - 1 ? ctx->roi_h - 1 : y1;
    int cx = (int)((ctx->ema_x_q8 + 128) >> 8);
    int cy = (int)((ctx->ema_y_q8 + 128) >> 8);
    out->cx = cx < 0 ? 0 : (cx >= ctx->roi_w ? ctx->roi_w - 1 : cx);
    out->cy = cy < 0 ? 0 : (cy >= ctx->roi_h ? ctx->roi_h - 1 : cy);
}

int motion_grid_w(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->gw : 0;
}

int motion_grid_h(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->gh : 0;
}

const uint8_t *motion_luma(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->luma : NULL;
}

const uint8_t *motion_background(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->bg : NULL;
}

const uint8_t *motion_diff(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->diff : NULL;
}

const uint8_t *motion_mask_raw(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->mask_raw : NULL;
}

const uint8_t *motion_mask(const motion_ctx_t *ctx)
{
    return (ctx && ctx->ready) ? ctx->mask : NULL;
}
