// TerrainTest -- heightfields v2 (physics_terrain_abi.h, docs/plans/wave11/SHAPES_MATERIALS.md).
//
// THE STANDARD IS OBSERVED GEOMETRY: a field built to the wrong orientation or the wrong size still returns a handle, so
// almost every check casts a ray at the middle of a known cell and reads where it lands, or drops a ball and sees whether
// it falls. The margins are tolerances: a failing
// relation is a prompt to look at the physics before loosening the number.
//
// Every field is built at spacing 100 cm and a body at the world origin, so sample (column x, row y) is at engine
// (-y * 100, x * 100) and the middle of cell (cx, cy) at (-(cy + 0.5) * 100, (cx + 0.5) * 100).
//
//   1. aver_phys_add_heightfield and the shape route give the same ray heights bit for bit,
//   2. a non-square field: the rectangle collides, its padding and surroundings do not, a ball rolls off the edge,
//   3. holes (NaN and FLT_MAX) remove the cells Jolt's diagonal rule says,
//   4. surfaces per cell: reported by a ray with the cell's coordinates, rewritten by set_cell_surfaces, capped at 256,
//   5. quantisation: bits per sample against the error get_heights reports,
//   6. edits: a raised block, the range clamp and its counter, headroom, waking a resting ball, the handle surviving,
//   7. get_heights, info, block size, and every refusal.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/physics/physics_terrain_abi.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const float kDt = 1.0f / 60.0f;
static const float kNaN = std::numeric_limits<float>::quiet_NaN();
static const int32_t kNullPointer = -2, kOutOfRange = -4, kUnsupported = -5, kInvalidArgument = -6;

static void stepN(int n) {
    for (int i = 0; i < n; ++i) aver_phys_step(kDt);
}

// ---- helpers -----------------------------------------------------------------------------------------------------------

static const float kSp = 100.0f;

struct Hf {
    int32_t shape = 0, body = 0;
};

// A static body at (ox, oy, oz) from a heightfield shape. 0 shape or body means the build was refused.
static Hf makeField(const std::vector<float>& s, int cols, int rows, const uint8_t* cellPal = nullptr, const int32_t* pal = nullptr,
                    int32_t palCount = 0, const float* p = nullptr, int32_t n = 0, float ox = 0, float oy = 0, float oz = 0) {
    Hf h;
    h.shape = aver_phys_heightfield_shape(s.data(), cols, rows, kSp, cellPal, pal, palCount, p, n);
    if (!h.shape) return h;
    const float o[3] = {ox, oy, oz};
    h.body = aver_phys_body_from_shape(h.shape, o, nullptr, AVER_PHYS_MOTION_STATIC, 0, nullptr, 0);
    return h;
}

static std::vector<float> flat(int cols, int rows, float h = 0.0f) { return std::vector<float>(static_cast<size_t>(cols) * rows, h); }

// Where a ray from above lands at engine (x, y): the hit height, or NaN for a miss.
static float rayZ(float x, float y) {
    float p[3] = {0, 0, 0};
    const int32_t b = aver_phys_raycast(x, y, 5000.0f, 0, 0, -1, 20000.0f, p, nullptr, nullptr);
    return b ? p[2] : kNaN;
}
static bool hits(float x, float y) { return !std::isnan(rayZ(x, y)); }

// Engine position of the middle of cell (cx, cy): x runs along the rows (-X), y along the columns (+Y).
static float cellX(int cy) { return -(static_cast<float>(cy) + 0.5f) * kSp; }
static float cellY(int cx) { return (static_cast<float>(cx) + 0.5f) * kSp; }

struct Detail {
    float f[AVER_PHYS_HITF_COUNT] = {};
    int32_t i[AVER_PHYS_HITI_COUNT] = {};
    int32_t body = 0;
};
static Detail rayDetail(float x, float y) {
    Detail d;
    d.body = aver_phys_raycast_detail(x, y, 5000.0f, 0, 0, -1, 20000.0f, 0xFFFFFFFFu, 0, 0, d.f, AVER_PHYS_HITF_COUNT, d.i, AVER_PHYS_HITI_COUNT);
    return d;
}

