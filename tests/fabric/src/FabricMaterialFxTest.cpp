// FabricMaterialFxTest -- G4 R6 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/MatGame, then the .ocmat files it wrote read back with the
// engine's own loader. Every BaseMaterial3D feature group lands in the material's `fx` (or in an existing .ocmat field), the cull mode is written for every
// material, and each stored property is rated in the report (converted / approximated / unsupported) as docs/plans/fabric/G4_RENDER.md R6 lists.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double abs = 1e-4) { return std::fabs(a - b) <= abs; }

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

int main() {
    AVER_INFO("FabricMaterialFxTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/MatGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/main.tscn", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-matfx-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    // Every material the converter wrote.
    std::vector<pbr::MaterialDesc> mats;
    const std::string matDir = out1 + "/MatGame/Content/Materials";
    for (const auto& e : std::filesystem::directory_iterator(matDir, ec)) {
        if (e.path().extension() != ".ocmat") continue;
        pbr::MaterialDesc d;
        std::string err;
        if (fmt::loadOcmat(e.path().string(), d, nullptr, &err)) mats.push_back(d);
        else check(false, "loads " + e.path().string() + ": " + err);
    }
    check(mats.size() == 11, "eleven materials were written (got " + std::to_string(mats.size()) + ")");
    const auto find = [&](const std::function<bool(const pbr::MaterialDesc&)>& pred) -> const pbr::MaterialDesc* {
        for (const pbr::MaterialDesc& d : mats) if (pred(d)) return &d;
        return nullptr;
    };

    AVER_INFO("-- the default material --");
    {
        const auto* d = find([](const pbr::MaterialDesc& m) { return m.fx.cull == pbr::FxCull::Back && !m.fx.usesUv() && !m.fx.usesVertex() && m.fx.blend == pbr::FxBlend::Default &&
                                                                       !m.fx.rim && !m.fx.detail && !m.fx.heightmap && m.alphaMode == pbr::AlphaMode::Opaque && !m.twoSided && !m.fx.unshaded &&
                                                                       m.coatWeight == 0.0f && m.transmission == 0.0f && m.fx.alphaExt == pbr::FxAlphaExt::None; });
        check(d != nullptr, "an ordinary material carries cull=back and nothing else: Godot's default cull mode is written for every material");
    }

    AVER_INFO("-- UV transforms and triplanar --");
    {
        const auto* d = find([](const pbr::MaterialDesc& m) { return m.fx.uv1Triplanar; });
        check(d != nullptr, "the triplanar material exists");
        if (d) {
            check(near(d->fx.uv1Scale[0], 2) && near(d->fx.uv1Scale[1], 3) && near(d->fx.uv1Offset[0], 0.25) && near(d->fx.uv1Offset[1], 0.5), "uv1 scale (2, 3) and offset (0.25, 0.5)");
            check(d->fx.uv1WorldTriplanar && near(d->fx.uv1Sharpness, 4), "world triplanar with sharpness 4");
            check(near(d->fx.uv2Scale[0], 4) && near(d->fx.uv2Offset[0], 0.1) && d->fx.uv2Triplanar, "uv2 scale, offset and the stored uv2 triplanar flag");
            check(pbr::fxVariantKey(d->fx) & pbr::fxkey::kTri && pbr::fxVariantKey(d->fx) & pbr::fxkey::kUvx, "the variant key asks for the UV and triplanar code");
        }
        const auto* orm = find([](const pbr::MaterialDesc& m) { return near(m.fx.uv1Scale[0], 5); });
        check(orm != nullptr && orm->fx.cull == pbr::FxCull::None && orm->twoSided, "an ORMMaterial3D maps the same way; cull_mode disabled is the two-sided flag");
    }

    AVER_INFO("-- pipeline state --");
    {
        const auto* d = find([](const pbr::MaterialDesc& m) { return m.fx.renderPriority == 5; });
        check(d != nullptr, "the state material exists");
        if (d) {
            check(d->alphaMode == pbr::AlphaMode::Blend && d->fx.blend == pbr::FxBlend::Add, "alpha blend with the add blend mode");
            check(d->fx.depthDraw == pbr::FxDepthDraw::Always && d->fx.noDepthTest && d->fx.unshaded && d->twoSided, "depth draw always, no depth test, unshaded, both faces");
            const u32 key = pbr::fxVariantKey(d->fx);
            check(((key >> pbr::fxkey::kBlendShift) & 7u) == 1u && (key & pbr::fxkey::kNoDepthTest) && (key & pbr::fxkey::kUnshaded), "the key carries blend add, no depth test and unshaded");
        }
        const auto* h = find([](const pbr::MaterialDesc& m) { return m.fx.alphaExt == pbr::FxAlphaExt::Antialias; });
        check(h != nullptr && h->alphaMode == pbr::AlphaMode::Mask && near(h->fx.alphaHashScale, 2.0), "alpha hash with alpha antialiasing: the dithered coverage path, hash scale 2");
        const auto* s = find([](const pbr::MaterialDesc& m) { return m.alphaMode == pbr::AlphaMode::Mask && m.fx.cull == pbr::FxCull::Front; });
        check(s != nullptr && near(s->alphaCutoff, 0.3), "alpha scissor keeps the threshold; front culling is written");
        const auto* m = find([](const pbr::MaterialDesc& x) { return x.fx.blend == pbr::FxBlend::Mul; });
        check(m != nullptr && m->alphaMode == pbr::AlphaMode::Blend, "blend mode multiply puts the surface in the transparent pass (as Godot does)");
    }

    AVER_INFO("-- vertex stage and fades --");
    {
        const auto* d = find([](const pbr::MaterialDesc& m) { return m.fx.billboard == pbr::FxBillboard::FixedY; });
        check(d != nullptr, "the billboard material exists");
        if (d) {
            check(d->fx.billboardKeepScale && d->fx.fixedSize && d->fx.grow && near(d->fx.growCm, 2.0), "keep scale, fixed size and grow 0.02 m = 2 cm");
            check(pbr::fxVariantKey(d->fx) & pbr::fxkey::kVertex, "the key asks for the vertex stage");
        }
        const auto* f = find([](const pbr::MaterialDesc& m) { return m.fx.distanceFade == pbr::FxDistanceFade::PixelDither; });
        check(f != nullptr && near(f->fx.fadeMinCm, 200) && near(f->fx.fadeMaxCm, 2000), "distance fade pixel dither, 2 m to 20 m = 200 to 2000 cm");
        if (f) check(pbr::fxVariantKey(f->fx) & pbr::fxkey::kLateZ, "a discarding fade compiles late-Z");
    }

    AVER_INFO("-- surface features --");
    {
        const auto* d = find([](const pbr::MaterialDesc& m) { return m.fx.rim; });
        check(d != nullptr, "the surface material exists");
        if (d) {
            check(near(d->fx.rimAmount, 0.8) && near(d->fx.rimTint, 0.2), "rim and rim tint");
            check(d->fx.anisotropy && near(d->fx.anisotropyAmount, 0.5), "anisotropy");
            check(d->fx.backlight && near(d->fx.backlightColor[0], 0.2140, 2e-3), "backlight, the colour decoded to linear");
            check(near(d->coatWeight, 0.7) && near(d->coatRoughness, 0.2), "clearcoat maps onto the coat fields");
            check(near(d->transmission, 0.85) && near(d->ior, 1.5), "refraction maps onto transmission and ior");
            check(near(d->subsurfaceWeight, 0.6), "subsurface scattering keeps its weight");
            check(d->fx.emissionOp == 1 && near(d->fx.aoLightAffect, 0.5), "emission operator multiply and ao light affect");
            check(near(d->reflectance, 0.02), "metallic_specular 0.25 is F0 0.02");
            check(d->fx.specularMode == pbr::FxSpecular::Disabled && d->fx.diffuseMode == pbr::FxDiffuse::LambertWrap, "specular disabled, diffuse lambert wrap");
            check(d->fx.noReceiveShadows && d->fx.disableAmbient, "no shadows received, no ambient light");
        }
        const auto* h = find([](const pbr::MaterialDesc& m) { return m.fx.heightmap; });
        check(h != nullptr, "the heightmap and detail material exists");
        if (h) {
            check(h->fx.deepParallax && h->fx.minLayers == 4 && h->fx.maxLayers == 16 && h->fx.flipTangent && near(h->fx.heightScale, 8), "parallax: deep, layers 4 to 16, flipped tangent, scale 8");
            check(h->fx.detail && h->fx.detailBlend == pbr::FxBlendOp::Mul && h->fx.detailUv == 1, "detail multiplied on UV2");
            check(!h->textures[static_cast<u32>(pbr::TextureSlot::Layer1MetalRough)].empty() && !h->textures[static_cast<u32>(pbr::TextureSlot::Layer1BaseColor)].empty() &&
                      !h->textures[static_cast<u32>(pbr::TextureSlot::Layer1Normal)].empty(),
                  "the heightmap, detail albedo and detail normal ride the layer-1 slots");
        }
    }

    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto rated = [&](const char* prop, const char* rating) {
            std::istringstream is(jsonl);
            std::string line;
            const std::string a = std::string("\"property\":\"") + prop + "\"", b = std::string("\"rating\":\"") + rating + "\"";
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) return true;
            return false;
        };
        check(rated("uv1_scale", "converted") && rated("uv1_triplanar", "converted") && rated("uv2_triplanar", "unsupported"), "uv1 converted, uv2 triplanar unsupported");
        check(rated("cull_mode", "converted") && rated("blend_mode", "converted") && rated("depth_draw_mode", "converted") && rated("render_priority", "converted") &&
                  rated("no_depth_test", "converted") && rated("shading_mode", "converted"), "state properties are converted");
        check(rated("transparency", "approximated") && rated("alpha_antialiasing_mode", "approximated"), "alpha hash and alpha antialiasing are approximated");
        check(rated("billboard_mode", "converted") && rated("fixed_size", "converted") && rated("grow", "converted") && rated("distance_fade_mode", "converted"), "vertex stage and fade converted");
        check(rated("proximity_fade_enabled", "unsupported") && rated("disable_fog", "unsupported"), "proximity fade and disable_fog are unsupported, with reasons");
        check(rated("rim_enabled", "approximated") && rated("anisotropy_enabled", "approximated") && rated("backlight_enabled", "approximated") && rated("refraction_enabled", "approximated") &&
                  rated("clearcoat_enabled", "approximated") && rated("heightmap_deep_parallax", "approximated"), "rim, anisotropy, backlight, refraction, clearcoat and deep parallax are approximated");
        check(rated("heightmap_enabled", "converted") && rated("detail_enabled", "converted") && rated("detail_mask", "unsupported"), "heightmap and detail converted, the detail mask unsupported");
        check(rated("texture_filter", "approximated") && rated("specular_mode", "converted") && rated("diffuse_mode", "approximated"), "texture filter, specular and diffuse modes rated");
        check(!rated("uv1_scale", "unsupported"), "uv1_scale is no longer unsupported (the G3 gap)");
    }

    AVER_INFO("FabricMaterialFxTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
