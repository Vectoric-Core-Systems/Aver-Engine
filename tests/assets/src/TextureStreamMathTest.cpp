// TextureStreamMathTest -- the mip-window arithmetic and the .octex header-only reader (docs/plans/wave4/TEXTURE_STREAMING.md 2.2).
//
// The sizes are hand sums, not a second implementation of the same loop: a 4096 BC7 chain's window from mip 4 is
// 65536+16384+4096+1024+256+64+16+16+16 = 87408 bytes (the design's "about 87 KB"), its tail from mip 6 is 5488 bytes.
// The container half writes a real cache entry with saveCachedTexture and reads it back every way the streamer will:
// whole (loadCachedTexture), header-only (statCachedTexture), and by mip range (readCachedMips), byte for byte.
#include "aver/assets/TextureCache.hpp"
#include "aver/assets/TextureStreamMath.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::assets;

static int g_checks = 0;
static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    // ---- byte sizes ---------------------------------------------------------------------------------
    check(bc7MipBytes(4, 4) == 16 && bc7MipBytes(1, 1) == 16 && bc7MipBytes(2, 2) == 16, "a 4x4 block is 16 bytes and smaller levels still take one block");
    check(bc7MipBytes(256, 256) == 65536, "256x256 is 64x64 blocks of 16 bytes");
    check(bc7MipBytes(1500, 1500) == 2250000, "1500x1500 is 375x375 blocks");
    check(bc7MipBytes(6, 4) == 32, "an extent that is not a multiple of 4 rounds its block count up");
    check(mipExtent(4096, 12) == 1 && mipExtent(4096, 13) == 1 && mipExtent(5, 1) == 2, "mip extents halve and floor at 1");

    check(windowBytes(4096, 4096, 4, 13) == 87408, "4096 window from mip 4 is 87408 bytes");
    check(windowBytes(4096, 4096, 6, 13) == 5488, "4096 tail from mip 6 is 5488 bytes");
    check(windowBytes(4096, 4096, 0, 13) == 22369648, "the full 4096 chain is 22369648 bytes");
    check(windowBytes(4096, 4096, 13, 13) == 0, "an empty window is zero bytes");
    check(windowBytes(256, 256, 0, 9) == 65536 + 16384 + 4096 + 1024 + 256 + 64 + 16 + 16 + 16, "the full 256 chain");
    check(windowBytes(1500, 1500, 0, 11) == 2250000 + 188 * 188 * 16 + 94 * 94 * 16 + 47 * 47 * 16 +
                                                24 * 24 * 16 + 12 * 12 * 16 + 6 * 6 * 16 + 3 * 3 * 16 + 4 * 16 + 16 + 16,
          "a 1500 chain adds each level's own rounded block count");

    // ---- which mips may be the top ------------------------------------------------------------------
    check(allowedTopMask(4096, 4096, 13) == 0x7FFu, "4096: mips 0..10 (4096..4) may be the top, 2x2 and 1x1 may not");
    check(allowedTopMask(256, 256, 9) == 0x7Fu, "256: mips 0..6 (256..4)");
    check(allowedTopMask(1500, 1500, 11) == 1u, "1500: no legal top above mip 0");
    check(allowedTopMask(1024, 512, 11) == 0xFFu, "1024x512: the shorter side reaches 4 at mip 7");
    check(allowedTopMask(512, 512, 1) == 1u, "a single-level chain has only mip 0");
    check(snapToAllowed(5, 0x7Fu, 9) == 5 && snapToAllowed(8, 0x7Fu, 9) == 6, "snap keeps a legal mip and rounds an illegal one toward more detail");
    check(snapToAllowed(5, 1u, 11) == 0, "snap on a 1500 chain always lands on mip 0");
    check(snapToAllowed(99, 0x7Fu, 9) == 6, "snap clamps a mip past the chain");
    check(firstMipWithin(4096, 4096, 13, 0x7FFu, 64) == 6, "tail mip for 64: 4096 >> 6");
    check(firstMipWithin(4096, 4096, 13, 0x7FFu, 256) == 4, "initial mip for 256: 4096 >> 4");
    check(firstMipWithin(1500, 1500, 11, 1u, 64) == 0, "a 1500 chain cannot trim, so its tail is mip 0");
    check(firstMipWithin(32, 32, 6, 0x7u, 64) == 0, "a chain already inside the limit starts at mip 0");

    // ---- footprint to mip ---------------------------------------------------------------------------
    const u32 m4096 = 0x7FFu;
    check(wantedFirstMip(1.0f / 4096.0f, 4096, 4096, 13, 0, m4096) == 0, "one texel per pixel wants mip 0");
    check(wantedFirstMip(8.0f / 4096.0f, 4096, 4096, 13, 0, m4096) == 3, "8 texels per pixel wants mip 3");
    check(wantedFirstMip(7.9f / 4096.0f, 4096, 4096, 13, 0, m4096) == 2, "7.9 texels per pixel floors to mip 2");
    check(wantedFirstMip(8.0f / 4096.0f, 4096, 4096, 13, 1, m4096) == 4, "bias +1 is one mip coarser");
    check(wantedFirstMip(8.0f / 4096.0f, 4096, 4096, 13, -1, m4096) == 2, "bias -1 is one mip sharper");
    check(wantedFirstMip(0.25f / 4096.0f, 4096, 4096, 13, 0, m4096) == 0, "magnification clamps to mip 0");
    check(wantedFirstMip(0.25f / 4096.0f, 4096, 4096, 13, -3, m4096) == 0, "a sharper bias still clamps to mip 0");
    check(wantedFirstMip(100.0f, 4096, 4096, 13, 0, m4096) == 10, "a huge footprint clamps to the last legal top");
    check(wantedFirstMip(0.0f, 4096, 4096, 13, 0, m4096) == 10, "no footprint wants the least detail");
    check(wantedFirstMip(-1.0f, 4096, 4096, 13, 0, m4096) == 10, "a negative footprint wants the least detail");
    check(wantedFirstMip(8.0f / 2048.0f, 2048, 1024, 12, 0, 0x1FFu) == 3, "the larger extent sets the scale on a non-square texture");
    check(wantedFirstMip(8.0f / 1500.0f, 1500, 1500, 11, 0, 1u) == 0, "a 1500 chain snaps every want to mip 0");
    check(wantedFirstMip(1.0f, 4096, 4096, 0, 0, 1u) == 0, "an empty chain wants mip 0");

    // ---- the streamable predicate -------------------------------------------------------------------
    check(textureStreamable(true, 4096, 4096, 13), "a 4096 BC7 chain streams");
    check(textureStreamable(true, 512, 512, 10), "512 is the smallest streamable size");
    check(!textureStreamable(true, 256, 256, 9), "under 512 stays resident");
    check(!textureStreamable(false, 4096, 4096, 13), "non-BC7 stays resident");
    check(!textureStreamable(true, 1500, 1500, 11), "1500 has no legal top above mip 0, so it is pinned");
    check(!textureStreamable(true, 4096, 4096, 1), "a one-level chain cannot stream");

    // ---- the .octex container -----------------------------------------------------------------------
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aver-texstream-math-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "cache", ec);
    const std::string src = (root / "albedo.png").string();
    const u8 stub[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    check(writeFileBytes(src, stub, sizeof(stub)), "a stand-in source file for the key to stat");
    setTextureCacheDir((root / "cache").string());

    CachedTexture tex;
    tex.format = rhi::Format::BC7UnormSrgb;
    tex.width = 512; tex.height = 512; tex.mips = 10;
    tex.averageLinear[0] = 0.25f; tex.averageLinear[1] = 0.5f; tex.averageLinear[2] = 0.75f;
    tex.coveragePreserved = true;
    for (u32 m = 0; m < tex.mips; ++m) {
        std::vector<u8> blocks(static_cast<usize>(bc7MipBytes(mipExtent(512, m), mipExtent(512, m))));
        for (usize i = 0; i < blocks.size(); ++i) blocks[i] = static_cast<u8>(m * 31 + i * 7 + 1);
        tex.mipBlocks.push_back(std::move(blocks));
    }
    check(saveCachedTexture(src, TextureUsage::Colour, tex), "a cache entry is written");

    CachedTextureInfo info;
    check(statCachedTexture(src, TextureUsage::Colour, info) && info.ok, "statCachedTexture finds it from the header alone");
    check(info.width == 512 && info.height == 512 && info.mips == 10 && info.format == rhi::Format::BC7UnormSrgb, "...with the right shape");
    check(info.averageLinear[0] == 0.25f && info.averageLinear[2] == 0.75f && info.coveragePreserved, "...and the right averages and flag");
    check(info.mipOffset[0] == 40, "mip 0 follows the 40-byte header");
    bool contiguous = true;
    for (u32 m = 0; m < tex.mips; ++m) contiguous = contiguous && info.mipBytes(m) == tex.mipBlocks[m].size();
    check(contiguous, "every mip's byte count equals what was written");
    std::vector<u8> whole;
    check(readFileBytes(info.file, whole) && whole.size() == info.mipOffset[info.mips], "the last offset is exactly the file size");

    CachedTexture loaded;
    check(loadCachedTexture(src, TextureUsage::Colour, loaded) && loaded.mipBlocks == tex.mipBlocks, "loadCachedTexture still returns the same blocks");

    bool allEqual = true;
    for (u32 m = 0; m < tex.mips; ++m) {
        std::vector<u8> one;
        allEqual = allEqual && readCachedMips(info, m, m + 1, one) && one == tex.mipBlocks[m];
    }
    check(allEqual, "readCachedMips reads each mip byte for byte against the written blocks");

    std::vector<u8> range, expect;
    for (u32 m = 3; m < 7; ++m) expect.insert(expect.end(), tex.mipBlocks[m].begin(), tex.mipBlocks[m].end());
    check(readCachedMips(info, 3, 7, range) && range == expect, "a mip range comes back as the blocks back to back");
    expect.clear();
    for (u32 m = 0; m < tex.mips; ++m) expect.insert(expect.end(), tex.mipBlocks[m].begin(), tex.mipBlocks[m].end());
    check(readCachedMips(info, 0, tex.mips, range) && range == expect, "the whole chain as one range");
    check(!readCachedMips(info, 4, 4, range) && !readCachedMips(info, 5, 3, range) && !readCachedMips(info, 0, 11, range),
          "an empty, reversed or out-of-chain range is refused");
    CachedTextureInfo bad;
    check(!readCachedMips(bad, 0, 1, range), "an info that is not ok reads nothing");

    CachedTextureInfo other;
    check(!statCachedTexture(src, TextureUsage::Data, other) && !other.ok, "another usage is a different entry, so a miss");
    check(!statCachedTexture((root / "nothing.png").string(), TextureUsage::Colour, other), "a missing source is a miss");

    // A file that lost its tail: both readers call it a miss, and a range read past the end fails.
    {
        std::vector<u8> cut(whole.begin(), whole.end() - 100);
        check(writeFileBytes(info.file, cut.data(), cut.size()), "the entry is truncated by 100 bytes");
        check(!statCachedTexture(src, TextureUsage::Colour, other), "a truncated file is not ok for stat");
        check(!loadCachedTexture(src, TextureUsage::Colour, loaded), "...nor for the whole-file loader");
        check(!readCachedMips(info, 9, 10, range), "a stale info reading past the end of the shorter file fails");
        check(!readCachedMips(info, 0, 10, range), "...and the whole chain no longer reads");
    }
    // A file with extra bytes at the end.
    {
        std::vector<u8> grown = whole;
        grown.push_back(0);
        writeFileBytes(info.file, grown.data(), grown.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "trailing bytes make it a miss");
    }
    // Header damage.
    {
        std::vector<u8> damaged = whole;
        damaged[0] ^= 0xFF;
        writeFileBytes(info.file, damaged.data(), damaged.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "a bad magic is a miss");
        damaged = whole;
        damaged[4] = 99;
        writeFileBytes(info.file, damaged.data(), damaged.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "a different container version is a miss");
        damaged = whole;
        damaged[8] = 0;   // format Unknown
        writeFileBytes(info.file, damaged.data(), damaged.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "a format this cache never writes is a miss");
        damaged = whole;
        damaged[20] = 33;   // mips = 33 > kMaxTextureMips
        writeFileBytes(info.file, damaged.data(), damaged.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "an absurd mip count is a miss");
        std::vector<u8> tiny(10, 0);
        writeFileBytes(info.file, tiny.data(), tiny.size());
        check(!statCachedTexture(src, TextureUsage::Colour, other), "a file shorter than the header is a miss");
    }
    // Restored, it is usable again.
    writeFileBytes(info.file, whole.data(), whole.size());
    check(statCachedTexture(src, TextureUsage::Colour, other) && other.ok, "the intact bytes are ok again");

    setTextureCacheDir("");
    check(!statCachedTexture(src, TextureUsage::Colour, other), "with no cache directory set, stat is a miss");

    std::filesystem::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