static bool info(int32_t body, float* f, int32_t* i) {
    return aver_phys_heightfield_info(body, f, AVER_PHYS_HFI_COUNT, i, AVER_PHYS_HII_COUNT) == 1;
}

static float posZ(int32_t body) {
    float p[3] = {0, 0, 0};
    aver_phys_body_position(body, p);
    return p[2];
}

static int32_t ballAt(float x, float y, float z, float r = 20.0f, float mass = 5.0f) {
    const int32_t shape = aver_phys_shape_sphere(r, 0, nullptr, 0);
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_DYNAMIC, mass, nullptr, 0);
}

// A smooth field in cm: a ripple on a gentle ramp along x.
static std::vector<float> smoothField(int cols, int rows) {
    std::vector<float> v(static_cast<size_t>(cols) * rows);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            v[static_cast<size_t>(y) * cols + x] = 60.0f * std::sin(0.37f * x) * std::cos(0.29f * y) + 8.0f * x;
    return v;
}

// ---- 1. equivalence ----------------------------------------------------------------------------------------------------

static std::vector<float> rayGrid(bool legacy) {
    const std::vector<float> s = smoothField(33, 33);
    aver_phys_init();
    if (legacy) {
        aver_phys_add_heightfield(s.data(), 33, 50.0f, 100.0f, -200.0f, 10.0f);
    } else {
        const int32_t shape = aver_phys_heightfield_shape(s.data(), 33, 33, 50.0f, nullptr, nullptr, 0, nullptr, 0);
        const float o[3] = {100.0f, -200.0f, 10.0f};
        aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_STATIC, 0, nullptr, 0);
    }
    std::vector<float> z;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) z.push_back(rayZ(-1400.0f + 200.0f * i, -150.0f + 200.0f * j));
    aver_phys_shutdown();
    return z;
}

static void testEquivalence() {
    AVER_INFO("=== aver_phys_add_heightfield and the shape route are the same field ===");
    const std::vector<float> a = rayGrid(true), b = rayGrid(false);
    bool allHit = a.size() == 64 && b.size() == 64, bitEqual = allHit;
    for (size_t k = 0; allHit && k < a.size(); ++k) {
        if (std::isnan(a[k])) allHit = false;
        if (std::memcmp(&a[k], &b[k], sizeof(float)) != 0) bitEqual = false;
    }
    check(allHit, "all 64 rays land on the legacy field");
    check(bitEqual, "the shape route gives the same 64 heights, bit for bit");

    // The legacy body is a field this module made, so its info reads.
    aver_phys_init();
    const std::vector<float> s = smoothField(33, 33);
    const int32_t body = aver_phys_add_heightfield(s.data(), 33, 50.0f, 0, 0, 0);
    float f[AVER_PHYS_HFI_COUNT];
    int32_t iv[AVER_PHYS_HII_COUNT];
    check(body != 0 && info(body, f, iv) && iv[AVER_PHYS_HII_COLS] == 33 && iv[AVER_PHYS_HII_ROWS] == 33 && near(f[AVER_PHYS_HFI_SPACING_CM], 50.0f, 1e-4f),
          "aver_phys_heightfield_info reads a field made by aver_phys_add_heightfield");
    check(aver_phys_terrain_abi_version() == AVER_PHYS_TERRAIN_ABI_VERSION, "the ABI version call answers");
    aver_phys_shutdown();
}

// ---- 2. non-square ----------------------------------------------------------------------------------------------------

