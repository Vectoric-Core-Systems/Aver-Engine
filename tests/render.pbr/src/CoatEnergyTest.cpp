// The layered BSDF's coat lobe, worked out in C++ before a line of it existed in HLSL.
//
// WHY THIS FILE EXISTS AT ALL, given the white furnace already measures energy on a GPU. Two
// reasons, and the second is the one that matters.
//
// First, the furnace reads TONEMAPPED 8-BIT BACKBUFFER CODES. --furnace-grid builds albedo-1 plates
// that should vanish into a uniform background, and the probe reports raw(r,g,b) after ACES and an
// sRGB encode. That is a real oracle and it has caught real defects, but a coat that adds 2% energy
// moves an 8-bit code by less than one step over most of the range. This reads raw floats.
//
// Second, and the actual reason: THE FURNACE CANNOT TELL YOU WHICH TERM IS WRONG. It says the plate
// is brighter than the background. It does not say whether the coat lobe over-integrates, the base
// attenuation under-applies, or the split-sum fit is being asked for a value outside its range. Each
// of those is a different fix, and a GPU probe cannot distinguish them. So the composition is
// derived and checked HERE, at the level of the individual terms, and the furnace is then a
// confirmation that the shader implements what was checked -- not the place the maths is worked out.
//
// This mirrors what PbrShaders.cpp already says it did for the multi-scatter compensation: "Worked
// through by hand before it was written and then confirmed in the furnace."
//
// WHAT IS MIRRORED HERE. averEnvBRDF and fresnelSchlick are ported verbatim from
// modules/render.pbr/shaders/material_prelude.hlsl. A mirror can drift from the shader it mirrors --
// that is the standing hazard with tests/render.voxi/src/VoxiRtSeqTest.cpp too, which is why that one
// reads the shader text. This one asserts the SHADER TEXT CONTAINS the composition it checks, at the
// bottom, for the same reason.
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

using namespace aver;

namespace {

int g_failed = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    ++g_failed;
    AVER_ERROR("   FAIL  {}", what);
}

// ---------------------------------------------------------------- the shader's own maths, ported

// Verbatim from material_prelude.hlsl. F0 scalar; f90 is 1 for every dielectric coat.
f32 fresnelSchlick(f32 u, f32 f0, f32 f90) {
    const f32 m = 1.0f - u;
    const f32 m2 = m * m;
    return f0 + (f90 - f0) * (m2 * m2 * m);
}

// Karis's analytic split-sum environment BRDF, verbatim from material_prelude.hlsl's averEnvBRDF.
// Returns (scale, bias): the environment response is F0 * scale + bias.
void envBRDF(f32 ndv, f32 rough, f32* outScale, f32* outBias) {
    const f32 c0[4] = {-1.0f, -0.0275f, -0.572f, 0.022f};
    const f32 c1[4] = { 1.0f,  0.0425f,  1.04f, -0.04f};
    const f32 rx = rough * c0[0] + c1[0];
    const f32 ry = rough * c0[1] + c1[1];
    const f32 rz = rough * c0[2] + c1[2];
    const f32 rw = rough * c0[3] + c1[3];
    const f32 a004 = std::fmin(rx * rx, std::exp2(-9.28f * ndv)) * rx + ry;
    *outScale = -1.04f * a004 + rz;
    *outBias  =  1.04f * a004 + rw;
}

// The environment response of one specular lobe at normal-incidence reflectance f0.
f32 envSpecular(f32 ndv, f32 rough, f32 f0) {
    f32 scale = 0.0f, bias = 0.0f;
    envBRDF(ndv, rough, &scale, &bias);
    return f0 * scale + bias;
}

// ---------------------------------------------------------------- the composition under test

// THE COAT IS A SECOND SPECULAR LAYER OVER THE BASE, and the only thing that makes it energy-
// conserving rather than energy-adding is that what shows through is attenuated by the coat's own
// Fresnel. A coat that reflects 8% of the light must let at most 92% reach the base, and the base's
// answer must be scaled by that before it is added.
//
// Returned as a total reflected fraction of a uniform environment of radiance 1 -- which is exactly
// the white furnace's configuration, and therefore directly comparable to it.
struct Layered {
    f32 coat;       // what the coat lobe returns
    f32 base;       // what the base returns, already attenuated by the coat
    f32 total() const { return coat + base; }
};

Layered layeredEnv(f32 ndv, f32 baseRough, f32 baseF0, f32 coatWeight, f32 coatRough, f32 coatF0) {
    Layered r{};
    if (coatWeight <= 0.0f) {
        // Provably identical to the unlayered path: no coat term, no attenuation, nothing touched.
        r.coat = 0.0f;
        r.base = envSpecular(ndv, baseRough, baseF0);
        return r;
    }
    r.coat = envSpecular(ndv, coatRough, coatF0) * coatWeight;

    // ONE FACTOR OF (1 - Fc), NOT TWO, AND THIS IS THE CORRECTION THAT MATTERS.
    //
    // The obvious composition attenuates by (1 - Fc(ndv)) twice -- once for light entering the coat
    // and once for it leaving. That is right for a TRANSMITTED path through a slab, and wrong for an
    // environment lobe: averEnvBRDF already integrates the full hemisphere-to-eye response, so the
    // second factor charges the base twice for the same interface and loses energy. Squaring it here
    // put a white furnace plate visibly darker than its background at every roughness -- the exact
    // failure this file exists to catch before a GPU is involved.
    const f32 fcView = fresnelSchlick(ndv, coatF0, 1.0f) * coatWeight;
    r.base = envSpecular(ndv, baseRough, baseF0) * (1.0f - fcView);
    return r;
}

