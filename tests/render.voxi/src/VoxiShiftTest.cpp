// VoxiShiftTest -- the renderer's half of the floating origin that needs no device (docs/plans/wave11/LARGE_WORLD.md 2.2.6,
// section 5.4). Exit code = failure count. Links Aver.Core and reaches the headers by include path, like NeuRaCTest.
//
// WHAT IS DECIDED HERE, AND WHAT IS NOT. A rebase moves every scene-frame position by -delta, so everything the renderer
// keeps from the old frame has to move with it or be re-labelled: the three previous-camera matrices, the camera positions
// NRD2 differences, the GI volume, the draw lists, the RT instance rows, the GI draw-list key, the NeuRaC cache. The pieces
// that are ARITHMETIC are held to an independent f64 here (the matrix chain, the NRD2 camera delta, NeuRaC's tag byte for
// the three cell sizes). The pieces that are WIRING (does VoxiRenderer::shiftWorld touch every member that holds a position,
// in an order that keeps the volume from resetting; do both backends fold the cloud offset) are held by reading the sources
// the renderer is built from, the way the repo's other source assertions do: a member added later that holds a position and
// is not in the shift fails here by name. What no unit test can decide -- the rebase-frame image, the GI rebuild counters,
// the radiance-cache reset counter, the TLAS rebuild, TAA ghosting -- is the owner's headed pass (section 6).
#include "aver/core/Log.hpp"
#include "aver/rhi/WorldShift.hpp"
#include "aver/voxi/NeuRaCLayout.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace nc = aver::voxi::neurac;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

constexpr f64 kStep = 1'638'400.0;   // one region

struct M64 { f64 m[4][4]; };

