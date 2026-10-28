// RasterLightsTest -- the CPU half of the raster local lights (aver/voxi/RasterLights.hpp, G4 R2): the capped, culled and ranked light list,
// Godot's attenuation curves against its documented formulas, the energy calibration, the shadow-tile budget and atlas packing, and the tile
// matrices (a point on a spot's axis lands in the middle of its tile at the depth the shader compares against). The HLSL is what runs; its text
// is read at the end to check it still names the constants and fields this header packs for.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/GodotLight.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/voxi/RasterLights.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::voxi;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f64 a, f64 b, f64 rel = 1e-4, f64 abs = 1e-6) { return std::fabs(a - b) <= std::max(abs, rel * std::fabs(b)); }

namespace {

SceneLight lamp(f32 x, f32 y, f32 z, f32 cd = 33333.0f, bool shadow = false) {
    SceneLight s;
    s.kind = 0;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = z;
    s.intensityCd = cd;
    s.rangeCm = 1000.0f;
    s.castShadows = shadow;
    return s;
}

SceneLight spot(f32 x, f32 y, f32 z, f32 halfDeg, bool shadow) {
    SceneLight s = lamp(x, y, z, 33333.0f, shadow);
    s.kind = 1;
    s.axis[0] = 0; s.axis[1] = 0; s.axis[2] = -1;
    s.outerCos = std::cos(halfDeg * 0.017453292f);
    s.innerCos = std::cos(halfDeg * 0.8f * 0.017453292f);
    return s;
}

struct Clip { f64 x, y, z, w; };
Clip project(const Vec3& p, const Mat4& m) {
    Clip c;
    c.x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    c.y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    c.z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    c.w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    return c;
}

bool readText(const std::string& rel, std::string& out) {
    return readFileText(std::string(AVER_REPO_ROOT) + "/" + rel, out);
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

int main() {
    AVER_INFO("=== RasterLightsTest ===");

    AVER_INFO("=== layout ===");
    {
        check(sizeof(rl::Block) == 16 + 32 * 7 * 16 + 24 * 64 + 24 * 16, "the tail is header + 32 lights of 7 float4 + 24 tile matrices + 24 uv rects");
        check(sizeof(rl::Block) % 16 == 0 && sizeof(rl::Block) < 8192, "a legal constant-buffer tail, small enough that engine block + tail stays under 8 KB");
        check(offsetof(rl::Block, light) == 16 && offsetof(rl::Block, tileVP) == 16 + 32 * 7 * 16, "the arrays follow each other without padding");
    }

    AVER_INFO("=== Godot's curves ===");
    {
        namespace gl = fmt::godotlight;
        // distanceFactor is Godot's get_omni_attenuation itself (distance in metres there, centimetres here), so the energy at 1 m means the same.
        for (const f64 decay : {0.0, 1.0, 2.0, 3.5}) {
            for (const f64 dM : {0.25, 0.5, 1.0, 2.0, 4.0, 4.9}) {
                const f64 rangeM = 5.0;
                const f64 godot = gl::omniAttenuation(dM, rangeM, decay);
                const f64 ours = rl::distanceFactor(static_cast<f32>(dM * 100), static_cast<f32>(rangeM * 100), 0.0f, true, static_cast<f32>(decay));
                check(near(ours, godot, 2e-3), "omni decay " + std::to_string(decay) + " at " + std::to_string(dM) + " m matches get_omni_attenuation");
            }
        }
        check(near(rl::distanceFactor(100.0f, 500.0f, 0.0f, true, 1.0f), 0.8, 1e-4), "Godot default (decay 1) at 1 m, range 5 m: the window alone, 1 - 1/5");
        check(near(rl::distanceFactor(200.0f, 500.0f, 0.0f, true, 2.0f), 0.6 / 4.0, 1e-4), "decay 2 at 2 m: window 0.6 over 4");
        check(near(rl::distanceFactor(250.0f, 500.0f, 0.0f, true, 0.0f), 0.5, 1e-4), "decay 0: brightness is only the window, 0.5 halfway");
        check(rl::distanceFactor(500.0f, 500.0f, 0.0f, true, 1.0f) == 0.0f && rl::distanceFactor(900.0f, 500.0f, 0.0f, false, 2.0f) == 0.0f,
              "nothing reaches the range, in either curve");
        // Aver's law stays the engine's: (1 m / d)^2 times (1 - (d/R)^4)^2, the same as aversLightEval.
        {
            const f32 d = 300.0f, r = 1000.0f;
            const f32 x2 = d * d / (r * r);
            f32 win = std::min(std::max(1.0f - x2 * x2, 0.0f), 1.0f);
            win *= win;
            check(near(rl::distanceFactor(d, r, 1.0f, false, 7.0f), win * 1e4f / (d * d), 1e-5), "the Aver curve is aversLightEval's, whatever the decay field says");
        }
        check(near(rl::distanceFactor(0.0f, 500.0f, 5.0f, false, 2.0f), rl::distanceFactor(5.0f, 500.0f, 5.0f, false, 2.0f), 1e-6),
              "inside the emitter radius the factor stops growing");

        // Spot cone.
        for (const f64 angAtt : {0.5, 1.0, 2.0}) {
            for (const f64 deg : {0.0, 10.0, 20.0, 29.0, 31.0}) {
                const f64 c = std::cos(deg * gl::kPi / 180.0);
                const f64 ref = deg < 30.0 ? gl::spotCone(c, 30.0, angAtt) : 0.0;
                const f32 ours = rl::coneFactor(static_cast<f32>(c), static_cast<f32>(std::cos(30.0 * gl::kPi / 180.0)), 0.9f, true, static_cast<f32>(angAtt));
                check(near(ours, ref, 2e-3, 2e-4), "spot cone at " + std::to_string(deg) + " deg, angle attenuation " + std::to_string(angAtt));
            }
        }
        check(near(rl::coneFactor(1.0f, 0.5f, 0.9f, true, 1.0f), 1.0, 1e-3), "on the axis the Godot cone is 1");
        check(rl::coneFactor(0.4f, 0.5f, 0.9f, false, 1.0f) == 0.0f && near(rl::coneFactor(0.95f, 0.5f, 0.9f, false, 1.0f), 1.0, 1e-5),
              "the Aver cone: 0 outside, 1 inside the inner angle");
    }

    AVER_INFO("=== calibration ===");
    {
        namespace gl = fmt::godotlight;
        check(near(gl::kEngineUnitCdm2, kEngineUnitCdm2, 1e-6), "the formats header and the renderer agree on the engine radiance unit");
        check(near(gl::kLuxPerEnergy, 3.14159265358979 * 100000.0 / 3.0, 1e-9) && near(gl::kLuxPerEnergy, 104719.755, 1e-6), "Godot energy 1 is pi engine units = 104719.76 lux");
        check(near(gl::sunLux(2.0), 2.0 * gl::kLuxPerEnergy, 1e-12) && near(gl::candela(0.5), 0.5 * gl::kCandelaPerEnergy, 1e-12), "linear in the energy");
        // Irradiance at 1 m of an omni of energy E, white: pi * E engine units, so albedo / pi * that = albedo * E, which is what Godot draws.
        const f32 e1m = pointIrradiance1m(static_cast<f32>(gl::candela(1.0)));
        check(near(e1m, 3.14159265, 1e-5), "an energy-1 omni delivers pi engine units at 1 m");
        check(near(0.8 / 3.14159265358979 * e1m, 0.8, 1e-5), "a white-ish albedo 0.8 under it returns 0.8 * energy, like Godot's pi-scaled lights");
        // And the sun: 104719.755 lux over the 100000/3 unit is also pi engine units.
        check(near(gl::sunLux(1.0) / (100000.0 / 3.0), 3.14159265358979, 1e-9), "Godot sun energy 1 is the same pi engine units");
        check(near(gl::specularScale(0.5), 1.0, 1e-12) && near(gl::specularScale(0.0), 0.0, 1e-12) && near(gl::specularScale(1.0), 2.0, 1e-12),
              "light_specular 0.5 (the default) is a factor 1; 0 removes the highlight");
    }

    AVER_INFO("=== distance fade ===");
    {
        check(rl::lightFade(5000.0f, 0.0f, 0.0f) == 1.0f, "no fade parameters: always 1");
        check(rl::lightFade(3999.0f, 4000.0f, 1000.0f) == 1.0f && near(rl::lightFade(4500.0f, 4000.0f, 1000.0f), 0.5, 1e-6) && rl::lightFade(5000.0f, 4000.0f, 1000.0f) == 0.0f,
              "1 until begin, linear to 0 at begin + length (Godot 40 m / 10 m)");
        check(rl::shadowFade(1000.0f, 4000.0f, 1000.0f, 5000.0f) == 1.0f && rl::shadowFade(5000.0f, 4000.0f, 1000.0f, 5000.0f) == 1.0f
                  && near(rl::shadowFade(5500.0f, 4000.0f, 1000.0f, 5000.0f), 0.5, 1e-6) && rl::shadowFade(6000.0f, 4000.0f, 1000.0f, 5000.0f) == 0.0f,
              "the shadow holds to distance_fade_shadow, then fades out over the length");
    }

    AVER_INFO("=== culling, ranking, the cap ===");
    {
        // A camera at the origin looking along +X, 90 degrees.
        const Mat4 view = Mat4::lookAtDirLH(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(1.5707963f, 1.0f, 10.0f, 5000.0f);
        const Mat4 vp = view * proj;
        const rl::Frustum fr = rl::frustumFromViewProj(&vp.m[0][0]);
        const f32 ahead[3] = {1000, 0, 0}, behind[3] = {-1000, 0, 0}, side[3] = {500, 3000, 0};
        check(fr.valid && rl::sphereInFrustum(fr, ahead, 10.0f), "a sphere ahead is inside");
        check(!rl::sphereInFrustum(fr, behind, 10.0f), "a sphere behind the camera is outside");
        check(!rl::sphereInFrustum(fr, side, 100.0f) && rl::sphereInFrustum(fr, side, 3000.0f), "a far-off-axis sphere is in only once its radius reaches the frustum");

        std::vector<SceneLight> ls;
        ls.push_back(lamp(1000, 0, 0));                 // ahead
        ls.push_back(lamp(-1000, 0, 0));                // behind, range 1000: reaches the near plane region? its sphere touches x = 0 only
        ls.push_back(lamp(-3000, 0, 0));                // far behind
        ls.back().rangeCm = 500.0f;
        SceneLight rect = lamp(1000, 100, 0);
        rect.kind = 3;                                  // rectangles are not this path's
        ls.push_back(rect);
        rl::Input in;
        in.lights = ls.data(); in.count = static_cast<u32>(ls.size());
        in.viewProj = &vp.m[0][0];
        rl::Result r;
        rl::build(in, r);
        check(r.stats.considered == 3 && r.stats.skippedKind == 1, "point and spot lights are offered; a rectangle is counted and skipped");
        check(r.stats.culled >= 1 && r.lightCount >= 1 && r.lightCount <= 2, "the far light behind the camera is culled by its range sphere");
        check(r.sourceIndex[0] == 0, "the light ahead is first");
        check(near(r.block.header[0], r.lightCount, 1e-9), "the header carries the count");

        // The cap and the order: 40 lamps in a row, each farther than the last, equal intensity.
        std::vector<SceneLight> many;
        for (int i = 0; i < 40; ++i) many.push_back(lamp(static_cast<f32>(100 + 60 * i), 0, 0));
        in.lights = many.data(); in.count = 40; in.viewProj = nullptr;
        rl::build(in, r);
        check(r.lightCount == rl::kMaxLights && r.stats.overCap == 8, "40 lights: 32 kept, 8 over the cap");
        bool sorted = true;
        for (u32 i = 1; i < r.lightCount; ++i) sorted &= r.sourceIndex[i - 1] < r.sourceIndex[i];
        check(sorted && r.sourceIndex[0] == 0 && r.sourceIndex[31] == 31, "the nearest 32 survive, nearest first");
        // A brighter far light outranks nearer dim ones.
        many[39].intensityCd = 3.0e8f;
        rl::build(in, r);
        check(r.sourceIndex[0] == 39, "importance is intensity over squared distance, not distance alone");
        // The fade removes a light.
        std::vector<SceneLight> faded{lamp(100, 0, 0), lamp(200, 0, 0)};
        faded[1].fadeBeginCm = 50.0f; faded[1].fadeLengthCm = 100.0f;   // gone at 150 cm; the light is 200 cm away
        in.lights = faded.data(); in.count = 2;
        rl::build(in, r);
        check(r.lightCount == 1 && r.stats.faded == 1, "a light past begin + length is faded out of the list");
        // A light half-faded carries half the energy.
        faded[1].fadeBeginCm = 100.0f; faded[1].fadeLengthCm = 200.0f;   // 200 cm away: halfway
        rl::build(in, r);
        check(r.lightCount == 2 && near(r.block.light[1][1][0] / r.block.light[0][1][0], 0.5, 1e-4), "half faded: half the irradiance at 1 m");
    }

    AVER_INFO("=== packing ===");
    {
        SceneLight s = spot(10, 20, 30, 30.0f, false);
        s.godot = true;
        s.attenuation = 1.0f; s.spotAttenuation = 2.0f; s.specularScale = 0.0f; s.negative = true;
        s.colour[0] = 1; s.colour[1] = 0.5f; s.colour[2] = 0.25f;
        s.intensityCd = static_cast<f32>(fmt::godotlight::candela(2.0));
        s.rangeCm = 800.0f;
        s.radiusCm = 5.0f;
        rl::Input in;
        in.lights = &s; in.count = 1;
        rl::Result r;
        rl::build(in, r);
        const f32 (*v)[4] = r.block.light[0];
        check(v[0][0] == 10 && v[0][1] == 20 && v[0][2] == 30 && v[0][3] == 5.0f, "position and emitter radius");
        check(near(v[1][0], 2.0 * 3.14159265, 1e-4) && near(v[1][1], v[1][0] * 0.5, 1e-5) && v[1][3] == 800.0f, "colour times the irradiance at 1 m (pi * energy), and the range");
        check(v[2][2] == -1.0f && v[2][3] == 1.0f, "axis and the spot kind");
        check(v[3][0] == 1.0f && near(v[3][1], std::cos(30.0 * 0.017453292), 1e-5) && near(v[3][2], 0.5, 1e-6), "Godot: decay as stored, cos outer, 1 / angle attenuation");
        const u32 flags = static_cast<u32>(v[3][3]);
        check((flags & rl::kFlagGodot) && (flags & rl::kFlagNegative) && !(flags & rl::kFlagShadowed), "flags: Godot, negative, not shadowed");
        check(v[4][0] == 0.0f, "specular scale 0 is a value");
        // The Aver mode keeps its own curve: decay 2 and the inner cone.
        SceneLight a = spot(0, 0, 0, 40.0f, false);
        in.lights = &a;
        rl::build(in, r);
        const f32 (*w)[4] = r.block.light[0];
        check(w[3][0] == 2.0f && near(w[3][2], a.innerCos, 1e-6) && w[4][0] == 1.0f && !(static_cast<u32>(w[3][3]) & rl::kFlagGodot), "an Aver light: decay 2, its inner cone, specular 1");
    }

    AVER_INFO("=== the shadow budget and the atlas ===");
    {
        rl::AtlasPacker p;
        u32 x, y;
        check(p.alloc(4, x, y) && x == 0 && y == 0 && p.alloc(4, x, y) && x == 4 && y == 0, "1024 px tiles go side by side, aligned to their size");
        check(p.alloc(2, x, y) && x == 8 && y == 0, "a 512 px tile takes the first free spot, on a multiple of 2 cells");
        u32 got = 0;
        rl::AtlasPacker q;
        while (q.alloc(4, x, y)) ++got;
        check(got == 16, "the 4096 px atlas holds sixteen 1024 px tiles");
        q.release(4, 4, 4);
        check(q.alloc(4, x, y) && x == 4 && y == 4, "a released tile is reused");

        // One shadowed spot: one tile, the big size.
        SceneLight s = spot(0, 0, 500, 30.0f, true);
        rl::Input in;
        in.lights = &s; in.count = 1;
        rl::Result r;
        rl::build(in, r);
        check(r.tiles.size() == 1 && r.tiles[0].cells == 4 && r.stats.shadowed == 1, "a shadowed spot takes one 1024 px tile");
        check(static_cast<u32>(r.block.light[0][3][3]) & rl::kFlagShadowed && !(static_cast<u32>(r.block.light[0][3][3]) & rl::kFlagCube), "flagged shadowed, not cube");
        check(r.block.tileUv[0][2] == 0.25f && r.block.header[1] == 1.0f && near(r.block.header[2], 1.0 / 4096.0, 1e-9), "its uv rect is a quarter of the atlas");
        // An omni takes six.
        SceneLight o = lamp(0, 0, 500, 33333.0f, true);
        in.lights = &o;
        rl::build(in, r);
        check(r.tiles.size() == 6 && r.tiles[0].cells == 2 && static_cast<u32>(r.block.light[0][3][3]) & rl::kFlagCube, "a shadowed omni takes six 512 px faces and is flagged cube");
        // A wide spot is shadowed as a cube.
        SceneLight wide = spot(0, 0, 500, 80.0f, true);
        in.lights = &wide;
        rl::build(in, r);
        check(r.tiles.size() == 6, "a spot wider than 75 degrees is shadowed with a cube");
        // No shadow switch, no tiles.
        SceneLight off = spot(0, 0, 500, 30.0f, false);
        in.lights = &off;
        rl::build(in, r);
        check(r.tiles.empty(), "castShadows off: no tile");
        in.lights = &s; in.shadows = false;
        rl::build(in, r);
        check(r.tiles.empty() && r.lightCount == 1, "shadows unavailable: the light is still listed, with no tile");
        in.shadows = true;

        // Budget: 12 shadowed omnis want 72 tiles; at most 8 shadow lights and 24 tiles.
        std::vector<SceneLight> os;
        for (int i = 0; i < 12; ++i) os.push_back(lamp(static_cast<f32>(100 * i), 0, 100, 33333.0f, true));
        in.lights = os.data(); in.count = 12;
        rl::build(in, r);
        check(r.tiles.size() == 24 && r.stats.shadowed == 4 && r.stats.shadowDenied >= 1 && r.lightCount == 12, "24 tiles: four omnis shadowed, the rest lit without shadow");
        // No tile overlaps another and all lie in the atlas.
        bool ok = true;
        for (usize i = 0; i < r.tiles.size(); ++i) {
            const rl::Tile& a = r.tiles[i];
            ok &= a.cx + a.cells <= 16 && a.cy + a.cells <= 16;
            for (usize j = i + 1; j < r.tiles.size(); ++j) {
                const rl::Tile& b = r.tiles[j];
                const bool apart = a.cx + a.cells <= b.cx || b.cx + b.cells <= a.cx || a.cy + a.cells <= b.cy || b.cy + b.cells <= a.cy;
                ok &= apart;
            }
        }
        check(ok, "no two tiles overlap and every tile is inside the atlas");
        // Eight spots: the nearest two get the big tiles, the rest the smaller ones.
        std::vector<SceneLight> ss;
        for (int i = 0; i < 10; ++i) ss.push_back(spot(static_cast<f32>(100 * i), 0, 500, 30.0f, true));
        in.lights = ss.data(); in.count = 10;
        rl::build(in, r);
        check(r.stats.shadowed == 8 && r.tiles.size() == 8 && r.tiles[0].cells == 4 && r.tiles[1].cells == 4 && r.tiles[2].cells == 2, "eight spots shadowed; resolution falls with rank");
        check(near(r.block.light[0][5][0], 0.0, 1e-9) && near(r.block.light[1][5][0], 1.0, 1e-9) && r.block.light[0][5][1] == 1.0f, "each light's first tile and tile count");
        check(near(r.block.light[0][5][2], 1.0, 1e-5) && near(r.block.light[0][5][3], 2.0, 1e-5), "Godot's default shadow_bias 0.1 is 1 cm, shadow_normal_bias 2 texels");
        // A stored bias passes through.
        ss[0].shadowBias = 0.5f; ss[0].shadowNormalBias = 3.0f;
        rl::build(in, r);
        check(near(r.block.light[0][5][2], 5.0, 1e-5) && near(r.block.light[0][5][3], 3.0, 1e-5), "stored biases are used");
    }

    AVER_INFO("=== tile matrices ===");
    {
        // A spot at (0,0,500) shining down: the point on its axis lands in the middle of its tile.
        const Vec3 pos{0, 0, 500}, axis{0, 0, -1};
        const f32 half = 30.0f * 0.017453292f, n = rl::kShadowNearCm, f = 800.0f;
        const Mat4 m = rl::spotViewProj(pos, axis, half, n, f);
        const Clip c = project(Vec3{0, 0, 100}, m);   // 400 cm below
        check(near(c.x / c.w, 0.0, 1e-5, 1e-6) && near(c.y / c.w, 0.0, 1e-5, 1e-6), "a point on the axis projects to the tile centre");
        check(near(c.z / c.w, (f / (f - n)) * (1.0 - n / 400.0), 1e-4), "its depth is the standard perspective depth the shader compares with");
        // A point at the cone's edge (30 degrees) lands inside the tile (the matrix is a few percent wider).
        const f64 edge = std::tan(30.0 * 0.017453292) * 400.0;
        const Clip e = project(Vec3{static_cast<f32>(edge), 0, 100}, m);
        const f64 ex = std::max(std::fabs(e.x / e.w), std::fabs(e.y / e.w));
        check(ex < 1.0 && ex > 0.95, "the cone's edge sits just inside the tile border (the matrix is 4 percent wider)");

        // Cube faces: a point along each axis projects to the middle of its own face, and the face pick agrees.
        const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        bool all = true;
        for (u32 face = 0; face < 6; ++face) {
            const Mat4 fm = rl::faceViewProj(pos, face, n, f);
            const Vec3 p = pos + dirs[face] * 300.0f;
            const Clip fc = project(p, fm);
            all &= near(fc.x / fc.w, 0.0, 1e-5, 1e-5) && near(fc.y / fc.w, 0.0, 1e-5, 1e-5) && near(fc.z / fc.w, (f / (f - n)) * (1.0 - n / 300.0), 1e-4);
            all &= rl::cubeFaceOf(dirs[face] * 300.0f) == face;
        }
        check(all, "each cube face looks along its axis; a point on that axis is its centre, at the perspective depth");
        // A 45 degree point is on the border of its face and inside it.
        const Clip b = project(pos + Vec3{200, 190, 0}, rl::faceViewProj(pos, 0, n, f));
        check(near(std::fabs(b.x / b.w), 0.95, 1e-4) && near(b.y / b.w, 0.0, 1e-5, 1e-5), "a point at +X, offset 190 of 200 sideways: inside the +X face at 0.95");
        check(rl::cubeFaceOf(Vec3{200, 190, 0}) == 0 && rl::cubeFaceOf(Vec3{190, 200, 0}) == 2 && rl::cubeFaceOf(Vec3{0, 0, -5}) == 5, "the major axis picks the face");
        // The up convention of the sun's map: +Z world is +y in the tile for a horizontal face (uv.y smaller).
        const Clip up = project(pos + Vec3{300, 0, 50}, rl::faceViewProj(pos, 0, n, f));
        check(up.y / up.w > 0.0, "world up is +y in clip space on the side faces");
    }

    AVER_INFO("=== the shader still says what the header packs for ===");
    {
        std::string voxi, rlh;
        check(readText("modules/render.voxi/shaders/voxi.hlsl", voxi), "voxi.hlsl is readable");
        check(readText("modules/render.voxi/shaders/voxi_locallights_raster.hlsli", rlh), "voxi_locallights_raster.hlsli is readable");
        check(has(voxi, "#define AVER_RL_MAX_LIGHTS 32") && has(voxi, "#define AVER_RL_LIGHT_VECS 7") && has(voxi, "#define AVER_RL_MAX_TILES 24"),
              "the HLSL constants equal rl::kMaxLights / kLightVecs / kMaxTiles");
        check(has(voxi, "float4   gRlHeader;") && has(voxi, "float4   gRlLight[AVER_RL_MAX_LIGHTS * AVER_RL_LIGHT_VECS];") &&
                  has(voxi, "float4x4 gRlTileVP[AVER_RL_MAX_TILES];") && has(voxi, "float4   gRlTileUv[AVER_RL_MAX_TILES];"),
              "the cbuffer tail has the header, the lights, the tile matrices and the uv rects in the Block's order");
        const usize tail = voxi.find("float4   gRlHeader;");
        const usize decal = voxi.find("float4   gDecalParams;");
        check(tail != std::string::npos && decal != std::string::npos && decal < tail && voxi.find("#if AVER_LOCAL_RASTER") < tail,
              "the tail comes after the engine block's last field and only under AVER_LOCAL_RASTER");
        check(has(voxi, "#ifndef AVER_LOCAL_RASTER\n#define AVER_LOCAL_RASTER 0") || has(voxi, "#ifndef AVER_LOCAL_RASTER\r\n#define AVER_LOCAL_RASTER 0"),
              "AVER_LOCAL_RASTER defaults to 0: the ordinary variants do not see the tail");
        check(has(voxi, "if (gRlHeader.x > 0.5) radiance += rlShade(s, i.wpos, N);"), "PSMainVoxi calls the raster lights");
        check(has(rlh, "#define AVER_RL_F_GODOT    1u") && has(rlh, "#define AVER_RL_F_NEGATIVE 2u") && has(rlh, "#define AVER_RL_F_SHADOWED 4u") && has(rlh, "#define AVER_RL_F_CUBE     8u"),
              "the flag bits equal kFlagGodot / Negative / Shadowed / Cube");
        check(has(rlh, "if (a.x >= a.y && a.x >= a.z) return d.x >= 0.0 ? 0u : 1u;") && has(rlh, "if (a.y >= a.z) return d.y >= 0.0 ? 2u : 3u;") && has(rlh, "return d.z >= 0.0 ? 4u : 5u;"),
              "the shader's cube-face pick is rl::cubeFaceOf");
        check(has(rlh, "max(1.0 - x, 0.0) * pow(100.0 / max(d, max(radius, 0.01)), decay)") && has(rlh, "1.0 - pow(rim, cosInnerOrInvAtt)"),
              "the shader's Godot curves are the ones tested above");
        // Defaults unchanged: nothing outside the guards.
        usize outside = 0, pos = 0;
        while ((pos = voxi.find("rlShade", pos)) != std::string::npos) { ++outside; pos += 7; }
        check(outside == 1, "rlShade is called in exactly one place");
        check(has(voxi, "#if AVER_LOCAL_RASTER\n#include \"voxi_locallights_raster.hlsli\"") || has(voxi, "#if AVER_LOCAL_RASTER\r\n#include \"voxi_locallights_raster.hlsli\""),
              "the include is guarded");
    }

    AVER_INFO("RasterLightsTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
