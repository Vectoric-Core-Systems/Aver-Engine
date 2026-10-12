// .cube 3D LUT reader. See CubeLut.hpp.
#include "aver/formats/CubeLut.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/formats/IesProfile.hpp"   // floatToHalf
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

// Trilinear sample of an N^3 float RGB grid at `u` in [0,1]^3 (grid coordinates u * (N - 1)).
void sampleGrid(const std::vector<f32>& g, u32 n, const f32 u[3], f32 out[3]) {
    f32 p[3];
    u32 i0[3], i1[3];
    for (int a = 0; a < 3; ++a) {
        p[a] = std::clamp(u[a], 0.0f, 1.0f) * static_cast<f32>(n - 1);
        i0[a] = std::min(static_cast<u32>(p[a]), n - 1);
        i1[a] = std::min(i0[a] + 1, n - 1);
        p[a] -= static_cast<f32>(i0[a]);
    }
    const auto at = [&](u32 r, u32 gg, u32 b, int c) {
        return g[(static_cast<usize>(b) * n * n + static_cast<usize>(gg) * n + r) * 3 + c];
    };
    for (int c = 0; c < 3; ++c) {
        f32 v = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const u32 r = (corner & 1) ? i1[0] : i0[0], gg = (corner & 2) ? i1[1] : i0[1],
                      b = (corner & 4) ? i1[2] : i0[2];
            const f32 w = ((corner & 1) ? p[0] : 1 - p[0]) * ((corner & 2) ? p[1] : 1 - p[1]) *
                          ((corner & 4) ? p[2] : 1 - p[2]);
            v += w * at(r, gg, b, c);
        }
        out[c] = v;
    }
}

} // namespace

bool parseCubeLut(std::string_view text, CubeLut& out, std::string* err) {
    out = CubeLut{};
    u32 n = 0;
    bool is1d = false;
    std::vector<f32> data;   // RGB triples as read
    usize pos = 0;
    while (pos <= text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = trim(truncateHash(text.substr(pos, eol - pos)));
        pos = eol + 1;
        if (line.empty()) continue;
        const auto t = splitWhitespace(line);
        if (t.empty()) continue;
        const char c0 = t[0][0];
        if ((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.') {
            if (t.size() < 3) return fail(err, "a data line needs three numbers");
            for (int c = 0; c < 3; ++c) {
                f32 v = 0;
                const auto r = aver::fromChars(t[c].data(), t[c].data() + t[c].size(), v);
                if (r.ec != std::errc{}) return fail(err, "non-numeric data in the table");
                data.push_back(v);
            }
        } else if (equalsCI(t[0], "TITLE")) {
            const usize q = line.find('"'), q2 = line.rfind('"');
            out.title = (q != std::string_view::npos && q2 > q) ? std::string(line.substr(q + 1, q2 - q - 1))
                                                              : std::string(trim(line.substr(5)));
        } else if (equalsCI(t[0], "LUT_3D_SIZE") && t.size() >= 2) {
            const i32 v = parseI32(t[1]);
            if (v < static_cast<i32>(kCubeLutMinSize) || v > static_cast<i32>(kCubeLutMaxSize))
                return fail(err, "LUT_3D_SIZE " + std::string(t[1]) + " is outside " +
                                 std::to_string(kCubeLutMinSize) + ".." + std::to_string(kCubeLutMaxSize));
            n = static_cast<u32>(v);
        } else if (equalsCI(t[0], "LUT_1D_SIZE")) {
            is1d = true;
        } else if (equalsCI(t[0], "DOMAIN_MIN") && t.size() >= 4) {
            for (int c = 0; c < 3; ++c) out.domainMin[c] = static_cast<f32>(parseF64(t[1 + c]));
        } else if (equalsCI(t[0], "DOMAIN_MAX") && t.size() >= 4) {
            for (int c = 0; c < 3; ++c) out.domainMax[c] = static_cast<f32>(parseF64(t[1 + c], 1.0));
        } else if (equalsCI(t[0], "LUT_3D_INPUT_RANGE") && t.size() >= 3) {   // Resolve's spelling of the domain
            for (int c = 0; c < 3; ++c) {
                out.domainMin[c] = static_cast<f32>(parseF64(t[1]));
                out.domainMax[c] = static_cast<f32>(parseF64(t[2], 1.0));
            }
        }
        // anything else (LUT_1D_INPUT_RANGE, vendor keywords): skipped
    }
    if (n == 0) return fail(err, is1d ? "1D LUTs are not supported (only LUT_3D_SIZE)" : "no LUT_3D_SIZE");
    const usize want = static_cast<usize>(n) * n * n;
    if (data.size() != want * 3)
        return fail(err, "expected " + std::to_string(want) + " table rows for size " + std::to_string(n) +
                         ", found " + std::to_string(data.size() / 3));
    for (int c = 0; c < 3; ++c)
        if (!(out.domainMax[c] > out.domainMin[c])) return fail(err, "DOMAIN_MAX must exceed DOMAIN_MIN");

    // Re-grid onto [0,1] when the file's domain is something else, so the shader can sample with the display
    // colour directly. Input x in [0,1] reads the file at (x - min) / (max - min), clamped.
    bool unit = true;
    for (int c = 0; c < 3; ++c) unit = unit && out.domainMin[c] == 0.0f && out.domainMax[c] == 1.0f;
    std::vector<f32> grid;
    const std::vector<f32>* src = &data;
    if (!unit) {
        grid.resize(data.size());
        const f32 inv = 1.0f / static_cast<f32>(n - 1);
        usize k = 0;
        for (u32 b = 0; b < n; ++b)
            for (u32 g = 0; g < n; ++g)
                for (u32 r = 0; r < n; ++r, k += 3) {
                    const f32 x[3] = {static_cast<f32>(r) * inv, static_cast<f32>(g) * inv, static_cast<f32>(b) * inv};
                    f32 u[3];
                    for (int c = 0; c < 3; ++c)
                        u[c] = (x[c] - out.domainMin[c]) / (out.domainMax[c] - out.domainMin[c]);
                    sampleGrid(data, n, u, &grid[k]);
                }
        src = &grid;
    }

    out.size = n;
    out.rgba16f.resize(want * 4);
    for (usize i = 0; i < want; ++i) {
        for (int c = 0; c < 3; ++c) out.rgba16f[i * 4 + c] = floatToHalf((*src)[i * 3 + c]);
        out.rgba16f[i * 4 + 3] = floatToHalf(1.0f);
    }
    return true;
}

bool loadCubeLut(const std::string& path, CubeLut& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) return fail(err, "cannot read " + path);
    return parseCubeLut(text, out, err);
}

std::string writeIdentityCube(u32 size, const char* title) {
    size = std::clamp(size, kCubeLutMinSize, kCubeLutMaxSize);
    std::string s = std::string("TITLE \"") + title + "\"\nLUT_3D_SIZE " + std::to_string(size) + "\n";
    const f32 inv = 1.0f / static_cast<f32>(size - 1);
    char buf[80];
    for (u32 b = 0; b < size; ++b)
        for (u32 g = 0; g < size; ++g)
            for (u32 r = 0; r < size; ++r) {
                std::snprintf(buf, sizeof buf, "%.6f %.6f %.6f\n", r * inv, g * inv, b * inv);
                s += buf;
            }
    return s;
}

} // namespace aver::fmt
