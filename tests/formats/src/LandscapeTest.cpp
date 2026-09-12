// `.ocland`, the landscape heightfield: round trip, corruption, forward compatibility and the
// file-based save/load path. CPU only, no device. Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcLand.hpp"

#include <algorithm>
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


    AVER_INFO("=== ITEM 0.7: an old file (no LRNG chunk) migrates its range exactly once ===");
    {
        // Simulate a file saved before the authored-range fix existed. A struct that has never been
        // loaded still derives its range fresh from the live heights on its FIRST save -- exactly
        // what the pre-fix writer always did -- so writeOcLand's own output here is what an old
        // writer would have produced, except that it ALSO now carries the new LRNG chunk. Stripping
        // that one chunk back out leaves exactly an old-style file.
        std::string why;
        const fmt::OcLandData src = makeGrid(9);
        std::vector<u8> freshWrite;
        check(fmt::writeOcLand(src, freshWrite, &why), "a baseline write (" + why + ")");

        fmt::Avr1File container;
        check(fmt::parseAvr1(freshWrite.data(), freshWrite.size(), container, &why),
              "reopened as a raw container (" + why + ")");
        const usize chunksBefore = container.chunks.size();
        container.chunks.erase(
            std::remove_if(container.chunks.begin(), container.chunks.end(),
                            [](const fmt::AvrChunk& c) { return c.id == fmt::kOcLandChunkRange; }),
            container.chunks.end());
        check(container.chunks.size() == chunksBefore - 1,
              "the LRNG chunk was present in a fresh write and is now removed");
        std::vector<u8> oldStyle;
        check(fmt::writeAvr1(container, oldStyle, &why), "rewritten without it (" + why + ")");

        fmt::OcLandData loaded1;
        check(fmt::parseOcLand(oldStyle.data(), oldStyle.size(), loaded1, &why),
              "the old-style file still loads (" + why + ")");
        check(loaded1.hasQuantRange,
              "loading it PINS an authored range even though the file itself carried none");

        std::vector<u8> migrated;
        check(fmt::writeOcLand(loaded1, migrated, &why), "the migration save (" + why + ")");
        check(migrated != oldStyle,
              "the migration save DOES change the file once -- it gains the LRNG chunk the old file lacked");
        check(migrated.size() > oldStyle.size(), "specifically, it grows by the new chunk");

        fmt::OcLandData loaded2;
        check(fmt::parseOcLand(migrated.data(), migrated.size(), loaded2, &why),
              "the migrated file loads (" + why + ")");
        std::vector<u8> again;
        check(fmt::writeOcLand(loaded2, again, &why), "saved again with no further edits (" + why + ")");
        check(again == migrated,
              "and from the migration onward, every further save is byte-identical -- exactly once, then stable");
    }

    AVER_INFO("=== ITEM 0.7: a new file saved repeatedly is byte-identical every time ===");
    {
        std::string why;
        const fmt::OcLandData src = makeGrid(9);
        std::vector<u8> first, second;
        check(fmt::writeOcLand(src, first, &why), "first save of a never-loaded struct (" + why + ")");
        check(fmt::writeOcLand(src, second, &why), "second save of the SAME struct, no edits (" + why + ")");
        check(first == second, "byte-identical with no intervening load");

        fmt::OcLandData loaded;
        check(fmt::parseOcLand(first.data(), first.size(), loaded, &why), "loads back (" + why + ")");
        std::vector<u8> third;
        check(fmt::writeOcLand(loaded, third, &why), "and a save after a load with no edits (" + why + ")");
        check(third == first,
              "is ALSO byte-identical -- pinning the range on load introduces no drift of its own");
    }

    AVER_INFO("=== ITEM 0.7 (c1): an edit WITHIN the pinned range never drifts an untouched sample ===");
    {
        // THE FIRST BUG, still fixed: an edit that stays inside the already-pinned range must not
        // move the range at all, so every untouched sample's quantised code -- and therefore its
        // decoded height -- is IDENTICAL between saves. No growth, no headroom, no quantisation-step
        // bound needed: this case admits no ambiguity.
        std::string why;
        const fmt::OcLandData src = makeGrid(9);   // height(ix,iy) = iy*1000 + ix: corner (0,0) is the
                                                    // section's current MINIMUM (0), corner (8,8) its
                                                    // max (8008)
        std::vector<u8> baseline;
        check(fmt::writeOcLand(src, baseline, &why),
              "the section's existing, already-saved state (" + why + ")");

        fmt::OcLandData editing;
        check(fmt::parseOcLand(baseline.data(), baseline.size(), editing, &why),
              "loaded into the editor (" + why + ")");
        check(editing.hasQuantRange, "and carries a pinned range");

        const u32 ux = 4, uy = 4;   // the grid's centre: never touched below
        std::vector<u8> save1;
        check(fmt::writeOcLand(editing, save1, &why), "save #1, before any edit (" + why + ")");
        fmt::OcLandData afterSave1;
        check(fmt::parseOcLand(save1.data(), save1.size(), afterSave1, &why),
              "reads save #1 back (" + why + ")");

        // THE EDIT: raise corner (0,0) to the MIDPOINT of the pinned range -- comfortably inside it,
        // nowhere near either end.
        const f32 mid = 0.5f * (editing.quantMinCm + editing.quantMaxCm);
        editing.heights[0] = mid;

        std::vector<u8> save2;
        check(fmt::writeOcLand(editing, save2, &why),
              "save #2, after an edit WITHIN the pinned range (" + why + ")");
        fmt::OcLandData afterSave2;
        check(fmt::parseOcLand(save2.data(), save2.size(), afterSave2, &why),
              "reads save #2 back (" + why + ")");

        check(afterSave1.heightAt(ux, uy) == afterSave2.heightAt(ux, uy),
              "the untouched sample decodes to the EXACT SAME value after both saves (" +
              std::to_string(afterSave1.heightAt(ux, uy)) + " vs " +
              std::to_string(afterSave2.heightAt(ux, uy)) +
              ") -- an in-range edit does not move the range");

        check(afterSave2.quantMinCm == editing.quantMinCm && afterSave2.quantMaxCm == editing.quantMaxCm,
              "and the pinned range itself did not move");

        const f32 step = (editing.quantMaxCm - editing.quantMinCm) / 65535.0f;
        check(std::fabs(afterSave2.heightAt(0, 0) - mid) <= step,
              "the edited corner lands within one quantisation step of the authored midpoint");
    }

    AVER_INFO("=== ITEM 0.7 (c2): an edit ABOVE the pinned range grows it instead of clipping the edit ===");
    {
        // THE SECOND BUG: writeOcLand used to reuse the pinned range UNCONDITIONALLY and clamp
        // anything outside it. Before ITEM 0.7's first fix, lo/hi always covered the live heights, so
        // that clamp never fired; after it, sculpting past the pinned ceiling was silently flattened
        // on save -- and landscape sculpting is the one authoring mode that ships. The fix GROWS the
        // range (with headroom) to cover the new extreme instead of clamping, so this case checks all
        // three parts of that contract:
        //   1. the edit SURVIVES -- decoded near the authored value, not clamped to the old ceiling;
        //   2. the pinned range actually grew;
        //   3. an untouched sample may drift from the regrow, but the drift is BOUNDED by one
        //      quantisation step of the resulting (grown) scale -- asserted numerically, not as "no
        //      change", because a real regrow does perturb every sample's step size once.
        std::string why;
        const fmt::OcLandData src = makeGrid(9);
        std::vector<u8> baseline;
        check(fmt::writeOcLand(src, baseline, &why),
              "the section's existing, already-saved state (" + why + ")");

        fmt::OcLandData editing;
        check(fmt::parseOcLand(baseline.data(), baseline.size(), editing, &why),
              "loaded into the editor (" + why + ")");
        check(editing.hasQuantRange, "and carries a pinned range");
        const f32 oldMax = editing.quantMaxCm;

        const u32 ux = 4, uy = 4;   // untouched throughout
        std::vector<u8> save1;
        check(fmt::writeOcLand(editing, save1, &why), "save #1, before any edit (" + why + ")");
        fmt::OcLandData afterSave1;
        check(fmt::parseOcLand(save1.data(), save1.size(), afterSave1, &why),
              "reads save #1 back (" + why + ")");
        const f32 untouchedBefore = afterSave1.heightAt(ux, uy);

        // THE EDIT: sculpt corner (8,8) -- already the section's max at 8008 -- further up, past the
        // pinned ceiling.
        const f32 authored = oldMax + 500.0f;
        editing.heights[static_cast<usize>(8) * 9 + 8] = authored;

        std::vector<u8> save2;
        check(fmt::writeOcLand(editing, save2, &why), "save #2, after the above-ceiling edit (" + why + ")");
        fmt::OcLandData afterSave2;
        check(fmt::parseOcLand(save2.data(), save2.size(), afterSave2, &why),
              "reads save #2 back (" + why + ")");

        check(afterSave2.quantMaxCm > oldMax,
              "the pinned range GREW to cover the new height (was " + std::to_string(oldMax) +
              ", now " + std::to_string(afterSave2.quantMaxCm) + ")");

        const f32 newStep = (afterSave2.quantMaxCm - afterSave2.quantMinCm) / 65535.0f;
        check(std::fabs(afterSave2.heightAt(8, 8) - authored) <= newStep,
              "the edit SURVIVES -- decoded within one quantisation step of the authored value, not "
              "clamped to the old ceiling (decoded " + std::to_string(afterSave2.heightAt(8, 8)) +
              " vs authored " + std::to_string(authored) + ")");

        const f32 untouchedAfter = afterSave2.heightAt(ux, uy);
        check(std::fabs(untouchedAfter - untouchedBefore) <= newStep,
              "an untouched sample may drift from the regrow, but by at most one quantisation step of "
              "the new scale (" + std::to_string(untouchedBefore) + " vs " +
              std::to_string(untouchedAfter) + ", step " + std::to_string(newStep) + ")");
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
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
