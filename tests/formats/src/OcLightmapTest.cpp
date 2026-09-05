// OcLightmapTest -- the .oclightmap container and its RGB9E5 texel encoding.
//
// WHAT THIS IS ACTUALLY GUARDING. A lightmap stores IRRADIANCE, not colour, so the interesting
// values are the ones above 1.0 -- an encoding that quietly clamps to LDR would produce a file that
// round-trips perfectly on every test value anyone reaches for first (0.5, 1.0, black) and destroys
// every real bake. So the round trip below deliberately carries values in the hundreds, and the
// header's own claim -- that RGB9E5 makes a negative texel unrepresentable rather than merely
// unlikely -- is checked by feeding it one.
//
// The truncation and bad-magic cases matter for a different reason: this format is written into a
// project's derived data, where a half-written file after a crash is normal rather than exotic. A
// reader that returns success on a truncated file hands the renderer garbage UVs.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLightmap.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
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

namespace {

// Relative, because RGB9E5 is a floating format: a fixed epsilon that passes at 0.5 is meaningless
// at 400. The tolerance is the quantisation step the shared 9-bit mantissa actually gives, with
// slack for the shared exponent pulling a dimmer channel down.
bool near(f32 got, f32 want, f32 rel = 0.01f) {
    const f32 scale = std::fabs(want) > 1.0f ? std::fabs(want) : 1.0f;
    return std::fabs(got - want) <= rel * scale;
}

fmt::OcLightmap makeLightmap() {
    fmt::OcLightmap lm;
    lm.width = 4;
    lm.height = 3;
    lm.vertexCount = 5;
    lm.sourceMesh = "Content/Meshes/Wall.ocmesh";
    lm.sourceHash = 0xDEADBEEFCAFEF00Dull;
    for (u32 v = 0; v < lm.vertexCount; ++v) {
        lm.uv.push_back(static_cast<f32>(v) * 0.125f);
        lm.uv.push_back(1.0f - static_cast<f32>(v) * 0.0625f);
    }
    // A deliberate spread: black, sub-unit, unit, and well into HDR. The last is the one that
    // matters and the one an LDR encoding would silently ruin.
    const f32 values[] = {0.0f, 0.25f, 1.0f, 12.5f, 300.0f, 0.03125f,
                          7.75f, 64.0f, 0.5f, 2.0f, 128.0f, 1000.0f};
    for (u32 i = 0; i < lm.texelCount(); ++i) {
        fmt::LightmapTexel t;
        t.r = values[i % 12];
        t.g = values[(i + 4) % 12];
        t.b = values[(i + 8) % 12];
        lm.texels.push_back(t);
    }
    return lm;
}

std::string tempPath(const char* name) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "aver_oclightmap_test";
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

} // namespace