static void testNonSquare() {
    AVER_INFO("=== a 40 x 16 field: the rectangle collides, the padding and the outside do not ===");
    aver_phys_init();
    const Hf h = makeField(flat(40, 16), 40, 16);
    check(h.body != 0, "a 40 x 16 field builds");

    check(near(rayZ(-750.0f, 2000.0f), 0.0f, 0.5f), "a ray inside the rectangle lands at 0");
    check(near(rayZ(cellX(14), cellY(38)), 0.0f, 0.5f), "the far corner cell (38, 14) of the rectangle collides");
    check(!hits(-2500.0f, 2000.0f), "a ray over the padded rows (x = -2500) misses");
    check(!hits(500.0f, 2000.0f), "a ray beyond the first row (x = +500) misses");
    check(!hits(-750.0f, -500.0f), "a ray before the first column (y = -500) misses");
    check(!hits(-750.0f, 4500.0f), "a ray after the last column (y = 4500) misses");

    float f[AVER_PHYS_HFI_COUNT];
    int32_t iv[AVER_PHYS_HII_COUNT];
    check(info(h.body, f, iv) && iv[AVER_PHYS_HII_COLS] == 40 && iv[AVER_PHYS_HII_ROWS] == 16 && iv[AVER_PHYS_HII_PADDED] == 40 &&
          iv[AVER_PHYS_HII_BLOCK_SIZE] == 2 && iv[AVER_PHYS_HII_BITS] == 8 && iv[AVER_PHYS_HII_MATERIALS] == 0,
          "info: 40 x 16 in a padded 40, block 2, 8 bits, no surfaces");

    // A ball rolls along the rows (-X) off the last one and falls.
    const int32_t ball = ballAt(-100.0f, 1000.0f, 40.0f);
    aver_phys_body_set_velocity(ball, -300.0f, 0, 0);
    stepN(900);
    check(posZ(ball) < -200.0f, "a ball rolling off the last row falls, z = " + f2s(posZ(ball)));
    aver_phys_shutdown();
}

// ---- 3. holes ----------------------------------------------------------------------------------------------------------

static void testHoles() {
    AVER_INFO("=== holes: NaN and FLT_MAX ===");
    aver_phys_init();
    std::vector<float> s = flat(16, 16);
    s[8 * 16 + 8] = kNaN;       // sample (8, 8)
    s[4 * 16 + 4] = FLT_MAX;    // sample (4, 4)
    const Hf h = makeField(s, 16, 16);
    check(h.body != 0, "a field with holes builds");

    // Jolt drops a cell whole when a corner on its diagonal is a hole: cells (8, 8) and (7, 7) for sample (8, 8).
    check(!hits(cellX(8), cellY(8)), "the middle of cell (8, 8) is open (the NaN sample)");
    check(!hits(cellX(7), cellY(7)), "the middle of cell (7, 7) is open too");
    check(!hits(cellX(4), cellY(4)), "the middle of cell (4, 4) is open (the FLT_MAX sample)");
    check(!hits(cellX(3), cellY(3)), "the middle of cell (3, 3) is open too");
    check(near(rayZ(cellX(10), cellY(2)), 0.0f, 0.5f), "a cell far from both holes still collides");
    check(near(rayZ(cellX(12), cellY(12)), 0.0f, 0.5f), "so does cell (12, 12)");

    const int32_t ball = ballAt(cellX(8), cellY(8), 100.0f);
    stepN(120);
    check(posZ(ball) < -200.0f, "a ball over the hole falls through it, z = " + f2s(posZ(ball)));
    const int32_t onSolid = ballAt(cellX(12), cellY(12), 100.0f);
    stepN(120);
    check(near(posZ(onSolid), 20.0f, 3.0f), "a ball over solid cells rests on the field, z = " + f2s(posZ(onSolid)));

    std::vector<float> allHoles(16 * 16, kNaN);
    check(aver_phys_heightfield_shape(allHoles.data(), 16, 16, kSp, nullptr, nullptr, 0, nullptr, 0) == 0, "a field of nothing but holes is refused");
    aver_phys_shutdown();
}

// ---- 4. surfaces per cell ----------------------------------------------------------------------------------------------

