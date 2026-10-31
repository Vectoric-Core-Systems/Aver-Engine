// ImageDecodeCorpus -- decodes every .webp, .svg and .exr under a directory and prints counts. A smoke
// run over somebody else's files, not a test of ours: given no argument it does nothing and passes, so
// the ctest walk can register it.
//
//   ImageDecodeCorpus <directory> [--list-failures]
//
// Files are read only through aver::decodeImage / decodeFloatImage and never copied or executed.
// Prints, per extension: files, decoded, refused, the largest image, and (with --list-failures) the path
// and reason of each refusal. Exit code is 0 unless the directory cannot be read or a decode crashes.
#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace aver;

namespace {

struct Tally {
    int files = 0, ok = 0, refused = 0;
    u32 maxW = 0, maxH = 0;
    u64 pixels = 0;
    std::map<std::string, int> reasons;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("ImageDecodeCorpus: no directory given, nothing to do");
        return 0;
    }
    const bool listFailures = argc > 2 && std::string(argv[2]) == "--list-failures";
    std::error_code ec;
    if (!std::filesystem::is_directory(argv[1], ec)) {
        std::printf("ImageDecodeCorpus: '%s' is not a directory\n", argv[1]);
        return 1;
    }

    std::map<std::string, Tally> byExt;
    std::vector<std::string> failures;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::filesystem::recursive_directory_iterator it(argv[1], std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".webp" && ext != ".svg" && ext != ".exr") continue;
        Tally& t = byExt[ext];
        ++t.files;
        const std::string path = it->path().string();
        std::string err;
        ImageData img;
        bool ok = decodeImage(path, img, &err) && img.valid();
        if (ok && ext == ".exr") {
            FloatImageData f;   // and the float form
            ok = decodeFloatImage(path, f, &err) && f.valid() && f.width == img.width && f.height == img.height;
            if (!ok && err.empty()) err = "float decode disagrees with the 8-bit decode";
        }
        if (ok) {
            ++t.ok;
            t.maxW = std::max(t.maxW, img.width);
            t.maxH = std::max(t.maxH, img.height);
            t.pixels += u64(img.width) * img.height;
        } else {
            ++t.refused;
            ++t.reasons[err.empty() ? "unknown" : err];
            if (listFailures) failures.push_back(path + " : " + err);
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    int total = 0, okTotal = 0;
    for (const auto& [ext, t] : byExt) {
        std::printf("%-6s files %5d  decoded %5d  refused %4d  largest %ux%u  pixels %llu\n", ext.c_str(), t.files, t.ok, t.refused, t.maxW, t.maxH,
                    static_cast<unsigned long long>(t.pixels));
        for (const auto& [why, n] : t.reasons) std::printf("         %4d x %s\n", n, why.c_str());
        total += t.files;
        okTotal += t.ok;
    }
    for (const std::string& f : failures) std::printf("  refused: %s\n", f.c_str());
    std::printf("total %d files, %d decoded, %d refused, %.1f s\n", total, okTotal, total - okTotal, secs);
    return 0;
}