M64 mul(const M64& a, const M64& b) {
    M64 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            f64 s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// A left-handed row-vector view-projection (translation in row 3, depth 0..1), built in f64 and rounded to the f32 matrix
// the renderer stores. z is up, the camera looks along `yaw` / `pitch`.
void viewProjF32(const f64 eye[3], f64 yaw, f64 pitch, f32 out[16]) {
    const f64 fwd[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const f64 up[3] = {0, 0, 1};
    const auto cross = [](const f64 a[3], const f64 b[3], f64 o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
    };
    const auto norm = [](f64 v[3]) { const f64 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= l; v[1] /= l; v[2] /= l; };
    const auto dot = [](const f64 a[3], const f64 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    f64 f[3] = {fwd[0], fwd[1], fwd[2]}, s[3], u[3];
    norm(f);
    cross(up, f, s); norm(s);
    cross(f, s, u);
    M64 v{};
    for (int i = 0; i < 3; ++i) { v.m[i][0] = s[i]; v.m[i][1] = u[i]; v.m[i][2] = f[i]; }
    v.m[3][0] = -dot(s, eye); v.m[3][1] = -dot(u, eye); v.m[3][2] = -dot(f, eye); v.m[3][3] = 1.0;
    const f64 fovY = 1.0, aspect = 16.0 / 9.0, zn = 10.0, zf = 1.0e6;
    const f64 ys = 1.0 / std::tan(fovY * 0.5), xs = ys / aspect;
    M64 p{};
    p.m[0][0] = xs; p.m[1][1] = ys;
    p.m[2][2] = zf / (zf - zn); p.m[2][3] = 1.0;
    p.m[3][2] = -zn * zf / (zf - zn);
    const M64 vp = mul(v, p);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<f32>(vp.m[r][c]);
}

// clip = (x y z 1) * M, returned as NDC (x / w, y / w). False behind the camera.
bool project(const f32 m[16], const f64 p[3], f64 ndc[2]) {
    f64 c[4];
    for (int j = 0; j < 4; ++j)
        c[j] = static_cast<f64>(m[12 + j]) + p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j];
    if (!(c[3] > 1e-9)) return false;
    ndc[0] = c[0] / c[3];
    ndc[1] = c[1] / c[3];
    return true;
}

// Nrd2.cpp's prevViewProjRel, as written there: the previous matrix about the previous eye (folded in f64).
void prevViewProjRel(const f32 m[16], const f32 eye[3], f32 out[16]) {
    std::memcpy(out, m, 12 * sizeof(f32));
    for (u32 j = 0; j < 4; ++j) {
        f64 t = m[12 + j];
        for (u32 k = 0; k < 3; ++k) t += static_cast<f64>(eye[k]) * static_cast<f64>(m[k * 4 + j]);
        out[12 + j] = static_cast<f32>(t);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The text of the function whose signature starts with `sig`: from its opening brace to the matching close.
std::string bodyOf(const std::string& src, const char* sig) {
    const usize at = src.find(sig);
    if (at == std::string::npos) return {};
    const usize open = src.find('{', at);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (usize i = open; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}' && --depth == 0) return src.substr(open, i - open + 1);
    }
    return {};
}

bool hasAll(const std::string& text, const std::vector<const char*>& needles, std::string& missing) {
    for (const char* n : needles)
        if (text.find(n) == std::string::npos) { missing = n; return false; }
    return true;
}

} // namespace

int main() {
    AVER_INFO("VoxiShiftTest");
    std::mt19937 rng(20261026u);
    const auto frand = [&](f64 lo, f64 hi) { return lo + (hi - lo) * (static_cast<f64>(rng() >> 8) / 16777216.0); };

    // ---- (1) the previous-camera chain: all three matrices move with the scene, and the NDC of a point does not ----
    {
        const rhi::ShiftVec3d delta{kStep, 0.0, 0.0};
        f64 worstOld = 0.0, worstFresh = 0.0;
        int pointsChecked = 0;
        for (int trial = 0; trial < 200; ++trial) {
            const f64 eye[3] = {frand(900000.0, 1200000.0), frand(-50000.0, 50000.0), frand(100.0, 3000.0)};
            const f64 yaw = frand(-3.1, 3.1), pitch = frand(-0.4, 0.4);
            f32 cur[16], prev[16], prev2[16];
            viewProjF32(eye, yaw, pitch, cur);
            viewProjF32(eye, yaw + 0.01, pitch, prev);       // a camera that was turning
            viewProjF32(eye, yaw + 0.02, pitch, prev2);
            f32 curS[16], prevS[16], prev2S[16];
            std::memcpy(curS, cur, sizeof cur); std::memcpy(prevS, prev, sizeof prev); std::memcpy(prev2S, prev2, sizeof prev2);
            rhi::composePrevViewProjInPlace(curS, delta);    // VoxiRenderer::shiftWorld composes cur, prev and prev2
            rhi::composePrevViewProjInPlace(prevS, delta);
            rhi::composePrevViewProjInPlace(prev2S, delta);
            const f64 eyeNew[3] = {eye[0] - kStep, eye[1], eye[2]};
            f32 prevFresh[16];
            viewProjF32(eyeNew, yaw + 0.01, pitch, prevFresh);
            for (int k = 0; k < 20; ++k) {
                // A point 20 m to 300 m in front of the camera, in the OLD frame, and the same point in the new one.
                const f64 fwd[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
                const f64 d = frand(5000.0, 30000.0), sx = frand(-0.4, 0.4) * d, sz = frand(-0.2, 0.2) * d;
                const f64 pOld[3] = {eye[0] + fwd[0] * d - fwd[1] * sx, eye[1] + fwd[1] * d + fwd[0] * sx, eye[2] + fwd[2] * d + sz};
                const f64 pNew[3] = {pOld[0] - kStep, pOld[1], pOld[2]};
                f64 a[2], b[2], c[2];
                if (!project(prev, pOld, a) || !project(prevS, pNew, b) || !project(prevFresh, pNew, c)) continue;
                if (std::fabs(a[0]) > 1.5 || std::fabs(a[1]) > 1.5) continue;
                ++pointsChecked;
                worstOld = std::fmax(worstOld, std::fmax(std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1])));
                worstFresh = std::fmax(worstFresh, std::fmax(std::fabs(c[0] - b[0]), std::fabs(c[1] - b[1])));
            }
            // prev2 is composed with the same delta: it reprojects the same point identically to prev's rule. The probe is 50 m
            // down prev2's own axis: a point off to the side can sit at w ~ 0, where NDC blows up and measures nothing.
            const f64 ax[3] = {std::cos(pitch) * std::cos(yaw + 0.02), std::cos(pitch) * std::sin(yaw + 0.02), std::sin(pitch)};
            f64 q[3] = {eyeNew[0] + ax[0] * 5000.0, eyeNew[1] + ax[1] * 5000.0, eyeNew[2] + ax[2] * 5000.0}, n0[2], n1[2];
            const f64 qOld[3] = {q[0] + kStep, q[1], q[2]};
            if (project(prev2, qOld, n0) && project(prev2S, q, n1))
                worstOld = std::fmax(worstOld, std::fmax(std::fabs(n0[0] - n1[0]), std::fabs(n0[1] - n1[1])));
        }
        check(pointsChecked > 1000, "enough projected points to mean something (" + std::to_string(pointsChecked) + ")");
        check(worstOld < 2e-4, "the composed previous matrix sends a new-frame point where the old one sent the old-frame point (worst NDC "
                                   + std::to_string(worstOld) + ")");
        check(worstFresh < 5e-3, "and agrees with a matrix rebuilt at the new eye to the f32 loss of the old translation row (worst NDC "
                                     + std::to_string(worstFresh) + ")");

        // Six rebases in a row (a 100 km flight) compose to one of six regions: the same NDC for the same point, to the
        // f32 rounding of the translation row each composition ends in (1 cm at 9.8e6 cm, so well under a pixel at 50 m).
        const f64 eye[3] = {1200000.0, 1000.0, 300.0};
        f32 base[16], once[16], six[16];
        viewProjF32(eye, 0.4, 0.1, base);
        std::memcpy(once, base, sizeof base);
        std::memcpy(six, base, sizeof base);
        for (int i = 0; i < 6; ++i) rhi::composePrevViewProjInPlace(six, rhi::ShiftVec3d{kStep, 0.0, 0.0});
        rhi::composePrevViewProjInPlace(once, rhi::ShiftVec3d{6.0 * kStep, 0.0, 0.0});
        f64 worst = 0.0;
        int seen = 0;
        for (int k = 0; k < 200; ++k) {
            const f64 d = frand(5000.0, 30000.0);
            const f64 pOld[3] = {eye[0] + std::cos(0.4) * std::cos(0.1) * d - std::sin(0.4) * frand(-0.3, 0.3) * d,
                                 eye[1] + std::sin(0.4) * std::cos(0.1) * d, eye[2] + std::sin(0.1) * d};
            const f64 pNew[3] = {pOld[0] - 6.0 * kStep, pOld[1], pOld[2]};
            f64 a[2], b[2], c[2];
            if (!project(base, pOld, a) || !project(once, pNew, b) || !project(six, pNew, c)) continue;
            if (std::fabs(a[0]) > 1.5 || std::fabs(a[1]) > 1.5) continue;
            ++seen;
            worst = std::fmax(worst, std::fmax(std::fabs(a[0] - c[0]), std::fabs(a[1] - c[1])));
            worst = std::fmax(worst, std::fmax(std::fabs(b[0] - c[0]), std::fabs(b[1] - c[1])));
        }
        check(seen > 50 && worst < 1e-3, "six single-region shifts compose to one six-region shift (" + std::to_string(seen) +
                                              " points, worst NDC " + std::to_string(worst) + ")");

        // A zero delta (the identity at origin zero) leaves every history matrix bit for bit.
        f32 z[16], zc[16];
        viewProjF32(eye, 0.7, 0.0, z);
        std::memcpy(zc, z, sizeof z);
        rhi::composePrevViewProjInPlace(zc, rhi::ShiftVec3d{});
        check(std::memcmp(z, zc, sizeof z) == 0, "a zero shift leaves the matrices bit for bit");
    }

    // ---- (2) NRD2: the previous eye moves with the matrix, so a still camera sees no motion across a rebase ----
    {
        const rhi::ShiftVec3d delta{kStep, 0.0, 0.0};
        const f64 eye64[3] = {1100000.0, 2000.0, 400.0};
        const f32 eye[3] = {static_cast<f32>(eye64[0]), static_cast<f32>(eye64[1]), static_cast<f32>(eye64[2])};
        f32 prevVP[16];
        viewProjF32(eye64, 0.3, -0.05, prevVP);

        f32 relOld[16];
        prevViewProjRel(prevVP, eye, relOld);
        f32 prevVPS[16];
        std::memcpy(prevVPS, prevVP, sizeof prevVP);
        rhi::composePrevViewProjInPlace(prevVPS, delta);
        f32 prevCamS[3] = {eye[0], eye[1], eye[2]};
        rhi::shiftPoint(prevCamS, delta);            // VoxiRenderer::prevCamPos_
        f32 relNew[16];
        prevViewProjRel(prevVPS, prevCamS, relNew);
        // The relative matrix takes an OFFSET from the previous eye: the same offsets must land on the same NDC before and
        // after (rows 0..2 are untouched; row 3 differs only by the f32 rounding of the composed translation, ~0.1 cm).
        f64 worst = 0.0;
        int seen = 0;
        for (int k = 0; k < 200; ++k) {
            const f64 d = frand(5000.0, 30000.0);
            const f64 off[3] = {std::cos(0.3) * std::cos(0.05) * d - std::sin(0.3) * frand(-0.3, 0.3) * d,
                                std::sin(0.3) * std::cos(0.05) * d + std::cos(0.3) * frand(-0.3, 0.3) * d, -std::sin(0.05) * d};
            f64 a[2], b[2];
            if (!project(relOld, off, a) || !project(relNew, off, b)) continue;
            if (std::fabs(a[0]) > 1.5 || std::fabs(a[1]) > 1.5) continue;
            ++seen;
            worst = std::fmax(worst, std::fmax(std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1])));
        }
        check(seen > 50 && worst < 3e-4, "the camera-relative previous matrix reprojects the same offsets after the shift (" +
                                             std::to_string(seen) + " offsets, worst NDC " + std::to_string(worst) + ")");

        f32 eyeNew[3] = {eye[0], eye[1], eye[2]};
        rhi::shiftPoint(eyeNew, delta);              // the camera standing still, in the new frame
        const f64 camDelta = static_cast<f64>(eyeNew[0]) - static_cast<f64>(prevCamS[0]);
        check(camDelta == 0.0 && eyeNew[1] == prevCamS[1] && eyeNew[2] == prevCamS[2],
              "NRD2's camera delta (eye - prevCamPos) is exactly zero for a camera that did not move");
        const f64 stale = static_cast<f64>(eyeNew[0]) - static_cast<f64>(eye[0]);
        check(std::fabs(stale) == kStep, "negative control: with prevCamPos left unshifted NRD2 would be fed a 16.384 km camera motion");
    }

    // ---- (3) NeuRaC: the cache survives a rebase, cascade 2 by a one-time relabel ----
    {
        // A rebase of k regions along one axis moves a world cell of cascade c by k * 1,638,400 / size cells.
        bool texelsHold = true, tagsHold01 = true, relabelOk = true, controlFires = false, keepsFlags = true;
        for (int c = 0; c < static_cast<int>(nc::kCascades); ++c) {
            const i64 perRegion = nc::kOriginStepCm / static_cast<i64>(nc::kCellSizeCm[c]);
            for (int trial = 0; trial < 400; ++trial) {
                const i32 x = static_cast<i32>(rng() % 400001u) - 200000, y = static_cast<i32>(rng() % 400001u) - 200000,
                          z = static_cast<i32>(rng() % 4001u) - 2000;
                for (i64 k : {-7, -4, -3, -1, 1, 2, 4, 5, 7}) {
                    const i32 x2 = static_cast<i32>(x - k * perRegion), y2 = static_cast<i32>(y - k * perRegion);
                    texelsHold = texelsHold && nc::cellIndex(static_cast<u32>(c), x2, y2, z) == nc::cellIndex(static_cast<u32>(c), x, y, z);
                    if (c < 2) tagsHold01 = tagsHold01 && nc::tagPack(x2, y2, z) == nc::tagPack(x, y, z);
                    else {
                        const u32 meta = nc::tagPack(x, y, z) | (5u << 24) | (3u << 28);   // n_eff 5, age 3
                        const u32 dx = nc::shiftTagUnits(k), dy = nc::shiftTagUnits(k);
                        const u32 re = nc::retagMeta(meta, dx, dy);
                        relabelOk = relabelOk && (re & 0xFFFFFFu) == nc::tagPack(x2, y2, z);
                        keepsFlags = keepsFlags && (re >> 24) == (meta >> 24);
                        if (k % 4 != 0 && nc::tagPack(x2, y2, z) != nc::tagPack(x, y, z)) controlFires = true;
                    }
                }
            }
        }
        check(texelsHold, "the toroidal texel of a cell never changes across a rebase (all three cascades)");
        check(tagsHold01, "cascades 0 and 1 keep their tag byte (a region is a whole number of 256-cell tag periods)");
        check(relabelOk, "cascade 2's relabel (retagMeta by shiftTagUnits) lands on the shifted cell's tag for every region count");
        check(keepsFlags, "the relabel keeps n_eff and age");
        check(controlFires, "negative control: without the relabel cascade 2's tags DO mismatch (the cache would read empty)");

        check(nc::originShiftFlags(0, 0) == 0u, "no rebase: no flags (identity)");
        check(nc::originShiftFlags(4, 0) == 0u && nc::originShiftFlags(-8, 12) == 0u, "four regions are a whole tag period: nothing to relabel");
        const u32 f = nc::originShiftFlags(1, 2);
        check((f & nc::kFlagRetagCoarse) != 0u && (f & nc::kFlagClearAll) == 0u && ((f >> 8) & 255u) == 64u && ((f >> 16) & 255u) == 128u,
              "one region in x and two in y: the flag, the x units (64) and the y units (128), no clear-all");
    }