static void testCellSurfaces() {
    AVER_INFO("=== a surface per cell ===");
    aver_phys_init();
    const int32_t sa = aver_phys_surface_id("TerrainA"), sb = aver_phys_surface_id("TerrainB"), sc = aver_phys_surface_id("TerrainC");
    const int32_t palette[3] = {sa, sb, sc};
    std::vector<uint8_t> cells(15 * 15);
    for (int cy = 0; cy < 15; ++cy)
        for (int cx = 0; cx < 15; ++cx) cells[static_cast<size_t>(cy) * 15 + cx] = static_cast<uint8_t>(cx < 5 ? 0 : (cx < 10 ? 1 : 2));
    Hf h = makeField(flat(16, 16), 16, 16, cells.data(), palette, 3);
    check(h.body != 0, "a 16 x 16 field with a palette of 3 builds");

    const Detail a = rayDetail(cellX(3), cellY(2));
    const Detail b = rayDetail(cellX(3), cellY(7));
    const Detail c = rayDetail(cellX(11), cellY(12));
    check(a.body == h.body && a.i[AVER_PHYS_HITI_SURFACE] == sa && a.i[AVER_PHYS_HITI_CELL_X] == 2 && a.i[AVER_PHYS_HITI_CELL_Y] == 3,
          "cell (2, 3) reports TerrainA and its own coordinates");
    check(b.i[AVER_PHYS_HITI_SURFACE] == sb && b.i[AVER_PHYS_HITI_CELL_X] == 7 && b.i[AVER_PHYS_HITI_CELL_Y] == 3, "cell (7, 3) reports TerrainB");
    check(c.i[AVER_PHYS_HITI_SURFACE] == sc && c.i[AVER_PHYS_HITI_CELL_X] == 12 && c.i[AVER_PHYS_HITI_CELL_Y] == 11, "cell (12, 11) reports TerrainC");

    float f[AVER_PHYS_HFI_COUNT];
    int32_t iv[AVER_PHYS_HII_COUNT];
    check(info(h.body, f, iv) && iv[AVER_PHYS_HII_MATERIALS] == 3, "info: 3 surfaces in the field");

    // A rectangle rewritten: cells (0..3, 0..3) become a new surface, nothing else moves.
    const int32_t sd = aver_phys_surface_id("TerrainD");
    const uint8_t zeros[16] = {};
    check(aver_phys_heightfield_set_cell_surfaces(h.body, 0, 0, 4, 4, zeros, &sd, 1) == 1, "set_cell_surfaces rewrites a 4 x 4 block");
    check(rayDetail(cellX(1), cellY(1)).i[AVER_PHYS_HITI_SURFACE] == sd, "cell (1, 1) now reports TerrainD");
    check(rayDetail(cellX(3), cellY(7)).i[AVER_PHYS_HITI_SURFACE] == sb && rayDetail(cellX(11), cellY(12)).i[AVER_PHYS_HITI_SURFACE] == sc,
          "cells outside the block keep their surface");
    check(rayDetail(cellX(3), cellY(4)).i[AVER_PHYS_HITI_SURFACE] == sa, "so does cell (4, 3), next to the block");
    check(info(h.body, f, iv) && iv[AVER_PHYS_HII_MATERIALS] == 4, "info: 4 surfaces now");
    check(aver_phys_heightfield_set_cell_surfaces(h.body, 14, 14, 2, 2, zeros, &sd, 1) == 0 && aver_phys_last_error() == kOutOfRange,
          "a block past the last cell is OutOfRange");
    const uint8_t badIndex[1] = {3};
    check(aver_phys_heightfield_set_cell_surfaces(h.body, 0, 0, 1, 1, badIndex, &sd, 1) == 0 && aver_phys_last_error() == kInvalidArgument,
          "a palette index outside the palette is InvalidArgument");
    aver_phys_shutdown();

    // A field built with NO surfaces: cells read 0 until rewritten, and the untouched ones keep reading 0.
    aver_phys_init();
    h = makeField(flat(16, 16), 16, 16);
    check(rayDetail(cellX(5), cellY(5)).i[AVER_PHYS_HITI_SURFACE] == 0, "with no palette every cell reports surface 0");
    check(aver_phys_heightfield_set_cell_surfaces(h.body, 2, 2, 3, 3, nullptr, &sa, 1) == 1, "set_cell_surfaces on a field built without a palette works");
    check(rayDetail(cellX(3), cellY(3)).i[AVER_PHYS_HITI_SURFACE] == sa, "cell (3, 3) is TerrainA");
    check(rayDetail(cellX(10), cellY(10)).i[AVER_PHYS_HITI_SURFACE] == 0, "cell (10, 10) still reports surface 0");
    aver_phys_shutdown();

    // The 256 limit: at creation, and in all after a merge.
    aver_phys_init();
    std::vector<int32_t> many(200), more(100);
    for (int i = 0; i < 200; ++i) many[static_cast<size_t>(i)] = 1000 + i;
    for (int i = 0; i < 100; ++i) more[static_cast<size_t>(i)] = 5000 + i;
    h = makeField(flat(16, 16), 16, 16, nullptr, many.data(), 200);
    check(h.body != 0, "a palette of 200 builds");
    const uint8_t one[1] = {0};
    check(aver_phys_heightfield_set_cell_surfaces(h.body, 0, 0, 1, 1, one, more.data(), 100) == 0, "merging 100 more surfaces (300 in all) is refused");
    check(info(h.body, f, iv) && iv[AVER_PHYS_HII_MATERIALS] == 200, "and the field still holds 200");
    std::vector<int32_t> tooMany(257, 7);
    check(aver_phys_heightfield_shape(flat(16, 16).data(), 16, 16, kSp, nullptr, tooMany.data(), 257, nullptr, 0) == 0, "a palette of 257 is refused at creation");
    aver_phys_shutdown();
}

