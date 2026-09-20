#include "NavBakeCommand.hpp"

#include "aver/core/Log.hpp"
// Only reachable, and only needed, where a scene::World is actually sampled below -- see the
// AVER_MODULE_SCENE guard around measureWorld/bakeNavigation for why the rest of this file, which
// never names the type, does not carry this include at all.
#if AVER_MODULE_SCENE
#include "aver/scene/World.hpp"
#endif

#include <algorithm>
#include <cmath>

#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif

namespace aver::editor {

// ---------------------------------------------------------------------------------------------
// measuring
// ---------------------------------------------------------------------------------------------

// GUARDED ON SCENE: the header explains why the declaration disappears with the module off; here
// it is the body that would not compile, since walking `world.count()`/`world.at(i)` IS the
// function -- there is nothing left to measure without a scene::World to measure it in.
#if AVER_MODULE_SCENE
NavExtent measureWorld(scene::World& world, const NavBakeSettings& s) {
    NavExtent e;
    if (s.cellSizeCm <= 0.0f) return e;

    f32 minX = 0.0f, minY = 0.0f, minZ = 0.0f, maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    bool any = false;
    const u32 n = world.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity ent = world.at(i);
        if (!world.valid(ent)) continue;
        const Mat4& m = world.worldMatrix(ent);
        const f32 x = m.m[3][0], y = m.m[3][1], z = m.m[3][2];
        // A NaN or an infinity in a transform would poison every comparison below and produce an
        // extent that passes the cell cap by being nonsense rather than by being small. Skipping is
        // right: one broken entity should not decide the size of the bake.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;

        // THE ORIGIN IS NOT THE EXTENT, and using it alone is how this got its first bug: a level
        // whose floor is one big slab centred at (0,0) has every entity origin at the middle, so the
        // measured area came out as the margin and nothing else. The SCALE is the right stand-in
        // here because it is literally what physics used -- LevelInstance builds a placement's
        // static body as an axis-aligned box from its scale -- so this covers exactly the geometry
        // the bake is about to raycast against, no more and no less.
        //
        // Row lengths, not m[i][i]: a rotated placement puts its scale across a whole row. The box
        // collider ITSELF ignores that rotation (LevelInstance.cpp does not apply it), so this is an
        // over-estimate for a rotated object -- which is the safe direction, since a bake that
        // covers slightly too much ground wastes rays and a bake that covers too little loses floor.
        const auto rowLen = [&](int r) {
            return std::sqrt(m.m[r][0]*m.m[r][0] + m.m[r][1]*m.m[r][1] + m.m[r][2]*m.m[r][2]);
        };
        f32 ex = rowLen(0), ey = rowLen(1), ez = rowLen(2);
        if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(ez)) { ex = ey = ez = 0.0f; }

        if (!any) {
            minX = x - ex; maxX = x + ex;
            minY = y - ey; maxY = y + ey;
            minZ = z - ez; maxZ = z + ez;
            any = true;
            continue;
        }
        minX = std::fmin(minX, x - ex); maxX = std::fmax(maxX, x + ex);
        minY = std::fmin(minY, y - ey); maxY = std::fmax(maxY, y + ey);
        minZ = std::fmin(minZ, z - ez); maxZ = std::fmax(maxZ, z + ez);
    }
    if (!any) return e;

    minX -= s.marginCm; minY -= s.marginCm;
    maxX += s.marginCm; maxY += s.marginCm;

    e.originXCm = std::floor(minX / s.cellSizeCm) * s.cellSizeCm;
    e.originYCm = std::floor(minY / s.cellSizeCm) * s.cellSizeCm;
    const f32 spanX = maxX - e.originXCm;
    const f32 spanY = maxY - e.originYCm;
    e.widthCells = static_cast<u32>(std::ceil(spanX / s.cellSizeCm));
    e.heightCells = static_cast<u32>(std::ceil(spanY / s.cellSizeCm));
    if (e.widthCells == 0) e.widthCells = 1;
    if (e.heightCells == 0) e.heightCells = 1;

    // The probe starts above everything and reaches below everything, with the margin applied to Z
    // as well. The margin is not redundant with the extents above: a class placement has no scale
    // worth the name and no collider either, and the landscape has neither an entity nor a
    // transform in this world at all, so Z has to allow for ground the loop above never saw.
    e.topZCm = maxZ + s.marginCm;
    e.depthCm = (e.topZCm - minZ) + s.marginCm;
    return e;
}
#endif  // AVER_MODULE_SCENE