#ifdef AVER_REPO_ROOT
    const std::string root = AVER_REPO_ROOT;
    const std::string voxiCpp = readFile(root + "/modules/render.voxi/src/VoxiRenderer.cpp");
    const std::string voxiHpp = readFile(root + "/modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp");

    // ---- (4) NeuRaC's HLSL is the arithmetic the header states ----
    {
        const std::string hlsl = readFile(root + "/modules/render.voxi/shaders/voxi_neurac_resolve.hlsl");
        check(hlsl.find("#define RCR_FLAG_RETAG_COARSE 2u") != std::string::npos && nc::kFlagRetagCoarse == 2u,
              "the resolve and the header agree on the flag bit");
        check(hlsl.find("const uint tx = ((m & 255u) - dx) & 255u;") != std::string::npos &&
              hlsl.find("const uint ty = (((m >> 8) & 255u) - dy) & 255u;") != std::string::npos &&
              hlsl.find("(m & 0xFF000000u) | (m & 0x00FF0000u) | (ty << 8) | tx") != std::string::npos,
              "the resolve subtracts the units from the x and y tag bytes and keeps z, n_eff and age (retagMeta)");
        const usize clearAt = hlsl.find("RCR_FLAG_CLEAR_ALL) != 0u");
        const usize retagAt = hlsl.find("RCR_FLAG_RETAG_COARSE) != 0u");
        check(clearAt != std::string::npos && retagAt != std::string::npos && retagAt > clearAt,
              "a clear-all frame returns before the relabel");
        check(hlsl.find("cell >= 2u * RCR_PER_CASC") != std::string::npos, "the relabel is cascade 2 only");
    }

    // ---- (5) VoxiRenderer::shiftWorld touches every kept position, in an order that keeps the volume ----
    {
        const std::string body = bodyOf(voxiCpp, "void VoxiRenderer::shiftWorld(");
        check(!body.empty(), "VoxiRenderer::shiftWorld has a body");
        std::string missing;
        check(hasAll(body, {
            "if (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0) return;",   // identity at origin zero
            "composePrevViewProjInPlace(curViewProj_", "composePrevViewProjInPlace(prevViewProj_", "composePrevViewProjInPlace(prev2ViewProj_",
            "shiftPoint(curCamPos_", "shiftPoint(prevCamPos_", "froxel_.shiftWorld(delta)",
            "shiftCentre(authoredCenter_", "shiftCentre(center_", "shiftCentre(followLastEye_", "shiftCentre(giSnapCenter_",
            "shiftCentre(giBoxCentre_", "shiftCentre(giShadowCentre_", "shiftCentre(cascadeCentre_[c]", "cb_.voxelOrigin",
            "airVisInputs_", "for (Draw& dr : draws_) moveDraw(dr);", "for (Draw& dr : drawsPrev_) moveDraw(dr);",
            "rtInstanceData_", "ri.objectToWorld", "ri.prevObjectToWorld", "tlasInstScratch_",
            "giDrawsKey_ = giDrawsKey();", "giDrawsSubKeys(giDrawsCount_", "giHistValid_ = false;", "giVisHistValid_ = false;",
            "rc_.noteOriginShift("}, missing),
              "shiftWorld moves the history matrices, camera positions, froxel copy, GI centres, draw lists, RT rows, GI key and NeuRaC"
              + (missing.empty() ? std::string() : " (missing: " + missing + ")"));
        const usize authored = body.find("shiftCentre(authoredCenter_"), centre = body.find("shiftCentre(center_");
        check(authored != std::string::npos && centre != std::string::npos && authored < centre,
              "authoredCenter_ moves BEFORE center_ (else setVolume reads a >1 cm authored move as a level load and resets the cache)");
        const usize keyCheck = body.find("giKeyHeld = giDrawsKey() == giDrawsKey_;");
        const usize firstMove = body.find("for (Draw& dr : draws_) moveDraw(dr);");
        const usize rekey = body.find("giDrawsKey_ = giDrawsKey();");
        check(keyCheck != std::string::npos && firstMove != std::string::npos && rekey != std::string::npos &&
              keyCheck < firstMove && firstMove < rekey,
              "the GI key is judged on the unshifted list, then the lists move, then the key is recomputed over the shifted draws");
        check(body.find("giBuild_ = GiBuild{}") != std::string::npos, "a staged GI rebuild in flight is abandoned, not mixed across frames");

        // Every member that holds a world position and is NOT in the shift must be here by name, with the reason.
        const std::vector<const char*> directions = {"rtHistSunDir_", "sunDir_"};
        for (const char* d : directions)
            check(body.find(d) == std::string::npos && voxiHpp.find(d) != std::string::npos,
                  std::string(d) + " is a direction (frame-independent) and is correctly not shifted");

        const std::string froxel = readFile(root + "/modules/render.voxi/include/aver/voxi/VoxiFroxelFog.hpp");
        check(froxel.find("void shiftWorld(const rhi::ShiftVec3d& delta) { rhi::composePrevViewProjInPlace(prevViewProj_, delta); }") != std::string::npos,
              "the froxel fog's own previous matrix (a separate copy) takes the shift");
    }

    // ---- (6) the device and both backends ----
    {
        const std::string d3d = readFile(root + "/modules/rhi.d3d12/src/D3D12Device.cpp");
        const std::string d3dBody = bodyOf(d3d, "void shiftWorld(const ShiftWorldInfo& info) override");
        std::string missing;
        check(hasAll(d3dBody, {"composePrevViewProjInPlace(frameCB_.viewProj,", "composePrevViewProjInPlace(frameCB_.viewProjNoJitter,",
                               "composePrevViewProjInPlace(prevViewProj_,", "composePrevViewProjInPlace(taaLastViewProj_,",
                               "shiftPoint(fgPrevCamPos_", "worldOrigin_ = info.newOrigin;", "worldGeneration_ = info.generation;",
                               "refreshWorldOriginRows();"}, missing),
              "D3D12Device::shiftWorld composes the velocity, TAA and frame-generation history and refolds the origin rows"
              + (missing.empty() ? std::string() : " (missing: " + missing + ")"));

        const std::string vkCommon = readFile(root + "/modules/rhi.vulkan/src/VulkanCommon.hpp");
        const std::string vkBody = bodyOf(vkCommon, "void shiftWorld(const ShiftWorldInfo& info) override");
        check(hasAll(vkBody, {"composePrevViewProjInPlace(frameCB_.viewProj,", "worldOrigin_ = info.newOrigin;", "refreshWorldOriginRows();"}, missing),
              "VulkanDevice::shiftWorld installs the origin and refolds the rows (it keeps no previous matrix)"
              + (missing.empty() ? std::string() : " (missing: " + missing + ")"));

        const std::string vkDev = readFile(root + "/modules/rhi.vulkan/src/VulkanDevice.cpp");
        const char* fold = "cloudMotionFold(ShiftVec3d{static_cast<f64>(cloudWindOffset_[0])";
        check(d3d.find(fold) != std::string::npos && vkDev.find(fold) != std::string::npos,
              "both backends fold the origin into the cloud offset at setSkyAtmosphere (the two sites stay together)");
        check(d3d.find("refreshWorldOriginRows();   // the phase offsets follow the wave set") != std::string::npos &&
              vkCommon.find("refreshWorldOriginRows();   // the phase offsets follow the wave set") != std::string::npos,
              "both backends recompute the wave phase rows when the wave set changes");
        check(d3d.find("ShiftOrigin worldOrigin() const override") != std::string::npos &&
              vkCommon.find("ShiftOrigin worldOrigin() const override") != std::string::npos,
              "both backends answer worldOrigin() (water reads it)");

        const std::string water = readFile(root + "/modules/fluids/shaders/water.hlsl");
        const std::string displace = readFile(root + "/modules/fluids/shaders/water_displace.hlsl");
        check(water.find("+ gWaveLenAmp[i].z;") != std::string::npos && displace.find("+ gWaveLenAmp[i].z;") != std::string::npos,
              "both water shaders add the origin's wave phase offset");
        const std::string prelude = readFile(root + "/modules/rhi/shaders/shared_prelude.hlsl");
        check(prelude.find("averWavePhaseT(gWave[i], p, t) + gWavePhase[i]") != std::string::npos,
              "the shared wave functions add gWavePhase[i]");
    }
#endif

    AVER_INFO("VoxiShiftTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