// ---- 5. quantisation ---------------------------------------------------------------------------------------------------

static float maxError(int bits, const std::vector<float>& in, int cols, int rows) {
    const float p[4] = {-1.0f, static_cast<float>(bits), -1.0f, -1.0f};
    const Hf h = makeField(in, cols, rows, nullptr, nullptr, 0, p, 4);
    if (!h.body) return 1.0e9f;
    std::vector<float> out(in.size());
    if (aver_phys_heightfield_get_heights(h.body, 0, 0, cols, rows, out.data()) != 1) return 1.0e9f;
    float worst = 0.0f;
    for (size_t k = 0; k < in.size(); ++k) worst = std::fmax(worst, std::fabs(out[k] - in[k]));
    aver_phys_remove_body(h.body);
    return worst;
}

static void testQuantisation() {
    AVER_INFO("=== bits per sample against the error get_heights reports ===");
    aver_phys_init();
    const std::vector<float> s = smoothField(32, 32);
    const float e16 = maxError(16, s, 32, 32), e8 = maxError(8, s, 32, 32), e4 = maxError(4, s, 32, 32);
    check(e16 < 0.1f, "16 bits: the largest error is " + f2s(e16) + " cm");
    check(e8 < 1.0f, "8 bits: the largest error is " + f2s(e8) + " cm");
    check(e4 < 6.0f, "4 bits: the largest error is " + f2s(e4) + " cm");
    check(e4 > e16, "fewer bits cost more error");
    aver_phys_shutdown();
}

// ---- 6. edits ----------------------------------------------------------------------------------------------------------

// A ramp along x (5 cm a column), so the field has a height range to edit within.
static std::vector<float> rampField(int cols, int rows) {
    std::vector<float> v(static_cast<size_t>(cols) * rows);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) v[static_cast<size_t>(y) * cols + x] = 5.0f * x;
    return v;
}