// ---------------------------------------------------------------------------------------------
// baking against the live physics scene
// ---------------------------------------------------------------------------------------------

// GUARDED ON SCENE, wrapping physicsFloor/physicsHeadroom along with bakeNavigation itself rather
// than bakeNavigation alone: both are static helpers that exist only to be passed into
// synapse::bakeNav from inside this function, so leaving them compiled while their one caller is
// not would trade a missing-scene error for an unused-static-function one instead.
#if AVER_MODULE_SCENE
#if AVER_MODULE_PHYSICS
static bool physicsFloor(void* /*user*/, f32 x, f32 y, f32 topZ, f32 depth,
                         f32* outZ, f32* outNormalZ) {
    f32 point[3] = {0, 0, 0}, normal[3] = {0, 0, 1};
    int32_t entity = 0;
    // THE RETURN VALUE IS THE HIT TEST, not outEntity: a landscape hit is ownerless and comes back
    // with entity 0 while still being perfectly solid ground. Reading outEntity here would make
    // every terrain cell unwalkable and the bug would look like "the navmesh has holes in it".
    if (!aver_phys_raycast(x, y, topZ, 0.0f, 0.0f, -1.0f, depth, point, normal, &entity))
        return false;
    *outZ = point[2];
    *outNormalZ = normal[2];
    return true;
}

static bool physicsHeadroom(void* /*user*/, f32 x, f32 y, f32 z, f32 radiusCm, f32 heightCm) {
    // A capsule standing here has its lower sphere centred at z + r and its upper at z + h - r, so
    // the clear space to test is the sweep between them. The 1 cm lift is not a fudge factor: a
    // sphere resting exactly on the floor is touching it, and a sweep starting in contact reports a
    // hit against the ground the agent is standing on.
    const f32 lift = 1.0f;
    const f32 span = heightCm - 2.0f * radiusCm - lift;
    if (span <= 0.0f) return true;   // an agent shorter than it is wide needs no headroom test
    f32 point[3] = {0, 0, 0}, normal[3] = {0, 0, 1};
    const int32_t hit = aver_phys_sphere_cast(x, y, z + radiusCm + lift, 0.0f, 0.0f, 1.0f,
                                              span, radiusCm, point, normal);
    return hit == 0;
}
#endif

bool bakeNavigation(scene::World& world, const NavBakeSettings& s, fmt::OcNavData& out,
                    synapse::BakeStats* stats, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
#if !AVER_MODULE_PHYSICS
    (void)world; (void)s; (void)out; (void)stats;
    return fail("this build has no physics module, and the bake samples physics");
#else
    const NavExtent e = measureWorld(world, s);
    if (!e.valid()) return fail("nothing in this level to bake navigation for");

    const u64 cells = static_cast<u64>(e.widthCells) * e.heightCells;
    if (cells > s.maxCells) {
        if (why) {
            *why = "bake area is " + std::to_string(e.widthCells) + " x " +
                   std::to_string(e.heightCells) + " cells, over the " + std::to_string(s.maxCells) +
                   " limit -- raise the cell size, or check for an entity dropped far from the level";
        }
        return false;
    }

    synapse::BakeParams p;
    p.originXCm = e.originXCm;
    p.originYCm = e.originYCm;
    p.cellSizeCm = s.cellSizeCm;
    p.widthCells = e.widthCells;
    p.heightCells = e.heightCells;
    p.sampleTopZCm = e.topZCm;
    p.sampleDepthCm = e.depthCm;
    p.agentRadiusCm = s.agentRadiusCm;
    p.agentHeightCm = s.agentHeightCm;
    p.maxSlopeDeg = s.maxSlopeDeg;
    p.maxStepCm = s.maxStepCm;

    AVER_INFO("[Editor] baking navigation: {} x {} cells at {} cm, origin ({:.0f}, {:.0f}), "
              "probing from z={:.0f} down {:.0f}",
              e.widthCells, e.heightCells, s.cellSizeCm, e.originXCm, e.originYCm,
              e.topZCm, e.depthCm);

    synapse::BakeStats st;
    if (!synapse::bakeNav(p, physicsFloor, physicsHeadroom, nullptr, out, &st, why)) return false;
    if (stats) *stats = st;

    // A bake that found no floor ANYWHERE is a bake against an empty physics scene, which is what
    // happens when the level's bodies have not been built yet. It writes a perfectly valid file full
    // of nothing, so saying so here is the only place anyone finds out.
    if (st.cellsWalkable == 0)
        AVER_WARN("[Editor] navigation bake found no walkable ground at all -- is physics running "
                  "and are this level's collision bodies built?");
    return true;
#endif
}
#endif  // AVER_MODULE_SCENE