int main() {
    AVER_INFO("[oclightmap-test] .oclightmap container + RGB9E5 texels");

    const fmt::OcLightmap src = makeLightmap();
    check(src.valid(), "the fixture is internally consistent (valid())");

    // ---- round trip ----------------------------------------------------------------------------
    const std::string path = tempPath("wall.oclightmap");
    std::string err;
    check(fmt::writeOcLightmap(path, src, &err), "writeOcLightmap succeeds: " + err);

    fmt::OcLightmap back;
    err.clear();
    check(fmt::readOcLightmap(path, back, &err), "readOcLightmap succeeds: " + err);

    check(back.width == src.width && back.height == src.height,
          "width/height survive the round trip");
    check(back.vertexCount == src.vertexCount, "vertexCount survives the round trip");
    check(back.sourceMesh == src.sourceMesh, "sourceMesh survives the round trip");
    check(back.sourceHash == src.sourceHash, "sourceHash survives the round trip bit-exactly");

    bool uvExact = back.uv.size() == src.uv.size();
    for (usize i = 0; uvExact && i < src.uv.size(); ++i) uvExact = back.uv[i] == src.uv[i];
    check(uvExact, "every lightmap UV survives BIT-EXACTLY (they are stored as raw f32, not packed)");

    // ---- the HDR claim, which is the whole reason for the encoding ------------------------------
    // PER TEXEL, AGAINST THAT TEXEL'S OWN BRIGHTEST CHANNEL -- not against the channel's own value.
    // This is the shared-exponent trade the header argues for, made explicit: all three channels
    // quantise to a step chosen by the brightest of them, so a texel holding (0.03125, ..., 1000)
    // cannot represent its dim channel to 1% OF ITSELF and was never going to. The first version of
    // this check compared each channel to its own magnitude and failed exactly there -- the format
    // was right and the assertion was wrong. Judging each channel against the texel's own scale is
    // the accuracy RGB9E5 actually promises, and still fails loudly if the encoding is broken.
    bool hdrOk = back.texels.size() == src.texels.size();
    std::string worst;
    for (usize i = 0; hdrOk && i < src.texels.size(); ++i) {
        const fmt::LightmapTexel& w = src.texels[i];
        const fmt::LightmapTexel& g = back.texels[i];
        const f32 peak = (w.r > w.g ? (w.r > w.b ? w.r : w.b) : (w.g > w.b ? w.g : w.b));
        const f32 tol = (peak > 1.0f ? peak : 1.0f) * 0.005f;   // half a percent of the texel's peak
        if (std::fabs(g.r - w.r) > tol || std::fabs(g.g - w.g) > tol || std::fabs(g.b - w.b) > tol) {
            hdrOk = false;
            worst = "texel " + std::to_string(i) + " wanted (" + std::to_string(w.r) + ", " +
                    std::to_string(w.g) + ", " + std::to_string(w.b) + ") got (" +
                    std::to_string(g.r) + ", " + std::to_string(g.g) + ", " +
                    std::to_string(g.b) + ") tol " + std::to_string(tol);
        }
    }
    check(hdrOk, "every texel round-trips to within half a percent of its own peak channel, "
                 "INCLUDING values far above 1.0" + (worst.empty() ? std::string() : " -- " + worst));

    // Named separately from the loop above, because "an HDR value survived" is the single claim an
    // LDR encoding would fail, and burying it in an aggregate would let it fail quietly.
    {
        fmt::OcLightmap hdr;
        hdr.width = hdr.height = 1;
        hdr.vertexCount = 1;
        hdr.sourceMesh = "m.ocmesh";
        hdr.uv = {0.0f, 0.0f};
        hdr.texels.push_back({900.0f, 450.0f, 12.0f});
        const std::string p = tempPath("hdr.oclightmap");
        fmt::OcLightmap r;
        check(fmt::writeOcLightmap(p, hdr, nullptr) && fmt::readOcLightmap(p, r, nullptr) &&
                  near(r.texels[0].r, 900.0f, 0.02f),
              "a 900.0 texel is still ~900 after a round trip, not clamped to 1");
    }

    // ---- negatives cannot survive, by construction ----------------------------------------------
    {
        fmt::OcLightmap neg;
        neg.width = neg.height = 1;
        neg.vertexCount = 1;
        neg.sourceMesh = "m.ocmesh";
        neg.uv = {0.0f, 0.0f};
        neg.texels.push_back({-5.0f, 2.0f, -0.001f});
        const std::string p = tempPath("neg.oclightmap");
        fmt::OcLightmap r;
        const bool ok = fmt::writeOcLightmap(p, neg, nullptr) && fmt::readOcLightmap(p, r, nullptr);
        check(ok && r.texels[0].r == 0.0f && r.texels[0].b == 0.0f,
              "a negative texel reads back as 0, NOT as a bright value -- the encoding has no sign "
              "bit and clamps rather than reinterpreting");
        check(ok && near(r.texels[0].g, 2.0f),
              "and the positive channel beside it is unharmed");
    }

    // ---- refusals ------------------------------------------------------------------------------
    {
        fmt::OcLightmap bad = makeLightmap();
        bad.uv.pop_back();   // now inconsistent with vertexCount
        check(!fmt::writeOcLightmap(tempPath("bad.oclightmap"), bad, nullptr),
              "writing a lightmap whose uv count disagrees with vertexCount is REFUSED");
    }
    {
        fmt::OcLightmap noMesh = makeLightmap();
        noMesh.sourceMesh.clear();
        check(!fmt::writeOcLightmap(tempPath("nomesh.oclightmap"), noMesh, nullptr),
              "writing a lightmap with no sourceMesh is REFUSED (nothing could detect staleness)");
    }

    // ---- a truncated file must be rejected, not read as garbage ----------------------------------
    {
        std::vector<u8> bytes;
        check(readFileBytes(path, bytes) && bytes.size() > 64,
              "the written file can be read back as bytes");
        const std::string cut = tempPath("truncated.oclightmap");
        // Half the file: past the header, into the payload, which is exactly the shape a crash
        // mid-write leaves behind.
        if (FILE* f = std::fopen(cut.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size() / 2, f);
            std::fclose(f);
        }
        fmt::OcLightmap r;
        std::string why;
        check(!fmt::readOcLightmap(cut, r, &why),
              "a TRUNCATED file is rejected rather than read as garbage");
        check(!why.empty(), "and the rejection says why: " + why);
    }

    // ---- a hostile header must be REFUSED, not allocated against ---------------------------------
    //
    // WHAT THIS DOES AND DOES NOT PROVE, stated because the first version of this comment claimed
    // more than the test earns. It asserts a clean `false` + message for an absurd header. It does
    // NOT demonstrate an integer overflow: running it with OcLightmap.cpp's size ceiling disabled
    // still PASSES, because on a 64-bit build width*height (both u32) cannot overflow a usize, and
    // the *4 wraps only to another enormous value that no real chunk size matches -- so the ordinary
    // size check already rejects it. The ceiling is defence in depth for a 32-bit build and for
    // absurd-but-representable sizes; this case guards the refusal, not a crash.
    //
    // BUILT BY PATCHING A REAL FILE, not by hand-assembling a container: the AVR1 header carries a
    // CRC and per-chunk hashes, so a from-scratch forgery would be rejected by the container layer
    // and prove nothing about this reader's own arithmetic. Taking a valid file and rewriting four
    // bytes of the payload is what puts the hostile numbers past the container and in front of the
    // code under test.
    {
        std::vector<u8> bytes;
        const std::string hostile = tempPath("hostile.oclightmap");
        if (readFileBytes(path, bytes)) {
            // The header chunk's first eight bytes are width then height, little-endian. Find them
            // by their known values (4 and 3 from the fixture) rather than by a hard-coded offset,
            // which would silently stop testing anything if the container layout ever shifted.
            bool patched = false;
            for (usize i = 0; i + 8 <= bytes.size(); ++i) {
                const u32 w = u32(bytes[i]) | (u32(bytes[i+1])<<8) | (u32(bytes[i+2])<<16) | (u32(bytes[i+3])<<24);
                const u32 h = u32(bytes[i+4]) | (u32(bytes[i+5])<<8) | (u32(bytes[i+6])<<16) | (u32(bytes[i+7])<<24);
                if (w == 4u && h == 3u) {
                    for (int k = 0; k < 8; ++k) bytes[i + usize(k)] = 0xFFu;   // 4294967295 x 4294967295
                    patched = true;
                    break;
                }
            }
            check(patched, "the fixture's width/height could be located for patching");
            if (FILE* f = std::fopen(hostile.c_str(), "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
            }
            fmt::OcLightmap r;
            std::string why;
            // No try/catch: if this throws, the test process dies and the suite reports it, which is
            // the correct outcome for a regression here -- a swallowed exception would let the very
            // failure this guards against pass quietly.
            check(!fmt::readOcLightmap(hostile, r, &why),
                  "a header claiming a 4294967295x4294967295 atlas is refused cleanly");
            check(!why.empty(), "and it says why rather than throwing: " + why);
        }
    }

    // ---- wrong magic ----------------------------------------------------------------------------
    {
        const std::string junk = tempPath("junk.oclightmap");
        if (FILE* f = std::fopen(junk.c_str(), "wb")) {
            const char nonsense[] = "NOT AN AVR1 CONTAINER AT ALL, JUST SOME BYTES ON DISK ........";
            std::fwrite(nonsense, 1, sizeof(nonsense), f);
            std::fclose(f);
        }
        fmt::OcLightmap r;
        check(!fmt::readOcLightmap(junk, r, nullptr), "a file with the wrong magic is rejected");
    }

    // ---- a missing file is a failure, not an empty success ---------------------------------------
    {
        fmt::OcLightmap r;
        check(!fmt::readOcLightmap(tempPath("does_not_exist.oclightmap"), r, nullptr),
              "a missing file is rejected");
    }

    // ---- the size helper agrees with the struct --------------------------------------------------
    check(fmt::ocLightmapBytes(src) == usize(src.vertexCount) * 2 * sizeof(f32) +
                                       usize(src.width) * usize(src.height) * 4,
          "ocLightmapBytes is uv bytes + 4 bytes per texel");

    // ---- vertexCount mismatch is DETECTABLE by a caller ------------------------------------------
    // The format deliberately does not check this itself (it has no mesh to check against); what it
    // owes the caller is a number that survives intact so the caller CAN.
    check(back.vertexCount == src.vertexCount && back.uv.size() == usize(back.vertexCount) * 2,
          "a caller can compare vertexCount against a live mesh and trust uv's size to match it");

    AVER_INFO("[oclightmap-test] RESULT: {} ({} checks, {} failures)",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