static void testEdits() {
    AVER_INFO("=== edits: raise, clamp, headroom, wake ===");
    aver_phys_init();
    // With headroom the encodable range reaches 600 cm each way.
    const float roomy[4] = {-1.0f, -1.0f, -1.0f, 600.0f};
    Hf h = makeField(rampField(32, 32), 32, 32, nullptr, nullptr, 0, roomy, 4);
    check(h.body != 0, "a ramp with edit headroom builds");
    const float before = rayZ(cellX(12), cellY(12));
    check(near(before, 5.0f * 12.5f, 1.0f), "the cell (12, 12) height is the ramp's 62.5 cm, got " + f2s(before));

    // 5 x 5 samples at (10, 10), raised by 100: read the old values, add, write.
    std::vector<float> region(25);
    check(aver_phys_heightfield_get_heights(h.body, 10, 10, 5, 5, region.data()) == 1, "get_heights reads the block");
    for (float& v : region) v += 100.0f;
    check(aver_phys_heightfield_set_heights(h.body, 10, 10, 5, 5, region.data(), 0) == 1, "set_heights raises it by 100 cm");
    const float after = rayZ(cellX(12), cellY(12));
    check(near(after, before + 100.0f, 1.5f), "a ray inside the block moved up by 100 cm, now " + f2s(after));
    check(near(rayZ(cellX(12), cellY(20)), 5.0f * 20.5f, 1.5f), "a cell on the same row outside the block did not move");
    check(near(rayZ(cellX(20), cellY(12)), 5.0f * 12.5f, 1.5f), "neither did one on the same column");
    // The block edge is not block-aligned (columns 10 to 14): the sample just past it keeps its value.
    float next[1] = {0};
    aver_phys_heightfield_get_heights(h.body, 15, 12, 1, 1, next);
    check(near(next[0], 75.0f, 1.0f), "the sample just past the block (15, 12) is still about 75 cm, got " + f2s(next[0]));

    float f[AVER_PHYS_HFI_COUNT];
    int32_t iv[AVER_PHYS_HII_COUNT];
    check(info(h.body, f, iv) && f[AVER_PHYS_HFI_CLAMPED_TOTAL] == 0.0f && f[AVER_PHYS_HFI_MIN_HEIGHT_CM] < -500.0f && f[AVER_PHYS_HFI_MAX_HEIGHT_CM] > 700.0f,
          "info: nothing clamped, the range reaches past -500 and +700");

    // A crater 500 cm below the old minimum is possible with the headroom.
    std::vector<float> crater(25, -500.0f);
    check(aver_phys_heightfield_set_heights(h.body, 2, 20, 5, 5, crater.data(), 0) == 1, "a -500 cm crater is accepted with headroom");
    check(near(rayZ(cellX(22), cellY(4)), -500.0f, 2.0f), "and the ray reads about -500, got " + f2s(rayZ(cellX(22), cellY(4))));
    check(info(h.body, f, iv) && f[AVER_PHYS_HFI_CLAMPED_TOTAL] == 0.0f, "nothing was clamped");
    check(near(rayZ(cellX(12), cellY(12)), after, 0.05f), "the body handle still works after the clone swap and answers as before");
    aver_phys_shutdown();

    // Without headroom the range is the old minimum to maximum, and the same crater is clamped and counted.
    aver_phys_init();
    h = makeField(rampField(32, 32), 32, 32);
    std::vector<float> deep(25, -500.0f);
    check(aver_phys_heightfield_set_heights(h.body, 2, 20, 5, 5, deep.data(), 0) == 1, "an out-of-range edit still returns 1 (it clamps)");
    check(info(h.body, f, iv) && f[AVER_PHYS_HFI_CLAMPED_TOTAL] >= 25.0f, "HFI_CLAMPED_TOTAL counts the 25 clamped samples, got " + f2s(f[AVER_PHYS_HFI_CLAMPED_TOTAL]));
    const float floorZ = rayZ(cellX(22), cellY(4));
    check(floorZ > -50.0f && floorZ < 40.0f, "the crater bottoms out near the old minimum, got " + f2s(floorZ));
    aver_phys_shutdown();
}

// A plateau (samples 8..23 at 200 cm) with a ball asleep on top; the plateau is then flattened.
static float flattenAndWatch(int32_t flags) {
    aver_phys_init();
    std::vector<float> s = flat(32, 32);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x) s[static_cast<size_t>(y) * 32 + x] = 200.0f;
    const Hf h = makeField(s, 32, 32);
    const int32_t ball = ballAt(-1600.0f, 1600.0f, 300.0f, 25.0f);
    stepN(420);   // lands on the plateau and goes to sleep
    const std::vector<float> down(16 * 16, 0.0f);
    aver_phys_heightfield_set_heights(h.body, 8, 8, 16, 16, down.data(), flags);
    stepN(120);
    const float z = posZ(ball);
    aver_phys_shutdown();
    return z;
}