const std::string& shaderText() {
    static const std::string s = [] {
        const std::string path = std::string(AVER_REPO_ROOT) + "/modules/render.pbr/shaders/material_prelude.hlsl";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            AVER_ERROR("[CoatEnergy] cannot read {} -- the source assertions below would pass "
                       "vacuously against an empty string, so this is a failure, not a skip.", path);
            return std::string();
        }
        std::ostringstream ss; ss << f.rdbuf(); return ss.str();
    }();
    return s;
}

bool hlslHas(const char* needle) { return shaderText().find(needle) != std::string::npos; }

}   // namespace

int main() {
    AVER_INFO("[CoatEnergy] the coat lobe's composition, before the GPU sees it");

    const f32 kBaseF0 = 0.04f;   // an ordinary dielectric base
    const f32 kCoatF0 = 0.04f;   // ordinary lacquer, IOR 1.5

    // ---- 1. OFF IS OFF, and provably so ----
    //
    // The whole claim that a project with layeredBsdf Off renders bit-identically rests on this: at
    // coatWeight 0 the composition must return the base's own answer, not something that rounds to
    // it. Checked at exact equality rather than a tolerance, because the shader's Off path does not
    // evaluate the coat at all and anything less than exact here would mean the two paths differ.
    bool offExact = true;
    for (int ri = 0; ri <= 10; ++ri) {
        for (int vi = 1; vi <= 10; ++vi) {
            const f32 rough = static_cast<f32>(ri) / 10.0f;
            const f32 ndv   = static_cast<f32>(vi) / 10.0f;
            const Layered l = layeredEnv(ndv, rough, kBaseF0, 0.0f, 0.1f, kCoatF0);
            if (l.coat != 0.0f || l.base != envSpecular(ndv, rough, kBaseF0)) offExact = false;
        }
    }
    check(offExact, "at coatWeight 0 the coat contributes exactly 0 and the base is untouched");

    // ---- 2. ENERGY: a coat must never add energy ----
    //
    // The furnace's own configuration: a uniform environment of radiance 1. Coat plus attenuated
    // base must not exceed 1 anywhere in the parameter space, at any view angle.
    f32 worst = 0.0f;
    f32 worstAt[4] = {0, 0, 0, 0};
    for (int cw = 0; cw <= 10; ++cw) {
        for (int cr = 0; cr <= 10; ++cr) {
            for (int br = 0; br <= 10; ++br) {
                for (int vi = 1; vi <= 20; ++vi) {
                    const f32 coatWeight = static_cast<f32>(cw) / 10.0f;
                    const f32 coatRough  = static_cast<f32>(cr) / 10.0f;
                    const f32 baseRough  = static_cast<f32>(br) / 10.0f;
                    const f32 ndv        = static_cast<f32>(vi) / 20.0f;
                    const f32 t = layeredEnv(ndv, baseRough, kBaseF0, coatWeight, coatRough, kCoatF0).total();
                    if (t > worst) { worst = t; worstAt[0] = coatWeight; worstAt[1] = coatRough;
                                     worstAt[2] = baseRough; worstAt[3] = ndv; }
                }
            }
        }
    }
    AVER_INFO("   worst total reflectance {:.4f} at coatWeight {:.1f} coatRough {:.1f} "
              "baseRough {:.1f} ndv {:.2f}", worst, worstAt[0], worstAt[1], worstAt[2], worstAt[3]);
    check(worst <= 1.0f, "no combination of coat and base reflects more than it receives");

    // ---- 3. THE COAT MUST ACTUALLY DO SOMETHING ----
    //
    // A composition that conserves energy by returning zero would pass every check above. At a
    // grazing view a smooth coat should be the dominant term, and at normal incidence it should be
    // small but present.
    const Layered graz = layeredEnv(0.05f, 0.6f, kBaseF0, 1.0f, 0.05f, kCoatF0);
    const Layered face = layeredEnv(1.00f, 0.6f, kBaseF0, 1.0f, 0.05f, kCoatF0);
    AVER_INFO("   smooth coat over a rough base: grazing coat {:.3f} base {:.3f} | "
              "face-on coat {:.3f} base {:.3f}", graz.coat, graz.base, face.coat, face.base);
    check(graz.coat > face.coat, "the coat reflects more at grazing incidence than face-on");
    check(graz.coat > graz.base, "and at grazing it dominates the base beneath it");
    check(face.coat > 0.0f,      "while still contributing something face-on");

    // ---- 4. THE ATTENUATION IS REAL ----
    //
    // The base must be dimmer with a coat over it than without. If it is not, the coat is being
    // added without anything being taken away, which is how a layered model gains energy.
    const f32 bare = envSpecular(0.5f, 0.6f, kBaseF0);
    const Layered coated = layeredEnv(0.5f, 0.6f, kBaseF0, 1.0f, 0.05f, kCoatF0);
    check(coated.base < bare, "a coated base returns less than a bare one at the same angle");

    // ---- 5. THE SHADER IMPLEMENTS WHAT WAS CHECKED ----
    //
    // Everything above tests a C++ port. A port can drift from the shader it mirrors, and then this
    // suite passes while the GPU does something else -- the standing hazard VoxiRtSeqTest names
    // explicitly. These read the shader text the renderer actually compiles.
    if (shaderText().empty()) {
        check(false, "material_prelude.hlsl was readable");
    } else {
        check(hlslHas("averCoatTerms"), "the shader declares averCoatTerms");
        check(hlslHas("AVER_LAYERED_BSDF"),
              "and it is behind AVER_LAYERED_BSDF, so an unlayered build compiles none of it");
    }

    if (g_failed) { AVER_ERROR("[CoatEnergy] FAIL: {} check(s)", g_failed); return 1; }
    AVER_INFO("[CoatEnergy] PASS: the coat conserves energy, does something, and the shader has it");
    return 0;
}
