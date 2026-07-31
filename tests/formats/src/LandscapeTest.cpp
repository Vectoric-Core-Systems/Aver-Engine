// `.ocland`, the landscape heightfield: round trip, corruption, forward compatibility and the
// file-based save/load path. CPU only, no device. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/formats/OcLand.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

// Logs one assertion and counts the checks and the failures.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Builds an n x n grid where height(ix, iy) = iy*1000 + ix, so a transposed read is visible.
static fmt::OcLandData makeGrid(u32 n, f32 spacing = 100.0f) {
    fmt::OcLandData d;
    d.sampleCount = n;
    d.spacingCm = spacing;
    d.originCm[0] = 500.0f;
    d.originCm[1] = -250.0f;
    d.originCm[2] = 0.0f;
    d.heights.resize(static_cast<usize>(n) * n);
    for (u32 iy = 0; iy < n; ++iy)
        for (u32 ix = 0; ix < n; ++ix)
            d.heights[static_cast<usize>(iy) * n + ix] =
                static_cast<f32>(iy) * 1000.0f + static_cast<f32>(ix);
    return d;
}

// Runs every landscape format test. Returns the failure count.
int main() {
    AVER_INFO("=== .ocland round trip ===");
    {
        const u32 N = 9;
        const fmt::OcLandData src = makeGrid(N);

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcLand(src, bytes, &why), "writes (" + why + ")");
        check(bytes.size() > 64, "produced a container of " + std::to_string(bytes.size()) + " bytes");

        fmt::OcLandData back;
        check(fmt::parseOcLand(bytes.data(), bytes.size(), back, &why), "parses back (" + why + ")");
        check(back.valid(), "the result is internally consistent");
        check(back.sampleCount == N, "sampleCount survives");
        check(back.spacingCm == src.spacingCm, "spacing survives");
        check(back.originCm[0] == src.originCm[0] && back.originCm[1] == src.originCm[1],
              "origin survives");

        // Heights are u16 across the grid's own relief, so one step here is 8008/65535 cm.
        const f32 step = 8008.0f / 65535.0f;
        f32 worst = 0.0f;
        for (u32 iy = 0; iy < N; ++iy) {
            for (u32 ix = 0; ix < N; ++ix) {
                const f32 want = static_cast<f32>(iy) * 1000.0f + static_cast<f32>(ix);
                worst = std::fmax(worst, std::fabs(back.heightAt(ix, iy) - want));
            }
        }
        check(worst <= step,
              "every height is within one quantisation step (worst " + std::to_string(worst) +
              " cm, step " + std::to_string(step) + ")");

        check(std::fabs(back.heightAt(1, 7) - 7001.0f) <= step,
              "sample (ix=1, iy=7) is 7001, not 1007 -- rows and columns are not transposed");

        check(std::fabs(back.boundsMin[2] - 0.0f) <= step, "bounds min z recomputed from the heights");
        check(std::fabs(back.boundsMax[2] - 8008.0f) <= step, "bounds max z recomputed");
        check(std::fabs(back.boundsMax[0] - (src.originCm[0] + 800.0f)) < 0.01f,
              "footprint is (n-1) spacings wide, not n");

        f32 w[3];
        back.worldAt(2, 3, w);
        check(std::fabs(w[0] - (src.originCm[0] + 200.0f)) < 0.01f, "column ix runs along +X");
        check(std::fabs(w[1] - (src.originCm[1] + 300.0f)) < 0.01f, "row iy runs along +Y");
        check(std::fabs(w[2] - 3002.0f) <= step, "height is the +Z component");
    }

    AVER_INFO("=== degenerate and hostile input ===");
    {
        std::string why;
        fmt::OcLandData out;

    // Reports one refusal, reading `why` only after the call that filled it.
    auto refused = [&](bool didRefuse, const std::string& what) {
        check(didRefuse, what + " (" + why + ")");
    };


        fmt::OcLandData flat = makeGrid(4);
        for (f32& h : flat.heights) h = 1234.5f;
        std::vector<u8> flatBytes;
        check(fmt::writeOcLand(flat, flatBytes, &why), "a perfectly flat section writes (" + why + ")");
        check(fmt::parseOcLand(flatBytes.data(), flatBytes.size(), out, &why),
              "and parses (" + why + ")");
        bool allFlat = true;
        for (f32 h : out.heights) if (std::fabs(h - 1234.5f) > 0.01f) allFlat = false;
        check(allFlat, "every sample of a flat section decodes exactly (no divide by a zero span)");

        std::vector<u8> good;
        check(fmt::writeOcLand(makeGrid(8), good, &why), "a reference file for the corruption cases");

        std::vector<u8> badMagic = good;
        badMagic[0] ^= 0xFF;
        why.clear();
        { const bool r = !fmt::parseOcLand(badMagic.data(), badMagic.size(), out, &why);
          refused(r, "a flipped magic byte is refused"); }

        std::vector<u8> badHeader = good;
        badHeader[0x0A] ^= 0x01;
        why.clear();
        { const bool r = !fmt::parseOcLand(badHeader.data(), badHeader.size(), out, &why);
          refused(r, "a flipped header byte is caught by the header CRC"); }

        std::vector<u8> badPayload = good;
        badPayload[badPayload.size() - 1] ^= 0x01;
        why.clear();
        { const bool r = !fmt::parseOcLand(badPayload.data(), badPayload.size(), out, &why);
          refused(r, "a flipped last payload byte is caught by the chunk hash"); }

        std::vector<u8> truncated(good.begin(), good.end() - 16);
        why.clear();
        { const bool r = !fmt::parseOcLand(truncated.data(), truncated.size(), out, &why);
          refused(r, "a truncated file is refused"); }

        why.clear();
        { const bool r = !fmt::parseOcLand(good.data(), 4, out, &why);
          refused(r, "a file shorter than the header is refused"); }
        check(!fmt::parseOcLand(nullptr, 0, out, &why), "null input is refused rather than read");

        fmt::OcLandData tiny;
        tiny.sampleCount = 1;
        tiny.spacingCm = 100.0f;
        tiny.heights.assign(1, 0.0f);
        std::vector<u8> tinyBytes;
        why.clear();
        { const bool r = !fmt::writeOcLand(tiny, tinyBytes, &why);
          refused(r, "a 1x1 grid is refused -- one sample is a point, not a surface"); }

        fmt::OcLandData lying = makeGrid(4);
        lying.sampleCount = 5;
        std::vector<u8> lyingBytes;
        why.clear();
        { const bool r = !fmt::writeOcLand(lying, lyingBytes, &why);
          refused(r, "a sampleCount that disagrees with the height count is refused"); }
    }

    AVER_INFO("=== forward compatibility ===");
    {
        std::string why;
        std::vector<u8> bytes;
        check(fmt::writeOcLand(makeGrid(8), bytes, &why), "a base file");

        fmt::Avr1File file;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), file, &why), "reopened as a container");
        std::vector<u8> future(64, 0xAB);
        file.add(fmt::avrFourCC("LMSK"), std::move(future), /*flags*/0);
        std::vector<u8> withFuture;
        check(fmt::writeAvr1(file, withFuture, &why), "rewritten with an unknown chunk added");

        fmt::OcLandData out;
        check(fmt::parseOcLand(withFuture.data(), withFuture.size(), out, &why),
              "an unknown non-Required chunk is skipped and the landscape still loads (" + why + ")");
        check(out.valid() && out.sampleCount == 8, "and the surface is intact");
    }


    AVER_INFO("=== through a real file ===");
    {
        std::string why;
        const fmt::OcLandData src = makeGrid(17);

        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "aver-ocland-test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = (dir / "section.ocland").string();

        check(fmt::saveOcLand(path, src, &why), "saves to a file (" + why + ")");
        check(std::filesystem::exists(path), "the file is on disk");
        const auto onDisk = std::filesystem::file_size(path, ec);
        check(onDisk > 64, "and has real content (" + std::to_string(onDisk) + " bytes)");

        fmt::OcLandData back;
        why.clear();
        const bool loaded = fmt::loadOcLand(path, back, &why);
        check(loaded, "loads back from the file (" + why + ")");
        check(back.sampleCount == src.sampleCount, "sampleCount survives the file");
        check(back.spacingCm == src.spacingCm, "spacing survives");
        check(back.originCm[0] == src.originCm[0], "origin survives");

        const f32 step = 16016.0f / 65535.0f;
        check(std::fabs(back.heightAt(1, 7) - 7001.0f) <= step,
              "and the file path does not transpose either");

        fmt::OcLandData missing;
        why.clear();
        const bool gone = fmt::loadOcLand((dir / "does-not-exist.ocland").string(), missing, &why);
        check(!gone, "a missing file is refused (" + why + ")");

        {
            std::vector<u8> bytes;
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                bytes.resize(static_cast<usize>(n));
                const usize got = std::fread(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
                check(got == bytes.size(), "the saved file reads back byte for byte");
            }
            const std::string bad = (dir / "corrupt.ocland").string();
            if (!bytes.empty()) {
                bytes[bytes.size() / 2] ^= 0xFF;
                std::FILE* g = std::fopen(bad.c_str(), "wb");
                if (g) { std::fwrite(bytes.data(), 1, bytes.size(), g); std::fclose(g); }
                fmt::OcLandData rotten;
                why.clear();
                const bool read = fmt::loadOcLand(bad, rotten, &why);
                check(!read, "a byte flipped on disk is refused (" + why + ")");
            }
        }
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
