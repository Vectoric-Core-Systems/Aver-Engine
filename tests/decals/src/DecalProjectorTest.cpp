// DecalProjectorTest -- the CPU-checkable half of projected decals (aver/voxi/SceneDecal.hpp): the
// projector maths the shader mirrors (box test, soft edge, angle fade, uv orientation, the normal
// frame), eye-relative packing far from the origin, paint order, the mip chain builder and the record
// layout. The HLSL (voxi_decal.hlsli) is the code that runs; its source is read at the end to check it
// still names the layout and constants the header packs for. No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/voxi/SceneDecal.hpp"

#include <cmath>
#include <cstdlib>
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

static bool approx(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol; }
static bool nearV(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) {
    return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) && approx(a.z, b.z, tol);
}

// A decal at `pos`, rotated about +Z by yawDeg, scaled by `scale`, half extents `half` (local cm).
static SceneDecal makeDecal(const Vec3& pos, f32 yawDeg, const Vec3& scale, const Vec3& half) {
    SceneDecal d;
    const Mat4 m = Mat4::scale(scale) * Mat4::fromQuat(Quat::fromAxisAngle({0, 0, 1}, radians(yawDeg))) * Mat4::translation(pos);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) d.world[r * 4 + c] = m.m[r][c];
    d.halfExtentsCm[0] = half.x; d.halfExtentsCm[1] = half.y; d.halfExtentsCm[2] = half.z;
    return d;
}

static PackedDecal pack(const SceneDecal& d, const Vec3& eye, const DecalTextureIndices& tx = {}) {
    PackedDecal p{};
    const f32 e[3] = {eye.x, eye.y, eye.z};
    const bool ok = packSceneDecal(d, e, tx, p);
    check(ok, "the decal packs");
    return p;
}

