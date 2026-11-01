// TextureLoaderTest -- assets::requestTexture / takeTexture against real image files and a recording factory.
//
// Default run: no worker threads (setTextureWorkerCount(0)); the test runs queued jobs itself, so every state is exact: a request is
// Pending and costs nothing, a job makes it Ready, take is only the GPU copy, a second request for the same file hits the
// derived-data cache and compresses nothing, a Demand request runs before an older Prefetch one, a failure is reported once and a
// retry tries again, a cancel drops queued jobs and prepared results. "threads" run: the same pool with real workers, polling,
// and a cancel while jobs are running.
#include "MockRhi.hpp"
#include "aver/assets/TextureUpload.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace aver;
using namespace aver::assets;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A 24-bit uncompressed BMP, bottom-up, a gradient with texture to it so the encoder has something to do.
static void writeBmp(const fs::path& path, u32 w, u32 h, u32 seed) {
    const u32 rowBytes = (w * 3 + 3) & ~3u;
    std::vector<u8> px(static_cast<usize>(rowBytes) * h, 0);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[static_cast<usize>(y) * rowBytes + x * 3];
            p[0] = static_cast<u8>((x * 255) / w);
            p[1] = static_cast<u8>(((y * 255) / h) ^ ((x * seed) & 31));
            p[2] = static_cast<u8>((x ^ y) * 7 + seed);
        }
    std::vector<u8> file(54, 0);
    const u32 size = 54 + static_cast<u32>(px.size());
    auto put32 = [&](usize at, u32 v) { for (int i = 0; i < 4; ++i) file[at + i] = static_cast<u8>(v >> (8 * i)); };
    file[0] = 'B'; file[1] = 'M';
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    file[26] = 1; file[28] = 24; put32(34, static_cast<u32>(px.size()));
    file.insert(file.end(), px.begin(), px.end());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

