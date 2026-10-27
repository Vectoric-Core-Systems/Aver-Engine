#pragma once
// Aver.Physics: heightfields v2 (non-square, holes, a physical surface per cell, edits while the body runs).
// aver_phys_add_heightfield (physics_abi.h) is built through the same code with an empty parameter block. The shape route is
// aver_phys_heightfield_shape plus aver_phys_body_from_shape (static only; a dynamic request is refused).
// Units and orientation as aver_phys_add_heightfield: cm. samples is cols * rows row-major in the v1 PHYSICS ORDER: sample (column x, row y)
// lands at engine (cx - y * spacing, cy + x * spacing, cz + height) for a body at (cx, cy, cz). Cell (x, y) spans columns x..x+1, rows y..y+1.
// HOLES: a sample that is NaN, infinite or FLT_MAX is a hole. A hole on a cell's diagonal drops the whole cell; a hole on either other corner
// drops one triangle. So a hole at (x, y) removes cells (x, y) and (x-1, y-1) and half of (x-1, y) and (x, y-1). Rays pass through.
// NON-SQUARE: Jolt's field is square, built N x N with N = max(cols, rows) and extra samples as holes (memory is N^2 whatever the rectangle).
// Edits clone the shape, edit the clone and swap it with SetShape, so queries in flight finish on the old one. A clone copies the field:
// batch edits per frame. Edits invalidate raw sub-shape ids (cell coordinates stay valid).
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PHYS_TERRAIN_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_terrain_abi_version(void);

/* ---- heightfield parameter block (HP): any prefix, a longer block is refused, a negative entry is the default ---- */
#define AVER_PHYS_HP_BLOCK_SIZE       0   /* 2 to 8, default 2: bigger saves memory and slows queries */
#define AVER_PHYS_HP_BITS_PER_SAMPLE  1   /* 1 to 16, default 8 */
#define AVER_PHYS_HP_EDGE_ANGLE_DEG   2   /* active-edge angle, 0 to 90, default 5 */
#define AVER_PHYS_HP_EDIT_HEADROOM_CM 3   /* default 0: widens the editable height range by this much each way */
#define AVER_PHYS_HP_COUNT            4

/* Edit flags. */
#define AVER_PHYS_HF_WAKE             1   /* wake the dynamic bodies over the edited area so they fall into a new crater */

/* 2 to 16384 samples per side, spaced spacingCm apart. QUANTISATION: Jolt compresses each block relative to its own height range and
   edits clamp to a global range fixed here. With headroom 0 that range is the lowest to the highest sample, so a crater below the lowest
   sample cannot be made later; EDIT_HEADROOM_CM widens it. palette is up to 256 surface ids. Returns a shape handle. */
AVER_PHYS_API int32_t aver_phys_heightfield_shape(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                                  const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                                  const float* p, int32_t n);

/* Sets a w * h rectangle of samples at column x0, row y0 (row-major cm, NaN = hole). The body's shape must BE the heightfield (a scaled one
   is refused). Out-of-range values are clamped, not refused: the call returns 1 and HFI_CLAMPED_TOTAL counts them. Jolt re-compresses a
   border one block wide on the low side, so a few neighbouring samples lose a little precision. */
AVER_PHYS_API int32_t aver_phys_heightfield_set_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                        const float* heights, int32_t flags);

/* Reads a w * h rectangle back (row-major, cm). Values are decompressed, so they differ slightly; holes read FLT_MAX. */
AVER_PHYS_API int32_t aver_phys_heightfield_get_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h, float* outHeights);

/* Rewrites the surface of a w * h rectangle of CELLS at cell (x0, y0), one palette index per cell (NULL = palette[0]). The palette is merged
   into the field's own; more than 256 surfaces in all is refused and nothing changes. */
AVER_PHYS_API int32_t aver_phys_heightfield_set_cell_surfaces(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                              const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount);

/* aver_phys_heightfield_info float block (HFI): cm. */
#define AVER_PHYS_HFI_SPACING_CM      0
#define AVER_PHYS_HFI_MIN_HEIGHT_CM   1   /* the lowest height an edit can encode */
#define AVER_PHYS_HFI_MAX_HEIGHT_CM   2   /* the highest */
#define AVER_PHYS_HFI_CLAMPED_TOTAL   3   /* samples clamped by edits since the shape was made (a count, as a float) */
#define AVER_PHYS_HFI_COUNT           4
/* int block (HII). */
#define AVER_PHYS_HII_COLS            0
#define AVER_PHYS_HII_ROWS            1
#define AVER_PHYS_HII_PADDED          2   /* Jolt's square side, a multiple of the block size */
#define AVER_PHYS_HII_BLOCK_SIZE      3
#define AVER_PHYS_HII_BITS            4
#define AVER_PHYS_HII_MATERIALS       5   /* surfaces in the field's own list */
#define AVER_PHYS_HII_COUNT           6

/* Writes up to min(nF, HFI_COUNT) floats and min(nI, HII_COUNT) ints; either pointer may be NULL with its count 0.
   The body's shape must be a heightfield made by this module. */
AVER_PHYS_API int32_t aver_phys_heightfield_info(int32_t body, float* outF, int32_t nF, int32_t* outI, int32_t nI);

#ifdef __cplusplus
}
#endif
