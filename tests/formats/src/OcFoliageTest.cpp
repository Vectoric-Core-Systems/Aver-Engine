// `.ocfoliage`, one authored foliage type: byte-identical round trip, validity refusals, and the
// ABSENT-VS-ZERO rule OcFoliage.hpp documents -- a chunk shorter than this reader expects must leave
// every field it could not reach at OcFoliageData's own compiled-in default (randomizeYaw true,
// weight 1.0, ...), never at binary zero, and that has to stay visibly DIFFERENT from a full-length
// chunk that explicitly authored a zero/false value for the same field. Exit code = failure count.
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcFoliage.hpp"

#include "aver/core/Log.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static fmt::OcFoliageData makeFixture() {
    fmt::OcFoliageData d;
    d.meshPath = "Meshes/Foliage/Tree_Pine.ocmesh";
    d.material = "M_Tree";
    d.scaleMin = 0.8f;
    d.scaleMax = 1.35f;
    d.weight = 2.5f;
    d.randomizeYaw = true;
    d.collisionRadiusCm = 150.0f;
    d.alignToNormal = true;
    return d;
}

int main() {
    AVER_INFO("=== .ocfoliage round trip ===");
    {
        const fmt::OcFoliageData src = makeFixture();
        check(src.valid(), "the fixture is valid");

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcFoliage(src, bytes, &why), "writes (" + why + ")");
        check(bytes.size() > 64, "produced a container of " + std::to_string(bytes.size()) + " bytes");

        fmt::OcFoliageData back;
        check(fmt::parseOcFoliage(bytes.data(), bytes.size(), back, &why), "parses back (" + why + ")");
        check(back.meshPath == src.meshPath, "meshPath survives");
        check(back.material == src.material, "material survives");
        check(back.scaleMin == src.scaleMin && back.scaleMax == src.scaleMax, "scale range survives");
        check(back.weight == src.weight, "weight survives");
        check(back.randomizeYaw == src.randomizeYaw, "randomizeYaw survives");
        check(back.collisionRadiusCm == src.collisionRadiusCm, "collisionRadiusCm survives");
        check(back.alignToNormal == src.alignToNormal, "alignToNormal survives");

        // BYTE-IDENTICAL: re-writing what was just parsed back must reproduce the exact same bytes --
        // the round-trip contract this format actually promises (unlike .ocparticle's merge-into-
        // existing-text contract, this format has no author-facing text to preserve around the edges).
        std::vector<u8> bytes2;
        check(fmt::writeOcFoliage(back, bytes2, &why), "re-writes (" + why + ")");
        check(bytes == bytes2, "re-encoding the parsed result reproduces the identical bytes, size " +
                                    std::to_string(bytes.size()) + " vs " + std::to_string(bytes2.size()));
    }

    AVER_INFO("=== defaults for an empty/unset material ===");
    {
        fmt::OcFoliageData src = makeFixture();
        src.material.clear();
        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcFoliage(src, bytes, &why), "writes with no material (" + why + ")");
        fmt::OcFoliageData back;
        check(fmt::parseOcFoliage(bytes.data(), bytes.size(), back, &why), "parses back (" + why + ")");
        check(back.material.empty(), "material stays empty -- 'use the mesh's own material'");
    }

    AVER_INFO("=== valid() refuses what writeOcFoliage must refuse ===");
    {
        std::vector<u8> bytes;
        std::string why;

        fmt::OcFoliageData noMesh = makeFixture();
        noMesh.meshPath.clear();
        check(!noMesh.valid(), "no meshPath is invalid");
        check(!fmt::writeOcFoliage(noMesh, bytes, &why), "and writeOcFoliage refuses it");

        fmt::OcFoliageData badScale = makeFixture();
        badScale.scaleMin = 1.5f; badScale.scaleMax = 0.5f;   // max below min
        check(!badScale.valid(), "scaleMax below scaleMin is invalid");
        check(!fmt::writeOcFoliage(badScale, bytes, &why), "and writeOcFoliage refuses it");

        fmt::OcFoliageData zeroMin = makeFixture();
        zeroMin.scaleMin = 0.0f;
        check(!zeroMin.valid(), "a zero scaleMin is invalid -- ScatterSpecies' own scale range is a "
                                 "positive multiplier, never a degenerate mesh");
        check(!fmt::writeOcFoliage(zeroMin, bytes, &why), "and writeOcFoliage refuses it");

        fmt::OcFoliageData negRadius = makeFixture();
        negRadius.collisionRadiusCm = -1.0f;
        check(!negRadius.valid(), "a negative collisionRadiusCm is invalid");
        check(!fmt::writeOcFoliage(negRadius, bytes, &why), "and writeOcFoliage refuses it");
    }

    AVER_INFO("=== corruption: wrong subtype, missing FHDR, null/short input ===");
    {
        std::vector<u8> good;
        std::string why;
        check(fmt::writeOcFoliage(makeFixture(), good, &why), "a reference file for the corruption cases");

        fmt::OcFoliageData out;
        check(!fmt::parseOcFoliage(nullptr, 0, out, &why), "null input is refused rather than read");
        check(!fmt::parseOcFoliage(good.data(), 4, out, &why), "four bytes is refused rather than read");

        // Corrupt the subtype fourCC in the AVR1 header (offset documented by Avr1.hpp: magic then
        // subtype, both u32) and confirm the reader notices rather than misreading it as some other
        // format's container.
        std::vector<u8> badSubtype = good;
        if (badSubtype.size() >= 8) {
            badSubtype[4] ^= 0xFF; badSubtype[5] ^= 0xFF; badSubtype[6] ^= 0xFF; badSubtype[7] ^= 0xFF;
        }
        check(!fmt::parseOcFoliage(badSubtype.data(), badSubtype.size(), out, &why),
              "a mismatched subtype is refused (" + why + ")");

        // Round-trip through the real container API to drop the FHDR chunk entirely, rather than
        // guessing at byte offsets -- this exercises parseOcFoliage's own "no FHDR chunk" branch.
        fmt::Avr1File f;
        check(fmt::parseAvr1(good.data(), good.size(), f, &why), "the reference file parses as AVR1");
        std::vector<fmt::AvrChunk> keptOnlyStrings;
        for (const fmt::AvrChunk& c : f.chunks)
            if (c.id != fmt::kOcFoliageChunkHeader) keptOnlyStrings.push_back(c);
        f.chunks = std::move(keptOnlyStrings);
        std::vector<u8> noHeader;
        check(fmt::writeAvr1(f, noHeader, &why), "re-encodes without FHDR");
        check(!fmt::parseOcFoliage(noHeader.data(), noHeader.size(), out, &why),
              "a file with no FHDR chunk is refused (" + why + ")");
    }

    // THE ABSENT-VS-ZERO RULE, exercised directly: a chunk this reader expects but that ends early
    // (an old writer predating a later additive field, simulated here rather than waited for) leaves
    // every field it could not reach at OcFoliageData's OWN DEFAULT, never at the bit pattern zero --
    // and that default has to be visibly different from what a full-length chunk that explicitly
    // authored zero/false for the same field produces. See OcFoliage.hpp's own header comment.
    AVER_INFO("=== the absent-vs-zero rule: truncated FHDR defaults, it does not zero ===");
    {
        const fmt::OcFoliageData src = makeFixture();
        std::vector<u8> full;
        std::string why;
        check(fmt::writeOcFoliage(src, full, &why), "the reference file writes (" + why + ")");

        fmt::Avr1File f;
        check(fmt::parseAvr1(full.data(), full.size(), f, &why), "and parses as AVR1");

        auto truncateFhdrTo = [&](usize keepBytes) -> std::vector<u8> {
            fmt::Avr1File g = f;
            for (fmt::AvrChunk& c : g.chunks) {
                if (c.id == fmt::kOcFoliageChunkHeader) {
                    check(c.data.size() > keepBytes,
                          "FHDR is longer than the truncation point (" + std::to_string(c.data.size()) +
                              " > " + std::to_string(keepBytes) + ")");
                    c.data.resize(keepBytes);
                }
            }
            std::vector<u8> bytes;
            std::string why2;
            check(fmt::writeAvr1(g, bytes, &why2), "truncated container re-encodes (" + why2 + ")");
            return bytes;
        };

        // Only meshRef + matRef (2 x u32 = 8 bytes) survive -- everything past that point is absent.
        {
            const std::vector<u8> trunc = truncateFhdrTo(8);
            fmt::OcFoliageData out;
            check(fmt::parseOcFoliage(trunc.data(), trunc.size(), out, &why),
                  "a chunk with only mesh/material still parses (" + why + ")");
            check(out.meshPath == src.meshPath && out.material == src.material,
                  "the fields that WERE present survive");
            const fmt::OcFoliageData def;   // OcFoliageData's own compiled-in defaults
            check(out.scaleMin == def.scaleMin && out.scaleMax == def.scaleMax,
                  "absent scale range defaults to 0.75/1.25, not 0/0");
            check(out.weight == def.weight,
                  "absent weight defaults to 1.0, not 0 -- 0 would mean 'never picked'");
            check(out.randomizeYaw == def.randomizeYaw,
                  "absent randomizeYaw defaults to true, not false -- this is the exact case a blind "
                  "zero-fill gets backwards");
            check(out.collisionRadiusCm == def.collisionRadiusCm, "absent collisionRadiusCm defaults to 0");
            check(out.alignToNormal == def.alignToNormal, "absent alignToNormal defaults to false");
        }

        // meshRef + matRef + scaleMin + scaleMax (16 bytes) survive; weight/randomizeYaw/
        // collisionRadiusCm/alignToNormal do not.
        {
            const std::vector<u8> trunc = truncateFhdrTo(16);
            fmt::OcFoliageData out;
            check(fmt::parseOcFoliage(trunc.data(), trunc.size(), out, &why),
                  "a partially-truncated chunk still parses (" + why + ")");
            check(out.scaleMin == src.scaleMin && out.scaleMax == src.scaleMax,
                  "the scale range that WAS present survives");
            const fmt::OcFoliageData def;
            check(out.weight == def.weight, "weight past the truncation point still defaults to 1.0");
            check(out.randomizeYaw == def.randomizeYaw, "randomizeYaw past it still defaults to true");
        }

        // NOW THE OTHER HALF OF THE RULE: a FULL-LENGTH chunk that explicitly authored weight=0 and
        // randomizeYaw=false must come back as exactly that -- an author's real "never pick this" and
        // "never randomise yaw" must not collapse into the same bytes a truncated file produces.
        {
            fmt::OcFoliageData zeroed = src;
            zeroed.weight = 0.0f;          // ScatterSpecies' own documented meaning: never picked
            zeroed.randomizeYaw = false;
            std::vector<u8> bytes;
            check(fmt::writeOcFoliage(zeroed, bytes, &why), "an explicitly-zeroed record writes");
            fmt::OcFoliageData out;
            check(fmt::parseOcFoliage(bytes.data(), bytes.size(), out, &why), "and parses back");
            check(out.weight == 0.0f, "an AUTHORED zero weight reads back as exactly zero");
            check(!out.randomizeYaw, "an AUTHORED false randomizeYaw reads back as exactly false");
            check(bytes.size() == full.size(),
                  "a full-length record is the same size whether or not a field happens to be zero");
        }
    }

    AVER_INFO("=== file-based save/load round trip ===");
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-ocfoliage-test").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/Tree_Pine.ocfoliage";

        const fmt::OcFoliageData src = makeFixture();
        std::string why;
        check(fmt::saveOcFoliage(path, src, &why), "saves to disk (" + why + ")");

        fmt::OcFoliageData back;
        check(fmt::loadOcFoliage(path, back, &why), "loads back from disk (" + why + ")");
        check(back.meshPath == src.meshPath && back.material == src.material &&
                  back.scaleMin == src.scaleMin && back.scaleMax == src.scaleMax &&
                  back.weight == src.weight && back.randomizeYaw == src.randomizeYaw &&
                  back.collisionRadiusCm == src.collisionRadiusCm && back.alignToNormal == src.alignToNormal,
              "every field survives the disk round trip");

        check(!fmt::loadOcFoliage(dir + "/nope.ocfoliage", back, &why),
              "a missing file is refused rather than crashing (" + why + ")");

        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("OcFoliageTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
