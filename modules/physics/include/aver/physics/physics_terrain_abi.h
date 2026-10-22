#pragma once
// Aver.Physics -- HEIGHTFIELDS v2: a heightfield that can be non-square, have holes, carry a physical surface per
// cell, and be edited while the body runs.
//
// aver_phys_add_heightfield (physics_abi.h) stays exactly as it was and is now built through the same code with an
// empty parameter block, so a terrain made either way is bit for bit the same. This header adds the SHAPE route:
// aver_phys_heightfield_shape returns a handle in the generic shape table (physics_shape_builder_abi.h), and
// aver_phys_body_from_shape (static, mass 0) makes the body. A heightfield shape is static only; a dynamic request
// is refused there.
//
// UNITS AND ORIENTATION ARE aver_phys_add_heightfield's: centimetres, and `samples` is cols * rows row-major in the
// v1 PHYSICS ORDER, so sample (column x, row y) lands at engine (cx - y * spacing, cy + x * spacing, cz + height) when
// the body is created at origin (cx, cy, cz). A cell is the square between four neighbouring samples: (cols - 1) *
// (rows - 1) of them, cell (x, y) between columns x..x+1 and rows y..y+1.
//
// HOLES. A sample that is NaN, infinite or FLT_MAX is a hole. Jolt's rule: a cell is dropped whole when a corner on its
// diagonal is a hole (the diagonal runs from a cell's (x, y) corner to its (x+1, y+1) corner), and a hole on either other
// corner drops one of the cell's two triangles. So a hole at sample (x, y) removes cells (x, y) and (x-1, y-1) entirely
// and half of cells (x-1, y) and (x, y-1). Nothing collides there and a ray passes through.
//
// NON-SQUARE. Jolt's field is square. The shape is built N x N with N = max(cols, rows) and the extra samples are
// holes, so memory is N^2 samples whatever the rectangle: an 8:1 strip costs eight times a square of its short side.
//
// CELL SURFACES. `cellPaletteIndex` is one byte per cell indexing `palette` (up to 256 surface ids, 0 = none); NULL
// gives every cell palette[0]. A hit on the field reports the cell's surface and its (x, y) through
// aver_phys_raycast_detail (physics_surface_abi.h).
//
// EDITS go through Jolt's documented race-free route: the shape is cloned, the clone is edited and swapped onto the
// body with SetShape, so a query in flight finishes on the old one and the body handle keeps working. A clone costs a
// copy of the field per call: batch an edit per frame. A heightfield edit invalidates every raw sub-shape id taken
// before it (the cell coordinates stay valid). Edits are between-step calls on the thread that steps.
//
// Failures return 0 and set aver_phys_last_error.
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

/* Builds a heightfield shape from cols * rows samples (2 to 16384 each) spaced spacingCm apart. QUANTISATION: Jolt
   compresses each block relative to its own height range, then edits clamp to the global range fixed here. With
   headroom 0 that range is the lowest to the highest sample, so a crater below the lowest sample cannot be made later;
   EDIT_HEADROOM_CM widens the range. `palette` is up to 256 surface ids. Returns a shape handle. */
AVER_PHYS_API int32_t aver_phys_heightfield_shape(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                                  const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                                  const float* p, int32_t n);

/* Sets a rectangle of w * h samples at column x0, row y0 on the heightfield body (heights row-major, cm, NaN = hole).
   The body's shape must BE the heightfield (a scaled one is refused). Values outside the encoded range are clamped, not
   refused: the call returns 1 and HFI_CLAMPED_TOTAL counts them. Jolt re-compresses a border one block wide on the low
   side of the rectangle, so a few neighbouring samples lose a little precision. flags is AVER_PHYS_HF_*. */
AVER_PHYS_API int32_t aver_phys_heightfield_set_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                        const float* heights, int32_t flags);

/* Reads a rectangle of w * h samples back (row-major, cm). Values are decompressed, so they differ slightly from what
   the field was made with; holes read FLT_MAX. */
AVER_PHYS_API int32_t aver_phys_heightfield_get_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h, float* outHeights);

/* Rewrites the surface of a rectangle of w * h CELLS at cell (x0, y0): one palette index per cell (NULL = palette[0]).
   The palette is merged into the field's own; more than 256 surfaces in all is refused and nothing changes. A field
   built with no palette reports surface 0 for the cells this call does not touch. */
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
