// ThumbnailDiskCacheTest -- keys and LRU eviction for the Content Browser's on-disk thumbnail cache.
#include "../../../sandbox/src/ThumbnailDiskCache.hpp"

#include "aver/core/Log.hpp"

#include <fstream>
#include <string>

using namespace aver;
using namespace aver::editor;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void writeFile(const std::string& p, usize bytes) {
    std::ofstream f(p, std::ios::binary);
    f << std::string(bytes, 'x');
}

int main() {
    AVER_INFO("=== thumbnail disk cache ===");
    {
        const std::string k = thumbnailDiskKey("Content/a.png", 100, 2000);
        check(k.size() == 16, "key is 16 hex characters");
        check(k == thumbnailDiskKey("Content/a.png", 100, 2000), "key is deterministic");
        check(k != thumbnailDiskKey("Content/b.png", 100, 2000), "path changes the key");
        check(k != thumbnailDiskKey("Content/a.png", 101, 2000), "mtime changes the key");
        check(k != thumbnailDiskKey("Content/a.png", 100, 2001), "size changes the key");
        check(k != thumbnailDiskKey("Content/a.png", 100, 2000, kThumbnailDiskVersion + 1), "version changes the key");
    }
    const fs::path dir = fs::temp_directory_path() / "aver_thumb_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        ThumbnailDiskCache c;
        c.configure(dir.string(), 0);
        check(!c.enabled(), "a zero cap disables the cache");
        c.configure(dir.string(), 1000);
        check(c.enabled() && c.totalBytes() == 0, "an empty folder starts at zero");
        std::string p;
        check(!c.lookup("missing", p), "a missing key misses");

        writeFile(c.pathFor("aaaa"), 400); c.recordWritten("aaaa", 400);
        writeFile(c.pathFor("bbbb"), 400); c.recordWritten("bbbb", 400);
        check(c.totalBytes() == 800 && c.fileCount() == 2, "writes are counted");
        check(c.lookup("aaaa", p) && p == c.pathFor("aaaa"), "a written key hits and returns its file");   // aaaa is now newest
        writeFile(c.pathFor("cccc"), 400);
        const std::vector<std::string> gone = c.recordWritten("cccc", 400);
        check(gone.size() == 1 && gone[0] == "bbbb", "past the cap the least recently used file goes (a hit refreshed aaaa)");
        check(!fs::exists(c.pathFor("bbbb")) && fs::exists(c.pathFor("aaaa")) && fs::exists(c.pathFor("cccc")), "and only that file is deleted");
        check(c.totalBytes() == 800, "the total follows");
        writeFile(c.pathFor("dddd"), 5000);
        c.recordWritten("dddd", 5000);
        check(c.fileCount() == 1 && fs::exists(c.pathFor("dddd")), "a single over-cap file stays alone rather than emptying the cache");
        fs::remove(c.pathFor("dddd"), ec);
        check(!c.lookup("dddd", p) && c.fileCount() == 0, "a file deleted behind its back is dropped on lookup");
    }
    {
        writeFile((dir / "e1.png").string(), 300);
        writeFile((dir / "e2.png").string(), 300);
        writeFile((dir / "notes.txt").string(), 999);
        ThumbnailDiskCache c;
        c.configure(dir.string(), 1000);
        check(c.fileCount() == 2 && c.totalBytes() == 600, "a scan adopts existing .png files only");
        c.configure(dir.string(), 400);
        check(c.fileCount() == 1 && c.totalBytes() == 300, "lowering the cap evicts immediately");
    }
    fs::remove_all(dir, ec);
    if (g_failures == 0) AVER_INFO("=== all {} thumbnail disk cache checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} thumbnail disk cache check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
