/*
 * Block-luma motion detector for RGB565 frames.
 *
 * Pure C99 with no ESP-IDF dependencies, so the same file runs in the firmware,
 * the host unit tests (test/host) and the host simulation (tools/sim).
 * The caller owns all memory; nothing is allocated here.
 *
 * Per frame, for each cell x cell block of the region of interest (ROI):
 *   Y    = (77 R + 150 G + 29 B) >> 8 of the block's centre pixel
 *   diff = |Y - B|                       (B = background before this frame)
 *   raw  = diff > diff_threshold
 *   mask = raw && (set 8-neighbours >= min_neighbors)
 *   B   += (Y - B) >> bg_shift           (held while a cell is freshly changed,
 *                                         see MOTION_FG_ABSORB_FRAMES)
 * Motion is active after hysteresis_frames consecutive frames with at least
 * min_cells mask cells. The bounding box spans the mask cell centres plus
 * cell - 1 pixels on each side (the gap to the next, unchanged sample); the
 * centroid is m10/m00, m01/m00 of the mask cell centres, smoothed with an EMA.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cell;                   /* block size in pixels (square) */
    uint8_t diff_threshold;     /* luma difference that marks a cell as changed */
    uint8_t bg_shift;           /* background EMA weight 1 / 2^bg_shift */
    uint8_t min_neighbors;      /* changed cell kept if >= this many of its 8 neighbours changed */
    int min_cells;              /* mask cells needed for a frame to count as motion */
    uint8_t hysteresis_frames;  /* consecutive motion frames before active */
    uint8_t centroid_ema_shift; /* centroid EMA weight 1 / 2^centroid_ema_shift */
} motion_cfg_t;

#define MOTION_CFG_DEFAULT                                                                         \
    {                                                                                              \
        .cell = 8, .diff_threshold = 18, .bg_shift = 3, .min_neighbors = 3, .min_cells = 12,      \
        .hysteresis_frames = 2, .centroid_ema_shift = 2,                                           \
    }

/* The background is not updated in cells whose raw bit is set; otherwise it
 * soaks up a moving object and leaves a ghost trail behind it. A cell that stays
 * set for this many consecutive frames (a stopped object, a lighting change) is
 * reset to the current luma so it cannot stay "moving" forever. Max 255. */
#define MOTION_FG_ABSORB_FRAMES 120

typedef struct {
    bool active;
    int x0, y0, x1, y1; /* inclusive bounding box, ROI pixel coordinates; valid when active */
    int cx, cy;         /* smoothed centroid, ROI pixel coordinates; valid when active */
    int changed_cells;  /* mask cells this frame, reported even when not active */
} motion_result_t;

/* Treat as opaque; fields are public only so the context can live in static storage. */
typedef struct {
    motion_cfg_t cfg;
    int roi_w, roi_h;
    int gw, gh;
    uint16_t *bg_q8;   /* background, 8.8 fixed point */
    uint8_t *bg;       /* background rounded to 8 bits (value used for the next diff) */
    uint8_t *luma;     /* Y of the last frame */
    uint8_t *diff;     /* |Y - B| of the last frame */
    uint8_t *mask_raw; /* 0/1, threshold only */
    uint8_t *mask;     /* 0/1, after the neighbour filter */
    uint8_t *fg_run;   /* consecutive frames each cell has had its raw bit set */
    bool ready;
    bool seeded;
    int run;           /* consecutive frames with >= min_cells */
    bool ema_valid;
    int32_t ema_x_q8, ema_y_q8;
} motion_ctx_t;

/* Bytes of work memory motion_init() needs for this ROI, or 0 if the arguments are invalid. */
size_t motion_buffer_size(int roi_w, int roi_h, int cell);

/*
 * Prepares ctx for a roi_w x roi_h region. work_buf must be at least
 * motion_buffer_size() bytes and 2-byte aligned; it is used until the context
 * is discarded. Returns 0 on success or a negative value on invalid arguments,
 * in which case motion_process() reports no motion.
 */
int motion_init(motion_ctx_t *ctx, const motion_cfg_t *cfg, int roi_w, int roi_h, void *work_buf,
                size_t work_len);

/*
 * Processes one frame. rgb565 points at pixel (0, 0) of the whole image and
 * stride_bytes is its line pitch; the ROI starts at (roi_x, roi_y). The caller
 * guarantees the ROI lies inside the image. The first frame after init or
 * reset only seeds the background and reports no motion.
 */
void motion_process(motion_ctx_t *ctx, const uint16_t *rgb565, int stride_bytes, int roi_x,
                    int roi_y, motion_result_t *out);

/* Forgets the background and tracking state; the next frame re-seeds. */
void motion_reset(motion_ctx_t *ctx);

/* Debug views of the last processed frame, motion_grid_w() x motion_grid_h(), row-major. */
int motion_grid_w(const motion_ctx_t *ctx);
int motion_grid_h(const motion_ctx_t *ctx);
const uint8_t *motion_luma(const motion_ctx_t *ctx);
const uint8_t *motion_background(const motion_ctx_t *ctx);
const uint8_t *motion_diff(const motion_ctx_t *ctx);
const uint8_t *motion_mask_raw(const motion_ctx_t *ctx);
const uint8_t *motion_mask(const motion_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