int main() {
    AVER_INFO("=== DecalProjectorTest ===");

    AVER_INFO("=== the box and its coordinates ===");
    {
        const Vec3 pos{100, 200, 50};
        const SceneDecal d = makeDecal(pos, 0.0f, {1, 1, 1}, {20, 40, 30});
        const Vec3 eye{0, 0, 0};
        const PackedDecal p = pack(d, eye);
        const Vec3 up{0, 0, 1};
        // The surface faces back at the projector, which looks along +X: its normal is -X.
        const Vec3 facing{-1, 0, 0};

        DecalProjection c = decalProject(p, pos - eye, facing);
        check(c.inside && nearV(c.local, {0, 0, 0}), "the centre is inside, at local (0,0,0)");
        check(approx(c.u, 0.5f) && approx(c.v, 0.5f), "the centre is the middle of the image");
        check(approx(c.weight, 1.0f), "full weight at the centre of a surface facing the projector");

        DecalProjection right = decalProject(p, pos + Vec3{0, 20, 0} - eye, facing);
        check(right.inside && right.u > 0.5f && approx(right.v, 0.5f), "+Y (right) increases u");
        DecalProjection above = decalProject(p, pos + Vec3{0, 0, 15} - eye, facing);
        check(above.inside && above.v < 0.5f && approx(above.u, 0.5f), "+Z (up) decreases v: image up is box up");
        check(approx(right.u, 0.75f) && approx(above.v, 0.25f), "half-way to a face is a quarter of the image");

        check(!decalProject(p, pos + Vec3{25, 0, 0} - eye, facing).inside, "beyond the depth is outside");
        check(!decalProject(p, pos + Vec3{0, 41, 0} - eye, facing).inside, "beyond the width is outside");
        check(!decalProject(p, pos + Vec3{0, 0, -31} - eye, facing).inside, "beyond the height is outside");
        check(decalProject(p, pos + Vec3{19, 39, 29} - eye, facing).inside, "just inside every face is inside");
        (void)up;

        // Soft edge: 10% of the half extent. 5% from the face is half way up the ramp.
        const DecalProjection nearEdge = decalProject(p, pos + Vec3{0, 38.0f, 0} - eye, facing);   // 5% from +Y face
        check(approx(nearEdge.weight, 0.5f, 0.02f), "5% from a face the soft edge is at half weight");
        const DecalProjection onEdge = decalProject(p, pos + Vec3{0, 39.99f, 0} - eye, facing);
        check(onEdge.weight < 0.02f, "at the face the weight reaches zero");
    }

    AVER_INFO("=== the angle fade ===");
    {
        const Vec3 pos{0, 0, 0};
        SceneDecal d = makeDecal(pos, 0.0f, {1, 1, 1}, {50, 50, 50});
        d.angleFadeStartDeg = 60.0f;
        d.angleFadeEndDeg = 85.0f;
        const PackedDecal p = pack(d, {0, 0, 300});
        const Vec3 prel = pos - Vec3{0, 0, 300};
        auto weightAt = [&](f32 deg) {
            // A surface whose normal is `deg` away from the way back at the projector (-X).
            const f32 a = radians(deg);
            return decalProject(p, prel, Vec3{-std::cos(a), 0.0f, std::sin(a)}).weight;
        };
        check(approx(weightAt(0.0f), 1.0f), "a surface square to the projector gets full weight");
        check(approx(weightAt(59.0f), 1.0f), "inside the start angle is still full weight");
        check(approx(weightAt(86.0f), 0.0f), "past the end angle gets nothing");
        check(approx(weightAt(90.0f), 0.0f), "a surface running along the projection gets nothing");
        const f32 mid = weightAt(72.5f);
        check(mid > 0.05f && mid < 0.95f, "between the two it is partial");
        check(weightAt(65.0f) > weightAt(75.0f) && weightAt(75.0f) > weightAt(82.0f), "and falls steadily with the angle");
        check(approx(decalProject(p, prel, Vec3{1, 0, 0}).weight, 0.0f), "a surface facing away from the projector gets nothing");
    }

    AVER_INFO("=== rotation and scale ===");
    {
        // Yawed 90 degrees about Z: the projector's +X is world +Y, its +Y (right) is world -X.
        const Vec3 pos{500, 0, 0};
        const SceneDecal d = makeDecal(pos, 90.0f, {1, 1, 1}, {10, 20, 30});
        const PackedDecal p = pack(d, {0, 0, 0});
        const Vec3 ax{p.ax[0], p.ax[1], p.ax[2]}, ay{p.ay[0], p.ay[1], p.ay[2]}, az{p.az[0], p.az[1], p.az[2]};
        check(nearV(ax, {0, 1, 0}) && nearV(ay, {-1, 0, 0}) && nearV(az, {0, 0, 1}), "the packed axes are the rotated unit axes");
        const DecalProjection c = decalProject(p, pos + Vec3{0, 5, 0}, Vec3{0, -1, 0});
        check(c.inside && approx(c.local.x, 5.0f), "5 cm along world +Y is 5 cm along the projection");
        check(c.weight > 0.99f, "and that surface faces the projector");

        // Scale 2 on Y: the box is twice as wide in the world; local units stay unscaled.
        const SceneDecal s = makeDecal({0, 0, 0}, 0.0f, {1, 2, 1}, {10, 20, 30});
        const PackedDecal ps = pack(s, {0, 0, 0});
        check(decalProject(ps, Vec3{0, 39, 0}, Vec3{-1, 0, 0}).inside, "scale 2 puts the +Y face at 40 world cm");
        check(!decalProject(ps, Vec3{0, 41, 0}, Vec3{-1, 0, 0}).inside, "and nothing past it");
        check(approx(decalProject(ps, Vec3{0, 20, 0}, Vec3{-1, 0, 0}).local.y, 10.0f), "the local coordinate is in unscaled units");
    }

    AVER_INFO("=== the eye-relative packing keeps its precision far from the origin ===");
    {
        const Vec3 pos{2.0e6f, -1.5e6f, 400.0f};
        const SceneDecal d = makeDecal(pos, 30.0f, {1, 1, 1}, {25, 50, 50});
        const Vec3 eye = pos + Vec3{-300, 120, 0};
        const PackedDecal p = pack(d, eye);
        // A point 7 cm along the projector's own +Y axis from the centre. The offset from the eye is
        // built from the small difference, as the shader's (wpos - eye) is, not from a quantised world point.
        const Vec3 axisY{std::cos(radians(120.0f)), std::sin(radians(120.0f)), 0.0f};
        const Vec3 prel = (pos - eye) + axisY * 7.0f;
        const DecalProjection c = decalProject(p, prel, Vec3{-std::cos(radians(30.0f)), -std::sin(radians(30.0f)), 0.0f});
        check(c.inside && approx(c.local.y, 7.0f, 0.05f) && approx(c.local.x, 0.0f, 0.05f),
              "7 cm off-centre 2 km from the origin reads 7 cm, not a float-quantised neighbour");
    }

    AVER_INFO("=== the normal frame ===");
    {
        const SceneDecal d = makeDecal({0, 0, 0}, 0.0f, {1, 1, 1}, {50, 50, 50});
        const PackedDecal p = pack(d, {0, 0, 0});
        const Vec3 n{-1, 0, 0};   // a wall facing the projector
        check(nearV(decalWorldNormal(p, {0, 0, 1}, n), n), "a flat tangent-space normal is the surface normal");
        const Vec3 right = decalWorldNormal(p, {1, 0, 0}, n);
        check(right.y > 0.99f, "+X in the map tilts the normal toward the image's u direction (world +Y)");
        const Vec3 upN = decalWorldNormal(p, {0, 1, 0}, n);
        check(upN.z > 0.99f, "+Y in the map tilts the normal toward the image's up (world +Z)");
        // On a floor (normal +Z) facing the projector from above, the tangent frame follows the surface.
        const SceneDecal down = makeDecal({0, 0, 0}, 0.0f, {1, 1, 1}, {50, 50, 50});
        const PackedDecal pd = pack(down, {0, 0, 0});
        const Vec3 floorN{0, 0, 1};
        const Vec3 tilted = decalWorldNormal(pd, {0.5f, 0, 0.8660254f}, floorN);
        check(approx(tilted.size(), 1.0f) && dot(tilted, floorN) > 0.8f, "on a tilted surface the result stays unit and on its side");
    }

    AVER_INFO("=== packing refusals and flags ===");
    {
        SceneDecal d = makeDecal({0, 0, 0}, 0.0f, {1, 1, 1}, {10, 10, 10});
        PackedDecal p{};
        const f32 eye[3] = {0, 0, 0};
        DecalTextureIndices none;
        check(packSceneDecal(d, eye, none, p), "an untextured colour decal packs");
        check((decalFloatToBits(p.ext[3]) & kDecalPackColour) && !(decalFloatToBits(p.ext[3]) & kDecalPackTextured),
              "it paints colour and is not marked textured");
        check(!(decalFloatToBits(p.ext[3]) & kDecalPackRough), "no roughness image and no roughness value: roughness is left alone");

        SceneDecal r = d; r.roughness = 0.4f; r.roughnessMetal = true;
        check(packSceneDecal(r, eye, none, p) && (decalFloatToBits(p.ext[3]) & kDecalPackRough), "an explicit roughness paints it");

        SceneDecal t = d; t.baseId = 7; t.normalId = 8; t.ormId = 9; t.roughnessMetal = true;
        DecalTextureIndices tx; tx.base = 3; tx.normal = 4; tx.orm = 5;
        check(packSceneDecal(t, eye, tx, p), "a textured decal packs");
        const u32 fl = decalFloatToBits(p.ext[3]);
        check((fl & kDecalPackHasBase) && (fl & kDecalPackHasNormal) && (fl & kDecalPackHasOrm) && (fl & kDecalPackTextured) &&
                  (fl & kDecalPackNormal) && (fl & kDecalPackRough),
              "base, normal and ORM images set their flags and channels");
        check(p.tex[0] == 3 && p.tex[1] == 4 && p.tex[2] == 5, "and carry their bindless indices");

        SceneDecal onlyNormal = d; onlyNormal.colour = false; onlyNormal.roughnessMetal = false;
        check(!packSceneDecal(onlyNormal, eye, none, p), "a decal with no channel left does not pack (normal needs an image)");
        onlyNormal.normalId = 8;
        check(packSceneDecal(onlyNormal, eye, tx, p), "a normal-only decal with its image does");

        SceneDecal flat = d; flat.halfExtentsCm[1] = 0.0f;
        check(!packSceneDecal(flat, eye, none, p), "a zero-thickness box does not pack");
        SceneDecal collapsed = d; collapsed.world[0] = collapsed.world[1] = collapsed.world[2] = 0.0f;
        check(!packSceneDecal(collapsed, eye, none, p), "a matrix with a collapsed axis does not pack");
    }

    AVER_INFO("=== culling helpers and paint order ===");
    {
        check(approx(decalDistanceFade(100.0f, 0.0f), 1.0f), "no fade distance never fades");
        check(approx(decalDistanceFade(500.0f, 1000.0f), 1.0f) && approx(decalDistanceFade(600.0f, 1000.0f), 1.0f), "full strength to 60% of the distance");
        check(approx(decalDistanceFade(800.0f, 1000.0f), 0.5f) && approx(decalDistanceFade(1000.0f, 1000.0f), 0.0f) &&
                  approx(decalDistanceFade(5000.0f, 1000.0f), 0.0f), "then a linear ramp to nothing");
        check(decalPaintsBefore(0, 10.0f, 1, 10.0f) && !decalPaintsBefore(1, 10.0f, 0, 10.0f), "lower sort order paints first");
        check(decalPaintsBefore(2, 500.0f, 2, 100.0f) && !decalPaintsBefore(2, 100.0f, 2, 500.0f), "at equal order the farther paints first, so the nearer lands on top");

        const SceneDecal d = makeDecal({10, 20, 30}, 0.0f, {2, 1, 1}, {5, 5, 5});
        f32 c[3], r = 0.0f;
        decalBounds(d, c, r);
        check(approx(c[0], 10.0f) && approx(c[1], 20.0f) && approx(c[2], 30.0f), "the bounds centre is the matrix translation");
        check(approx(r, std::sqrt(10.0f * 10.0f + 5.0f * 5.0f + 5.0f * 5.0f)), "the radius covers the scaled half extents");
    }

    AVER_INFO("=== the mip chain ===");
    {
        check(decalMipCount(1, 1) == 1 && decalMipCount(256, 64) == 9 && decalMipCount(5, 3) == 3, "mip counts");
        std::vector<u8> flat(8 * 4 * 4);
        for (usize i = 0; i < flat.size(); i += 4) { flat[i] = 200; flat[i + 1] = 100; flat[i + 2] = 50; flat[i + 3] = 255; }
        const auto chain = buildDecalMips(flat.data(), 8, 4, DecalImageKind::Colour);
        check(chain.size() == 4 && chain[0].size() == 8 * 4 * 4 && chain[1].size() == 4 * 2 * 4 &&
                  chain[2].size() == 2 * 1 * 4 && chain[3].size() == 1 * 1 * 4, "8x4 -> 4x2 -> 2x1 -> 1x1");
        check(std::abs(int(chain[3][0]) - 200) <= 1 && std::abs(int(chain[3][1]) - 100) <= 1 && std::abs(int(chain[3][2]) - 50) <= 1 &&
                  chain[3][3] == 255, "a flat colour survives every level");

        // Left half opaque red, right half fully transparent black: the colour must not darken.
        std::vector<u8> cut(4 * 4 * 4, 0);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 2; ++x) { u8* px = &cut[(y * 4 + x) * 4]; px[0] = 255; px[1] = 0; px[2] = 0; px[3] = 255; }
        const auto cc = buildDecalMips(cut.data(), 4, 4, DecalImageKind::Colour);
        check(cc[1][0] == 255 && cc[1][1] == 0 && cc[1][3] == 255, "a texel fully inside the cutout keeps its colour");
        check(cc[2][0] >= 250 && cc[2][3] >= 120 && cc[2][3] <= 135, "alpha-weighting keeps the colour while coverage halves");

        // Normals: two opposite tilts average to a unit-length up, not a short vector.
        std::vector<u8> nrm(2 * 1 * 4);
        nrm[0] = 204; nrm[1] = 128; nrm[2] = 230; nrm[3] = 255;   // (+0.6, 0, 0.8)
        nrm[4] = 51;  nrm[5] = 128; nrm[6] = 230; nrm[7] = 255;   // (-0.6, 0, 0.8)
        const auto nc = buildDecalMips(nrm.data(), 2, 1, DecalImageKind::Normal);
        check(nc.size() == 2 && std::abs(int(nc[1][0]) - 128) <= 2 && nc[1][2] >= 250, "opposing normals average to straight up at unit length");

        check(buildDecalMips(nullptr, 4, 4, DecalImageKind::Data).empty(), "a null image has no chain");
    }

    AVER_INFO("=== the shader still agrees with the header ===");
    {
        std::string hlsl, voxi, rt, gi, cpp;
        const std::string root = AVER_REPO_ROOT;
        check(readFileText(root + "/modules/render.voxi/shaders/voxi_decal.hlsli", hlsl), "voxi_decal.hlsli is readable");
        check(readFileText(root + "/modules/render.voxi/shaders/voxi.hlsl", voxi), "voxi.hlsl is readable");
        check(readFileText(root + "/modules/render.voxi/shaders/voxi_rt.hlsli", rt), "voxi_rt.hlsli is readable");
        check(readFileText(root + "/modules/render.voxi/shaders/voxi_gi.hlsli", gi), "voxi_gi.hlsli is readable");
        check(readFileText(root + "/modules/render.voxi/src/VoxiRenderer.cpp", cpp), "VoxiRenderer.cpp is readable");
        const auto in = [](const std::string& hay, const char* s) { return hay.find(s) != std::string::npos; };

        check(in(hlsl, "struct AverDecalRec") && in(hlsl, "float4 r0, r1, r2;") && in(hlsl, "float4 ext;") &&
                  in(hlsl, "float4 tint;") && in(hlsl, "float4 shade;") && in(hlsl, "float4 ang;") && in(hlsl, "float4 uvo;") &&
                  in(hlsl, "uint4  tex;") && in(hlsl, "float4 ax, ay, az;"),
              "the 192-byte record has the fields PackedDecal packs, in order");
        const size_t at = hlsl.find("float4 r0, r1, r2;");
        check(at != std::string::npos && at < hlsl.find("float4 ext;") && hlsl.find("float4 ext;") < hlsl.find("float4 tint;") &&
                  hlsl.find("float4 tint;") < hlsl.find("float4 shade;") && hlsl.find("float4 shade;") < hlsl.find("float4 ang;") &&
                  hlsl.find("float4 ang;") < hlsl.find("float4 uvo;") && hlsl.find("float4 uvo;") < hlsl.find("uint4  tex;") &&
                  hlsl.find("uint4  tex;") < hlsl.find("float4 ax, ay, az;"), "in the order of the C++ struct");
        check(in(hlsl, "gDecals : register(t24)"), "the list is at t24");
        check(in(cpp, "srv[24] = rhi::SlotKind::StructuredBuffer") && in(cpp, "kGiSrvCount + 16") && in(cpp, "kVoxiSrvCount == 25") &&
                  in(cpp, "setSrvBuffer(bindings_, 24"), "the renderer declares, sizes and binds slot 24");
        check(in(hlsl, "#define AVER_DECAL_COLOUR     0x01u") && in(hlsl, "#define AVER_DECAL_NORMAL     0x02u") &&
                  in(hlsl, "#define AVER_DECAL_ROUGH      0x04u") && in(hlsl, "#define AVER_DECAL_HAS_BASE   0x08u") &&
                  in(hlsl, "#define AVER_DECAL_HAS_NORMAL 0x10u") && in(hlsl, "#define AVER_DECAL_HAS_ORM    0x20u") &&
                  in(hlsl, "#define AVER_DECAL_TEXTURED   0x40u"), "the flag bits are the header's kDecalPack* values");
        check(kDecalPackColour == 0x01 && kDecalPackNormal == 0x02 && kDecalPackRough == 0x04 && kDecalPackHasBase == 0x08 &&
                  kDecalPackHasNormal == 0x10 && kDecalPackHasOrm == 0x20 && kDecalPackTextured == 0x40, "(and the header says so)");
        check(in(voxi, "float4   gDecalParams;") && in(gi, "float4   gDecalParams;"), "both cbuffer mirrors end with gDecalParams");
        check(in(voxi, "if (gDecalParams.x > 0.5 && !averDrawIsTranslucent()) averApplyDecals(s, i.wpos, N, true);"),
              "the raster path applies decals after the material, before lighting");
        check(in(rt, "averApplyDecals(s, h.pos, h.N, detail == AVER_RT_HIT_FULL)") && in(rt, "#include \"voxi_decal.hlsli\""),
              "every ray hit applies them in rtHitSurface");
        check(in(hlsl, "if (flags & AVER_DECAL_TEXTURED) continue;"), "a textured decal draws nothing, not a flat box, without the bindless table");
        check(kMaxSceneDecals == 128, "the per-frame cap is what the docs say");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