static void testWake() {
    AVER_INFO("=== flattening the ground under a resting ball ===");
    const float woken = flattenAndWatch(AVER_PHYS_HF_WAKE);
    check(woken < 60.0f, "with HF_WAKE the ball falls to the new ground, z = " + f2s(woken));
    const float asleep = flattenAndWatch(0);
    check(asleep > 200.0f, "without it the sleeping ball stays where it was, z = " + f2s(asleep));
}

// ---- 7. get_heights, info, block size, refusals -----------------------------------------------------------------------

static void testReadBack() {
    AVER_INFO("=== get_heights and block size ===");
    aver_phys_init();
    std::vector<float> s = smoothField(16, 16);
    s[5 * 16 + 5] = kNaN;
    const float p16[4] = {-1.0f, 16.0f, -1.0f, -1.0f};
    const Hf h = makeField(s, 16, 16, nullptr, nullptr, 0, p16, 4);
    std::vector<float> out(16 * 16);
    check(h.body != 0 && aver_phys_heightfield_get_heights(h.body, 0, 0, 16, 16, out.data()) == 1, "the whole field reads back");
    float worst = 0.0f;
    for (size_t k = 0; k < out.size(); ++k)
        if (k != 5 * 16 + 5) worst = std::fmax(worst, std::fabs(out[k] - s[k]));
    check(worst < 0.1f, "values come back within quantisation, worst " + f2s(worst) + " cm");
    check(out[5 * 16 + 5] == FLT_MAX, "the hole reads back as FLT_MAX");
    float one[6];
    check(aver_phys_heightfield_get_heights(h.body, 3, 4, 3, 2, one) == 1 && near(one[0], s[4 * 16 + 3], 0.1f) && one[5] == FLT_MAX,
          "a sub-rectangle that is not block aligned reads, and its corner is the hole");
    aver_phys_shutdown();

    // Block size 2 against 8: the same rays, to quantisation.
    float maxDiff = 0.0f;
    std::vector<float> z2, z8;
    for (int bs : {2, 8}) {
        aver_phys_init();
        const float p[4] = {static_cast<float>(bs), 16.0f, -1.0f, -1.0f};
        const Hf hb = makeField(smoothField(33, 33), 33, 33, nullptr, nullptr, 0, p, 4);
        std::vector<float>& z = bs == 2 ? z2 : z8;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) z.push_back(rayZ(cellX(3 + 4 * i), cellY(3 + 4 * j)));
        float f[AVER_PHYS_HFI_COUNT];
        int32_t iv[AVER_PHYS_HII_COUNT];
        check(hb.body != 0 && info(hb.body, f, iv) && iv[AVER_PHYS_HII_BLOCK_SIZE] == bs, "block size " + std::to_string(bs) + " builds and reports");
        aver_phys_shutdown();
    }
    for (size_t k = 0; k < z2.size() && k < z8.size(); ++k) maxDiff = std::isnan(z2[k]) || std::isnan(z8[k]) ? 1e9f : std::fmax(maxDiff, std::fabs(z2[k] - z8[k]));
    check(z2.size() == 36 && z8.size() == 36 && maxDiff < 0.1f, "block 2 and block 8 answer the same rays, worst difference " + f2s(maxDiff) + " cm");
}