std::string navPathForLevel(const std::string& levelPath) {
    if (levelPath.empty()) return {};
    const usize slash = levelPath.find_last_of("/\\");
    const usize dot = levelPath.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExt ? levelPath.substr(0, dot) : levelPath) + ".ocnav";
}

// ---------------------------------------------------------------------------------------------
// the overlay
// ---------------------------------------------------------------------------------------------

// A region's colour: a HUE, not three independent channels.
//
// The first version hashed the id into r, g and b separately and every region came out a pale
// pastel, because three uniform channels average to grey and the brightness floor pushed them all
// toward white. Two regions differed by a few percent per channel, which is invisible on a lit
// floor -- and telling two regions apart at a glance is the ONLY thing this overlay is for.
//
// Hues advance by the golden ratio, so consecutive ids land as far apart on the wheel as a sequence
// can, and full saturation keeps them apart after the scene's own lighting has washed over them.
// Deterministic in the id, so the same level bakes to the same picture twice and a screenshot diff
// means something. Region 0 never reaches here -- it is the unwalkable id.
static void regionColour(u16 region, f32& r, f32& g, f32& b) {
    const f32 hue = std::fmod(static_cast<f32>(region) * 0.61803399f, 1.0f) * 6.0f;
    const f32 sat = 0.85f, val = 1.0f;
    const int sector = static_cast<int>(hue) % 6;
    const f32 f = hue - std::floor(hue);
    const f32 p = val * (1.0f - sat);
    const f32 q = val * (1.0f - sat * f);
    const f32 t = val * (1.0f - sat * (1.0f - f));
    switch (sector) {
        case 0:  r = val; g = t;   b = p;   break;
        case 1:  r = q;   g = val; b = p;   break;
        case 2:  r = p;   g = val; b = t;   break;
        case 3:  r = p;   g = q;   b = val; break;
        case 4:  r = t;   g = p;   b = val; break;
        default: r = val; g = p;   b = q;   break;
    }
}

std::vector<rhi::LineVertex> buildNavOverlay(const fmt::OcNavData& nav, bool regionColours) {
    std::vector<rhi::LineVertex> out;
    if (!nav.valid() || nav.cells.empty()) return out;

    // Lifted off the floor, or the overlay z-fights the ground it is describing and the result
    // looks like a rendering bug rather than a navmesh.
    const f32 lift = 3.0f;
    const f32 c = nav.cellSizeCm;
    const f32 inset = c * 0.08f;   // a visible gap between cells, so the grid reads as cells

    out.reserve(static_cast<usize>(nav.cells.size()) * 8);
    for (u32 y = 0; y < nav.heightCells; ++y) {
        for (u32 x = 0; x < nav.widthCells; ++x) {
            const fmt::OcNavCell* cell = nav.at(x, y);
            if (!cell || (cell->flags & fmt::kOcNavWalkable) == 0) continue;

            f32 r = 0.25f, g = 0.85f, b = 0.35f;
            if (regionColours) regionColour(cell->regionId, r, g, b);

            const f32 x0 = nav.originXCm + static_cast<f32>(x) * c + inset;
            const f32 x1 = nav.originXCm + static_cast<f32>(x + 1) * c - inset;
            const f32 y0 = nav.originYCm + static_cast<f32>(y) * c + inset;
            const f32 y1 = nav.originYCm + static_cast<f32>(y + 1) * c - inset;
            const f32 z = cell->floorZCm + lift;

            const f32 corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) & 3;
                out.push_back({corners[i][0], corners[i][1], z, r, g, b});
                out.push_back({corners[j][0], corners[j][1], z, r, g, b});
            }
        }
    }
    return out;
}

} // namespace aver::editor