static bool waitReady(const std::string& path, TextureUsage u, std::string* err = nullptr) {
    for (int i = 0; i < 4000; ++i) {
        const TextureState s = requestTexture(path, u, TexturePriority::Demand, err);
        if (s == TextureState::Ready) return true;
        if (s == TextureState::Failed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static void testStateMachine(const fs::path& dir) {
    AVER_INFO("=== queued -> ready -> taken ===");
    test::MockFactory fac;
    const std::string big = (dir / "big.bmp").string();
    std::string err;
    check(requestTexture(big, TextureUsage::Colour, TexturePriority::Prefetch) == TextureState::Pending, "a request is Pending and returns at once");
    check(requestTexture(big, TextureUsage::Colour, TexturePriority::Demand) == TextureState::Pending, "asking again is still Pending");
    TextureLoadStats s = textureLoadStats();
    check(s.requested == 1 && s.queued == 1 && s.ready == 0, "two asks are one job");
    check(takeTexture(fac, big, TextureUsage::Colour, &err) == 0 && !err.empty(), "taking before it is ready fails, with a reason");
    check(fac.textures.empty(), "and created no GPU texture");
    check(runTextureJobsHere() == 1, "one job runs");
    check(requestTexture(big, TextureUsage::Colour, TexturePriority::Demand) == TextureState::Ready, "now Ready");
    s = textureLoadStats();
    check(s.compressed == 1 && s.cacheHits == 0 && s.ready == 1 && s.readyBytes > 0, "first visit: decoded and BC7-encoded once");
    check(fac.textures.empty(), "still no GPU texture: preparing never touches the RHI");
    TextureUploadInfo info;
    const rhi::TextureHandle h = takeTexture(fac, big, TextureUsage::Colour, &err, &info);
    check(h != 0 && fac.textures.size() == 1, "take creates the GPU texture");
    check(fac.textures[0].format == rhi::Format::BC7UnormSrgb && fac.textures[0].width == 256 && fac.textures[0].mips == 9,
          "256x256 sRGB BC7 with a full 9-level chain");
    check(info.width == 256 && info.bytes > 0, "and reports its size");
    check(textureLoadStats().ready == 0 && textureLoadStats().uploaded == 1, "the prepared result is consumed");
    check(takeTexture(fac, big, TextureUsage::Colour, &err) == 0, "a second take finds nothing");

    AVER_INFO("=== the derived-data cache: a second visit compresses nothing ===");
    check(requestTexture(big, TextureUsage::Colour, TexturePriority::Demand) == TextureState::Pending, "asking again queues a fresh job");
    runTextureJobsHere();
    s = textureLoadStats();
    check(s.cacheHits == 1 && s.compressed == 1, "it read the cache: still one compression in all");
    const rhi::TextureHandle h2 = takeTexture(fac, big, TextureUsage::Colour, &err);
    check(h2 != 0 && fac.descOf(h2)->mips == 9 && fac.descOf(h2)->format == rhi::Format::BC7UnormSrgb, "and the same chain reaches the GPU");

    AVER_INFO("=== one file, two usages, two jobs ===");
    check(requestTexture(big, TextureUsage::Data, TexturePriority::Demand) == TextureState::Pending, "linear data is its own job");
    runTextureJobsHere();
    const rhi::TextureHandle h3 = takeTexture(fac, big, TextureUsage::Data, &err);
    check(h3 != 0 && fac.descOf(h3)->format == rhi::Format::BC7Unorm, "BC7 without sRGB");
    check(textureLoadStats().compressed == 2, "compressed once per usage");

    AVER_INFO("=== a small texture stays RGBA8 ===");
    const std::string small = (dir / "small.bmp").string();
    requestTexture(small, TextureUsage::Colour, TexturePriority::Demand);
    runTextureJobsHere();
    const rhi::TextureHandle h4 = takeTexture(fac, small, TextureUsage::Colour, &err);
    check(h4 != 0 && fac.descOf(h4)->format == rhi::Format::RGBA8UnormSrgb && fac.descOf(h4)->width == 8, "8x8 is not block-compressed");
}

static void testPriority(const fs::path& dir) {
    AVER_INFO("=== a Demand request runs before an older Prefetch one ===");
    const std::string a = (dir / "a.bmp").string(), b = (dir / "b.bmp").string(), c = (dir / "c.bmp").string();
    requestTexture(a, TextureUsage::Data, TexturePriority::Prefetch);
    requestTexture(b, TextureUsage::Data, TexturePriority::Prefetch);
    requestTexture(c, TextureUsage::Data, TexturePriority::Prefetch);
    requestTexture(c, TextureUsage::Data, TexturePriority::Demand);   // the draw wants c now
    check(runTextureJobsHere(1) == 1, "one job runs");
    check(requestTexture(c, TextureUsage::Data, TexturePriority::Prefetch) == TextureState::Ready, "it was c");
    check(requestTexture(a, TextureUsage::Data, TexturePriority::Prefetch) == TextureState::Pending, "a still waits");
    runTextureJobsHere(1);
    check(requestTexture(a, TextureUsage::Data, TexturePriority::Prefetch) == TextureState::Ready &&
          requestTexture(b, TextureUsage::Data, TexturePriority::Prefetch) == TextureState::Pending, "then a (the older prefetch), then b");
    runTextureJobsHere();
    cancelTextureRequests();
}

static void testFailureAndCancel(const fs::path& dir) {
    AVER_INFO("=== a file that does not decode: reported once, then retried ===");
    test::MockFactory fac;
    const std::string missing = (dir / "nope.bmp").string();
    std::string err;
    requestTexture(missing, TextureUsage::Colour, TexturePriority::Demand);
    runTextureJobsHere();
    check(requestTexture(missing, TextureUsage::Colour, TexturePriority::Demand, &err) == TextureState::Failed && !err.empty(),
          "Failed, with the reason");
    check(requestTexture(missing, TextureUsage::Colour, TexturePriority::Demand) == TextureState::Pending, "the next request tries again");
    runTextureJobsHere();
    check(textureLoadStats().failed >= 2, "and failed again");
    requestTexture(missing, TextureUsage::Colour, TexturePriority::Demand);   // consume the report

    AVER_INFO("=== cancel drops queued jobs and prepared results ===");
    const std::string big = (dir / "big.bmp").string();
    const std::string small = (dir / "small.bmp").string();
    requestTexture(big, TextureUsage::Colour, TexturePriority::Prefetch);
    runTextureJobsHere();
    requestTexture(small, TextureUsage::Colour, TexturePriority::Prefetch);   // queued, not run
    check(textureLoadStats().ready == 1 && textureLoadStats().queued == 1, "one prepared, one queued");
    cancelTextureRequests();
    TextureLoadStats s = textureLoadStats();
    check(s.ready == 0 && s.queued == 0 && s.readyBytes == 0, "both gone");
    check(takeTexture(fac, big, TextureUsage::Colour, &err) == 0, "a cancelled result cannot be taken");
    check(fac.textures.empty(), "and nothing was uploaded");
    check(requestTexture(big, TextureUsage::Colour, TexturePriority::Prefetch) == TextureState::Pending, "asking again starts over");
    runTextureJobsHere();
    check(textureLoadStats().cacheHits >= 2, "from the cache");
    cancelTextureRequests();
}

static void testThreads(const fs::path& dir) {
    AVER_INFO("=== real workers ===");
    test::MockFactory fac;
    std::vector<std::string> files;
    for (u32 i = 0; i < 6; ++i) {
        files.push_back((dir / ("t" + std::to_string(i) + ".bmp")).string());
        writeBmp(files.back(), 256, 256, 3 + i);
    }
    for (const std::string& f : files) requestTexture(f, TextureUsage::Colour, TexturePriority::Prefetch);
    std::string err;
    bool all = true;
    for (const std::string& f : files) all = waitReady(f, TextureUsage::Colour) && all;
    check(all, "six textures prepared in the background");
    u32 taken = 0;
    for (const std::string& f : files) taken += takeTexture(fac, f, TextureUsage::Colour, &err) != 0;
    check(taken == 6 && fac.textures.size() == 6, "and taken on this thread");
    check(textureLoadStats().compressed >= 6, "every one was encoded");

    // A cancel while jobs run: nothing crashes, nothing stale is left, and a later request still works.
    writeBmp(dir / "u0.bmp", 512, 512, 11); writeBmp(dir / "u1.bmp", 512, 512, 12); writeBmp(dir / "u2.bmp", 512, 512, 13);
    for (u32 i = 0; i < 3; ++i) requestTexture((dir / ("u" + std::to_string(i) + ".bmp")).string(), TextureUsage::Data, TexturePriority::Prefetch);
    cancelTextureRequests();
    for (int i = 0; i < 400; ++i) {
        const TextureLoadStats s = textureLoadStats();
        if (s.running == 0 && s.queued == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TextureLoadStats s = textureLoadStats();
    check(s.running == 0 && s.queued == 0 && s.ready == 0 && s.readyBytes == 0, "after the cancel nothing is queued, running or held");
    check(waitReady((dir / "u0.bmp").string(), TextureUsage::Data), "a texture asked for again after a cancel arrives");
    cancelTextureRequests();
}

int main(int argc, char** argv) {
    const bool threads = argc > 1 && std::string(argv[1]) == "threads";
    const fs::path dir = fs::temp_directory_path() / (threads ? "aver_texture_loader_test_threads" : "aver_texture_loader_test");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "ddc");
    writeBmp(dir / "big.bmp", 256, 256, 1);
    writeBmp(dir / "small.bmp", 8, 8, 2);
    for (const char* n : {"a", "b", "c"}) writeBmp(dir / (std::string(n) + ".bmp"), 256, 256, 5);
    setTextureCacheDir((dir / "ddc").string());
    if (!threads) setTextureWorkerCount(0);

    if (threads) testThreads(dir);
    else { testStateMachine(dir); testPriority(dir); testFailureAndCancel(dir); }

    fs::remove_all(dir, ec);
    if (g_failures) { AVER_ERROR("TextureLoaderTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("TextureLoaderTest: all passed");
    return 0;
}
