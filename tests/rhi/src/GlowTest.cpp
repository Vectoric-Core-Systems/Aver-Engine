// GlowTest -- no device. The glow blend modes and the debanding offset (rhi/Glow.hpp) against hand-computed reference values; which settings select
// the AVER_POST_TONEMAP_EXT composite (the defaults must keep the ordinary one); the PostCB packing; the HLSL read back as text (the ordinary
// composite's bloom line is unchanged, the new code sits behind the define).
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/Glow.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/Tonemap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace aver;
using namespace aver::rhi;

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static std::string slurp(const char* rel) {
    std::string out;
    readFileText(std::string(AVER_REPO_ROOT) + "/" + rel, out);
    out.erase(std::remove(out.begin(), out.end(), ''), out.end());
    return out;
}
static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int main() {
    using glow::Vec3f;
    AVER_INFO("-- the blend modes --");
    {
        const Vec3f c{{0.5f, 0.25f, 2.0f}}, g{{0.4f, 0.2f, 1.0f}};
        const Vec3f add = glow::blend(kGlowAdd, c, g, 0.5f, 1.0f, 0.0f);
        check(near(add.v[0], 0.7, 1e-6) && near(add.v[1], 0.35, 1e-6) && near(add.v[2], 2.5, 1e-6), "add is c + g * intensity, the ordinary composite's line");
        const Vec3f scr = glow::blend(kGlowScreen, c, g, 1.0f, 1.0f, 0.0f);
        check(near(scr.v[0], 0.5 + 0.4 - 0.2, 1e-6) && near(scr.v[1], 0.25 + 0.2 - 0.05, 1e-6) && near(scr.v[2], 2.0 + 1.0 - 2.0, 1e-6), "screen is c + g - c g");
        const Vec3f rep = glow::blend(kGlowReplace, c, g, 0.5f, 1.0f, 0.0f);
        check(near(rep.v[0], 0.2, 1e-6) && near(rep.v[2], 0.5, 1e-6), "replace is the glow alone, scaled by intensity");
        const Vec3f mix = glow::blend(kGlowMix, c, g, 0.25f, 1.0f, 0.0f);
        check(near(mix.v[0], 0.5 + (0.4 - 0.5) * 0.25, 1e-6) && near(mix.v[2], 2.0 + (1.0 - 2.0) * 0.25, 1e-6), "mix lerps the frame toward the unscaled glow by intensity");
        const Vec3f zero = glow::blend(kGlowSoftLight, c, Vec3f{{0, 0, 0}}, 1.0f, 1.0f, 0.0f);
        check(near(zero.v[0], 0.5, 1e-6) && near(zero.v[1], 0.25, 1e-6), "soft light with no glow leaves the frame alone");
        const double d = ((16 * 0.16 - 12) * 0.16 + 4) * 0.16;
        const Vec3f sl = glow::blend(kGlowSoftLight, Vec3f{{0.16f, 0.16f, 0.16f}}, Vec3f{{0.6f, 0.6f, 0.6f}}, 1.0f, 1.0f, 0.0f);
        check(near(sl.v[0], 0.16 + 0.6 * (d - 0.16), 1e-5), "soft light, the dark-backdrop branch (glow 0.6 maps to a source of 0.8)");
        const Vec3f sl2 = glow::blend(kGlowSoftLight, Vec3f{{0.64f, 0.64f, 0.64f}}, Vec3f{{0.6f, 0.6f, 0.6f}}, 1.0f, 1.0f, 0.0f);
        check(near(sl2.v[0], 0.64 + 0.6 * (std::sqrt(0.64) - 0.64), 1e-5), "soft light, the bright-backdrop branch");
    }
    {
        const Vec3f c{{0.0f, 0.0f, 0.0f}}, g{{3.0f, 3.0f, 3.0f}};
        const Vec3f capped = glow::blend(kGlowAdd, c, g, 1.0f, 1.0f, 1.0f);
        check(near(capped.v[0], 1.0, 1e-5) && near(capped.v[1], 1.0, 1e-5), "a luminance cap of 1 brings a grey 3 glow to 1");
        const Vec3f str = glow::blend(kGlowAdd, c, g, 1.0f, 0.5f, 0.0f);
        check(near(str.v[0], 1.5, 1e-6), "strength scales the glow");
        check(near(glow::debandOffset(0.0f), -0.5 / 255.0, 1e-9) && near(glow::debandOffset(0.5f), 0.0, 1e-9), "the debanding offset spans half a step either way");
        check(clampGlowBlend(-3) == 0 && clampGlowBlend(9) == 4 && clampGlowBlend(2) == 2, "the blend mode is clamped to 0..4");
    }

    AVER_INFO("-- which composite --");
    {
        PostSettings p;
        check(!postNeedsExtComposite(p) && !postUsesGlowExt(p), "the defaults use the ordinary composite");
        p.glowBlend = 2; check(postNeedsExtComposite(p) && postUsesGlowExt(p), "a glow blend selects the EXT composite");
        p = PostSettings{}; p.deband = 1; check(postNeedsExtComposite(p), "debanding selects it");
        p = PostSettings{}; p.glowLumCap = 4.0f; check(postNeedsExtComposite(p), "a luminance cap selects it");
        p = PostSettings{}; p.glowStrength = 0.5f; check(postNeedsExtComposite(p), "a strength other than 1 selects it");
        PostSettings a, b;
        check(postSettingsEqual(a, b), "equal settings are equal");
        b = a; b.glowBlend = 1; check(!postSettingsEqual(a, b), "postSettingsEqual sees the blend");
        b = a; b.deband = 1; check(!postSettingsEqual(a, b), "postSettingsEqual sees debanding");
        b = a; b.glowLumCap = 2; check(!postSettingsEqual(a, b), "postSettingsEqual sees the cap");
        b = a; b.glowStrength = 2; check(!postSettingsEqual(a, b), "postSettingsEqual sees the strength");
    }

    AVER_INFO("-- the post constants --");
    {
        PostCB cb{};
        fillPostLens(cb, PostSettings{}, 1920, 1080, false, 0, 0);
        check(cb.ext2[0] == 0.0f && cb.ext2[1] == 0.0f && cb.ext2[2] == 1.0f && cb.ext2[3] == 0.0f, "the defaults pack to add, no cap, strength 1, no debanding");
        PostSettings s;
        s.glowBlend = 9; s.glowLumCap = -2.0f; s.glowStrength = -1.0f; s.deband = 7;
        fillPostLens(cb, s, 1920, 1080, false, 0, 0);
        check(cb.ext2[0] == 4.0f && cb.ext2[1] == 0.0f && cb.ext2[2] == 0.0f && cb.ext2[3] == 1.0f, "a mode past the last clamps, negatives clamp to 0, any deband is 1");
    }

    AVER_INFO("-- the HLSL --");
    {
        const std::string color = slurp("modules/rhi/shaders/color.hlsli");
        const std::string post = slurp("modules/rhi/shaders/post.hlsl");
        check(!color.empty() && !post.empty(), "the shader files were read");
        check(has(post, "#else\n    c += gPostBloomTex.SampleLevel(gPostSamp, srcUv, 0).rgb * gPostTone.y;\n#endif"), "the ordinary composite adds the bloom exactly as it did");
        const size_t ext = post.find("float4 gPostExt;");
        const size_t ext2 = post.find("float4 gPostExt2;");
        const size_t end = post.find("#endif", ext);
        check(ext != std::string::npos && ext < ext2 && ext2 < end, "gPostExt2 is declared under the define, right after gPostExt");
        check(has(post, "c = averGlowBlend(c, gPostBloomTex.SampleLevel(gPostSamp, srcUv, 0).rgb, float4(gPostExt2.xyz, gPostTone.y));"), "the EXT composite blends the glow");
        const size_t deb = post.find("if (gPostExt2.w > 0.5)");
        check(deb != std::string::npos && post.rfind("#ifdef AVER_POST_TONEMAP_EXT", deb) > post.rfind("#endif", deb), "debanding is inside the define");
        check(deb != std::string::npos && post.find("outc = saturate(outc + n * gPostLens.y") < deb && post.find("return float4(outc, 1.0);", deb) != std::string::npos,
              "debanding is the last step, after the grain");
        const size_t extBegin = color.find("#ifdef AVER_POST_TONEMAP_EXT");
        const size_t blend = color.find("float3 averGlowBlend(");
        check(extBegin != std::string::npos && blend > extBegin && color.find("float3 acesTonemap(") < extBegin, "averGlowBlend sits inside the define");
        check(has(color, "return cb + (2.0 * cs - 1.0) * (d - cb);") && has(color, "((16.0 * cb - 12.0) * cb + 4.0) * cb") && has(color, "max((cb + g) - cb * g, 0.0)"),
              "the HLSL curves are the twin's");
    }

    AVER_INFO("GlowTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
