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
#include "aver/formats/Avr1.hpp"
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
    // THE PREVIOUS VERSION OF THIS TEST NEVER REACHED THE CODE IT NAMED, and the comment above it
    // drew a conclusion from that. It forged the file by patching width/height inside an ALREADY
    // WRITTEN file, which leaves the AVR1 chunk hash describing the original bytes -- so parseAvr1
    // refused it with `AVR1: chunk payload hash mismatch (file corrupt)` (measured: that string was
    // what this very check printed) and readOcLightmap's arithmetic never ran at all. That is also
    // why "it still passes with the ceiling disabled" was both true and worthless as evidence:
    // nothing ever got as far as the ceiling to be affected by disabling it.
    //
    // HAND-ASSEMBLED THROUGH THE WRITER, which is what makes a file hostile AND well-formed:
    // writeAvr1 computes every chunk hash from whatever bytes it is handed (Avr1.cpp), so a forged
    // container carries self-consistent hashes and a valid CRC. The old comment asserted the
    // opposite -- that a from-scratch forgery "would be rejected by the container layer" -- and that
    // false premise is precisely what pushed this test into the patching approach that defeated it.
    //
    // WHY 2^31 SQUARED IS THE INTERESTING SIZE and 0xFFFFFFFF is not: `usize(w) * usize(h) * 4` at
    // w = h = 2^31 is 2^62 * 4 == 2^64 exactly, which wraps a 64-bit usize to ZERO -- not to
    // "another enormous value no real chunk size matches", as the old comment claimed. A zero-byte
    // texel chunk MATCHES that wrapped size, so control reaches `texels.resize(2^62)`, which throws
    // std::length_error out of a function whose header documents a false return. The ceiling in
    // OcLightmap.cpp is therefore load-bearing on 64-bit too, not merely cover for 32-bit builds.
    //
    // The assertions check WHICH refusal fired, not merely that one did. An assertion demanding only
    // `false` is satisfied by the container hash error -- which is exactly how the previous version
    // passed while testing nothing.
    {
        auto put32 = [](std::vector<u8>& v, u32 x) {
            v.push_back(u8(x)); v.push_back(u8(x >> 8));
            v.push_back(u8(x >> 16)); v.push_back(u8(x >> 24));
        };
        auto put64 = [](std::vector<u8>& v, u64 x) {
            for (int i = 0; i < 8; ++i) v.push_back(u8(x >> (i * 8)));
        };
        // vertexCount 1 with a matching 8-byte uv chunk keeps everything except the atlas size
        // honest, so a uv mismatch cannot be the thing doing the refusing.
        auto buildHostile = [&](const std::string& out, u32 w, u32 h, usize texBytes) {
            std::vector<u8> head;
            put32(head, w);
            put32(head, h);
            put32(head, 1u);           // vertexCount
            put64(head, 0u);           // sourceHash
            put32(head, 1u);           // sourceMesh length
            head.push_back(u8('m'));   // sourceMesh itself
            std::vector<u8> uv(1u * 2u * sizeof(f32), 0u);
            std::vector<u8> tex(texBytes, 0u);
            fmt::Avr1File f;
            f.subtype = fmt::avrFourCC("LMAP");
            f.add(fmt::avrFourCC("LMHD"), std::move(head), fmt::kAvrChunkRequired);
            f.add(fmt::avrFourCC("LMUV"), std::move(uv),   fmt::kAvrChunkRequired);
            f.add(fmt::avrFourCC("LMTX"), std::move(tex),  fmt::kAvrChunkRequired);
            return fmt::saveAvr1(out, f, nullptr);
        };

        // Proving the container ACCEPTS the forgery is what makes the refusals below attributable to
        // the reader's own arithmetic rather than to a malformed file.
        const std::string wrap = tempPath("hostile_wrap.oclightmap");
        check(buildHostile(wrap, 0x80000000u, 0x80000000u, 0),
              "a forged .oclightmap can be written carrying valid AVR1 hashes");

        fmt::OcLightmap r;
        std::string why;
        // No try/catch: with the ceiling removed this throws std::length_error and takes the process
        // with it, which is the correct and highly visible outcome for a regression here.
        check(!fmt::readOcLightmap(wrap, r, &why),
              "a 2147483648x2147483648 atlas, whose byte count wraps to exactly 0, is refused");
        check(why.find("edge limit") != std::string::npos,
              "and it is the EDGE CEILING that refuses it, not the container: " + why);

        const std::string huge = tempPath("hostile_max.oclightmap");
        fmt::OcLightmap r2;
        std::string why2;
        check(buildHostile(huge, 0xFFFFFFFFu, 0xFFFFFFFFu, 0), "and one at the u32 maximum");
        check(!fmt::readOcLightmap(huge, r2, &why2),
              "a 4294967295x4294967295 atlas is refused cleanly");
        check(why2.find("edge limit") != std::string::npos,
              "and it too names the edge ceiling: " + why2);
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