static void testRefusals() {
    AVER_INFO("=== refusals ===");
    aver_phys_init();
    const std::vector<float> s = flat(16, 16);
    check(aver_phys_heightfield_shape(s.data(), 1, 16, kSp, nullptr, nullptr, 0, nullptr, 0) == 0 && aver_phys_last_error() == kInvalidArgument, "1 column is refused");
    check(aver_phys_heightfield_shape(s.data(), 16385, 2, kSp, nullptr, nullptr, 0, nullptr, 0) == 0, "16385 columns are refused");
    check(aver_phys_heightfield_shape(s.data(), 16, 16, 0.0f, nullptr, nullptr, 0, nullptr, 0) == 0, "a spacing of 0 is refused");
    check(aver_phys_heightfield_shape(s.data(), 16, 16, -5.0f, nullptr, nullptr, 0, nullptr, 0) == 0, "a negative spacing is refused");
    check(aver_phys_heightfield_shape(nullptr, 16, 16, kSp, nullptr, nullptr, 0, nullptr, 0) == 0 && aver_phys_last_error() == kNullPointer, "no samples is NullPointer");
    float tooLong[5] = {};
    check(aver_phys_heightfield_shape(s.data(), 16, 16, kSp, nullptr, nullptr, 0, tooLong, 5) == 0, "a parameter block longer than the header defines is refused");
    const float badBlock[1] = {9.0f};
    check(aver_phys_heightfield_shape(s.data(), 16, 16, kSp, nullptr, nullptr, 0, badBlock, 1) == 0, "a block size of 9 is refused");
    const float badBits[2] = {-1.0f, 17.0f};
    check(aver_phys_heightfield_shape(s.data(), 16, 16, kSp, nullptr, nullptr, 0, badBits, 2) == 0, "17 bits per sample are refused");
    const int32_t pal[1] = {5};
    const uint8_t badCell[15 * 15] = {1};
    check(aver_phys_heightfield_shape(s.data(), 16, 16, kSp, badCell, pal, 1, nullptr, 0) == 0, "a cell naming palette entry 1 of 1 is refused");

    const Hf h = makeField(s, 16, 16);
    float out[4];
    check(aver_phys_heightfield_set_heights(h.body, 14, 14, 4, 4, out, 0) == 0 && aver_phys_last_error() == kOutOfRange, "an edit past the last sample is OutOfRange");
    check(aver_phys_heightfield_set_heights(h.body, -1, 0, 2, 2, out, 0) == 0, "a negative origin is refused");
    check(aver_phys_heightfield_set_heights(h.body, 0, 0, 2, 2, out, 2) == 0 && aver_phys_last_error() == kInvalidArgument, "an unknown flag is InvalidArgument");
    check(aver_phys_heightfield_set_heights(h.body, 0, 0, 2, 2, nullptr, 0) == 0 && aver_phys_last_error() == kNullPointer, "no heights is NullPointer");

    // Not a heightfield: a box, and a scaled heightfield.
    const int32_t box = aver_phys_add_static_box(5000, 5000, 0, 50, 50, 50);
    check(aver_phys_heightfield_set_heights(box, 0, 0, 2, 2, out, 0) == 0 && aver_phys_last_error() == kUnsupported, "an edit on a box is Unsupported");
    check(aver_phys_heightfield_info(box, nullptr, 0, nullptr, 0) == 0, "info on a box is refused");
    const int32_t scaled = aver_phys_add_mesh_shape_body(h.shape, 20000, 0, 0, 0, 0, 0, 1, 2, 2, 2);
    check(scaled != 0 && aver_phys_heightfield_set_heights(scaled, 0, 0, 2, 2, out, 0) == 0 && aver_phys_last_error() == kUnsupported,
          "a scaled heightfield cannot be edited");
    check(aver_phys_heightfield_set_heights(9999, 0, 0, 2, 2, out, 0) == 0, "an unknown body is refused");

    // A heightfield cannot be dynamic.
    const float o[3] = {0, 0, 500};
    check(aver_phys_body_from_shape(h.shape, o, nullptr, AVER_PHYS_MOTION_DYNAMIC, 10.0f, nullptr, 0) == 0 && aver_phys_last_error() == kUnsupported,
          "a dynamic body from a heightfield is Unsupported");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("heightfields v2: non-square, holes, surfaces per cell, edits");

    testEquivalence();
    testNonSquare();
    testHoles();
    testCellSurfaces();
    testQuantisation();
    testEdits();
    testWake();
    testReadBack();
    testRefusals();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
