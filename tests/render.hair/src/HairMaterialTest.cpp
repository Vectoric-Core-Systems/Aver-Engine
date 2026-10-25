// HairMaterialTest -- the hair shading model's material side (docs/plans/wave11/SOFTBODY_HAIR.md 2.17). HEADLESS: no device.
//
// What is decided here: that a Hair material packs into the SAME 192-byte block as every other material (the five mirrors do not change), that the
// parameters land in the slots the shader reads them from, that the Coat and Subsurface flags stay clear for it (so the standard model, run on a ray hit,
// sees zero weights), that the library clamps what the lobes divide by, and that the HLSL's flag and model ids are the C++ ones. The shading arithmetic
// itself is decided in the headed look checks (docs/HAIR.md).
// Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"

#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef AVER_PBR_SHADER_DIR
#error "AVER_PBR_SHADER_DIR must name modules/render.pbr/shaders"
#endif

using namespace aver;
using namespace aver::pbr;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static bool near1(f32 a, f32 b, f32 tol = 1e-5f) { return std::fabs(a - b) <= tol; }

static f32 lin(f32 c) { return std::pow(c, 2.2f); }

int main() {
    AVER_INFO("=== packing ===");
    static_assert(sizeof(MaterialConstants) == 192, "the block must not grow for hair");
    {
        MaterialDesc d;
        d.shadingModel = ShadingModel::Hair;
        d.baseColorFactor[0] = 0.35f; d.baseColorFactor[1] = 0.22f; d.baseColorFactor[2] = 0.12f;
        d.hair.tipColor[0] = 0.55f; d.hair.tipColor[1] = 0.40f; d.hair.tipColor[2] = 0.25f;
        d.hair.colorVariation = 0.2f;
        d.hair.roughness = 0.3f;
        d.hair.shift = 0.09f;
        d.hair.specular = 0.8f;
        d.hair.tt = 0.7f;
        d.hair.trt = 0.6f;
        d.hair.azimuthRough = 0.5f;
        d.hair.tint[0] = 0.9f; d.hair.tint[1] = 0.6f; d.hair.tint[2] = 0.4f;
        d.hair.scatter = 0.45f;
        // Fields a Standard material would use, set to values that would turn its terms ON if the hair guard let them through.
        d.coatWeight = 1.0f;
        d.subsurfaceWeight = 1.0f;
        d.lightIntensity = 2.0f;

        const MaterialConstants c = packMaterial(d);
        check((c.flags & MaterialFlag_Hair) != 0, "the Hair flag is set");
        check(MaterialFlag_Hair == (1u << 18), "and it is bit 18");
        check((c.flags & MaterialFlag_TwoSided) != 0, "a ribbon is two sided");
        check((c.flags & (MaterialFlag_Coat | MaterialFlag_Subsurface | MaterialFlag_Light)) == 0, "the Coat, Subsurface and Light flags stay clear");

        check(near1(c.roughnessFactor, 0.3f), "beta is in roughnessFactor");
        check(near1(c.coatWeight, 0.8f) && near1(c.coatRoughness, 0.5f) && near1(c.coatF0, 0.09f), "R weight, azimuthal roughness and the cuticle tilt are in the coat slots");
        check(near1(c.subsurfaceWeight, 0.7f) && near1(c.subsurfaceRadius, 0.6f), "TT and TRT weights are in the subsurface slots");
        check(near1(c.occlusionStrength, 0.45f) && near1(c.metallicFactor, 0.2f), "the scatter weight and the colour variation are in occlusionStrength and metallicFactor");
        check(near1(c.emissiveFactor[0], lin(0.55f), 1e-4f) && near1(c.emissiveFactor[1], lin(0.40f), 1e-4f) && near1(c.emissiveFactor[2], lin(0.25f), 1e-4f),
              "the tip colour is in emissiveFactor, decoded to linear (the shader does not decode emissive)");
        check(near1(c.subsurfaceColor[0], lin(0.9f), 1e-4f) && near1(c.subsurfaceColor[2], lin(0.4f), 1e-4f), "the tint is in subsurfaceColor, decoded to linear");
        check(near1(c.baseColorFactor[0], lin(0.35f), 1e-4f) && near1(c.baseColorFactor[3], 1.0f), "the root colour is the base colour");
    }
    {
        MaterialDesc d;     // a plain material: nothing of this feature may touch it
        d.coatWeight = 0.5f;
        d.subsurfaceWeight = 0.5f;
        d.emissiveFactor[0] = 0.25f;
        const MaterialConstants c = packMaterial(d);
        check((c.flags & MaterialFlag_Hair) == 0, "a Standard material has no Hair flag");
        check((c.flags & MaterialFlag_Coat) != 0 && (c.flags & MaterialFlag_Subsurface) != 0, "and still gets its Coat and Subsurface flags");
        check(near1(c.emissiveFactor[0], 0.25f), "and its emissive is untouched");
        check(d.shadingModel == ShadingModel::Standard, "Standard is the default model");
    }

    AVER_INFO("=== the library clamps what the lobes divide by ===");
    {
        MaterialDesc d;
        d.shadingModel = ShadingModel::Hair;
        d.hair.roughness = 0.0f;
        d.hair.azimuthRough = -1.0f;
        d.hair.shift = 5.0f;
        d.hair.tt = 100.0f;
        d.hair.scatter = 7.0f;
        d.hair.tipColor[1] = -2.0f;
        MaterialLibrary& lib = MaterialLibrary::get();
        const MaterialHandle h = lib.create(d);
        check(h != 0, "the material is created");
        const MaterialDesc* s = lib.desc(h);
        check(s && s->hair.roughness >= 0.02f, "beta has a floor (a zero width would divide by zero in the Gaussian)");
        check(s && s->hair.azimuthRough >= 0.02f && s->hair.shift <= 0.35f && s->hair.tt <= 4.0f && s->hair.scatter <= 1.0f && s->hair.tipColor[1] >= 0.0f,
              "the other hair parameters are clamped to their ranges");
        MaterialDesc bad;
        bad.shadingModel = static_cast<ShadingModel>(77);
        const MaterialHandle h2 = lib.create(bad);
        const MaterialDesc* s2 = lib.desc(h2);
        check(s2 && s2->shadingModel == ShadingModel::Standard, "an unknown shading model reads as Standard");
        lib.destroy(h);
        lib.destroy(h2);
    }

    AVER_INFO("=== the HLSL mirrors ===");
    {
        std::ifstream f(std::string(AVER_PBR_SHADER_DIR) + "/material_prelude.hlsl", std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string hlsl = ss.str();
        check(!hlsl.empty(), "material_prelude.hlsl can be read");
        std::smatch m;
        check(std::regex_search(hlsl, m, std::regex(R"(#define\s+AVER_MAT_HAIR\s+\(1u\s*<<\s*(\d+)\))")) && std::stoul(m[1].str()) == 18,
              "AVER_MAT_HAIR is 1u << 18, the C++ flag");
        check(std::regex_search(hlsl, m, std::regex(R"(#define\s+AVER_MODEL_HAIR\s+(\d+)u)")) && std::stoul(m[1].str()) == 2, "AVER_MODEL_HAIR is 2");
        check(hlsl.find("float3 averHairDirect(") != std::string::npos && hlsl.find("float3 averHairIndirect(") != std::string::npos &&
                  hlsl.find("float3 averHairInit(") != std::string::npos,
              "the evaluator functions exist");
        check(hlsl.find("case AVER_MODEL_HAIR:") != std::string::npos, "and the dispatch switches know the model");
        check(hlsl.find("(gMaterialFlags & AVER_MAT_HAIR) ? AVER_MODEL_HAIR : gShadingModel") != std::string::npos, "the model is chosen from the material flag");
        // The functions are declared before use: averHairInit before the composition, the lobes after averEnvBRDF.
        const size_t init = hlsl.find("float3 averHairInit("), compose = hlsl.find("AverSurface averComposeSurface("),
                     env = hlsl.find("float2 averEnvBRDF("), direct = hlsl.find("float3 averHairDirect("), indirect = hlsl.find("float3 averHairIndirect("),
                     shade = hlsl.find("float3 averShadeDirect(");
        check(init < compose, "averHairInit is declared before averComposeSurface calls it");
        check(env < indirect && direct < shade && indirect < shade, "averHairIndirect is declared after averEnvBRDF, and both before the shade functions");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
