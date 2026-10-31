// Equirectangular panorama reading and the sky-dome fit. See PanoramaSky.hpp.
#include "aver/formats/PanoramaSky.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aver::fmt {

namespace {

constexpr f64 kPi = 3.14159265358979323846;

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

f32 rgbeChannel(u8 m, int e) { return m == 0 ? 0.0f : std::ldexp(static_cast<f32>(m), e - 136); }

void rgbeToFloat(const u8 px[4], f32 out[3]) {
    if (px[3] == 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    for (int c = 0; c < 3; ++c) out[c] = rgbeChannel(px[c], px[3]);
}

f64 srgb22ToLinear(f64 c) { return std::pow(c, 2.2); }

}  // namespace

bool decodeRadianceHdr(const u8* bytes, usize size, PanoramaImage& out, std::string* err) {
    out = PanoramaImage{};
    usize pos = 0;
    auto readLine = [&](std::string& line) {
        line.clear();
        while (pos < size && bytes[pos] != '\n') line.push_back(static_cast<char>(bytes[pos++]));
        if (pos >= size) return false;
        ++pos;
        return true;
    };
    std::string line;
    if (!readLine(line) || line.rfind("#?", 0) != 0) return fail(err, "not a Radiance .hdr file (no #? signature)");
    bool rgbe = false;
    while (true) {
        if (!readLine(line)) return fail(err, "the header ends before the resolution line");
        if (line.empty()) break;
        if (line.rfind("FORMAT=", 0) == 0) rgbe = line.find("32-bit_rle_rgbe") != std::string::npos;
        if (line.rfind("FORMAT=", 0) == 0 && !rgbe) return fail(err, "only the 32-bit_rle_rgbe format is read");
    }
    if (!readLine(line)) return fail(err, "no resolution line");
    char ySign = 0, xSign = 0;
    int h = 0, w = 0;
    if (std::sscanf(line.c_str(), "%cY %d %cX %d", &ySign, &h, &xSign, &w) != 4 || (ySign != '-' && ySign != '+') || (xSign != '-' && xSign != '+') || w <= 0 || h <= 0 ||
        w > 65536 || h > 65536)
        return fail(err, "an unsupported resolution line");

    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgb.assign(static_cast<usize>(w) * static_cast<usize>(h) * 3, 0.0f);
    std::vector<u8> scan(static_cast<usize>(w) * 4);
    for (int y = 0; y < h; ++y) {
        if (pos + 4 > size) return fail(err, "the pixel data is cut short");
        const bool rle = w >= 8 && w < 32768 && bytes[pos] == 2 && bytes[pos + 1] == 2 && (bytes[pos + 2] & 0x80) == 0;
        if (rle) {
            if (((static_cast<int>(bytes[pos + 2]) << 8) | bytes[pos + 3]) != w) return fail(err, "a scanline's width disagrees with the header");
            pos += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < w) {
                    if (pos >= size) return fail(err, "the pixel data is cut short");
                    int run = bytes[pos++];
                    if (run > 128) {
                        run -= 128;
                        if (pos >= size || x + run > w) return fail(err, "a run overruns its scanline");
                        const u8 v = bytes[pos++];
                        for (int i = 0; i < run; ++i) scan[static_cast<usize>(x++) * 4 + static_cast<usize>(c)] = v;
                    } else {
                        if (run == 0 || x + run > w || pos + static_cast<usize>(run) > size) return fail(err, "a literal run overruns its scanline");
                        for (int i = 0; i < run; ++i) scan[static_cast<usize>(x++) * 4 + static_cast<usize>(c)] = bytes[pos++];
                    }
                }
            }
        } else {
            if (pos + static_cast<usize>(w) * 4 > size) return fail(err, "the pixel data is cut short");
            std::memcpy(scan.data(), bytes + pos, static_cast<usize>(w) * 4);
            pos += static_cast<usize>(w) * 4;
        }
        const int row = ySign == '-' ? y : h - 1 - y;
        for (int x = 0; x < w; ++x) {
            const int col = xSign == '+' ? x : w - 1 - x;
            rgbeToFloat(&scan[static_cast<usize>(x) * 4], &out.rgb[(static_cast<usize>(row) * static_cast<usize>(w) + static_cast<usize>(col)) * 3]);
        }
    }
    return true;
}

bool loadPanorama(const std::string& path, PanoramaImage& out, std::string* err) {
    out = PanoramaImage{};
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isHdr = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".hdr") == 0;
    const bool isExr = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exr") == 0;
    if (isExr) {
        FloatImageData f;
        if (!decodeFloatImage(path, f, err) || !f.valid()) return false;
        out.width = f.width;
        out.height = f.height;
        out.rgb.resize(static_cast<usize>(f.width) * f.height * 3);
        for (usize i = 0, n = static_cast<usize>(f.width) * f.height; i < n; ++i)
            for (usize c = 0; c < 3; ++c) {
                const f32 v = f.rgba[i * 4 + c];
                out.rgb[i * 3 + c] = v > 0.0f ? v : 0.0f;   // negatives and NaN are not radiance
            }
        return true;
    }
    if (isHdr) {
        std::vector<u8> bytes;
        if (!readFileBytes(path, bytes)) return fail(err, "the file could not be read");
        return decodeRadianceHdr(bytes.data(), bytes.size(), out, err);
    }
    ImageData img;
    if (!decodeImage(path, img, err) || !img.valid()) return false;
    out.width = img.width;
    out.height = img.height;
    out.rgb.resize(static_cast<usize>(img.width) * img.height * 3);
    for (usize i = 0, n = static_cast<usize>(img.width) * img.height; i < n; ++i)
        for (usize c = 0; c < 3; ++c) out.rgb[i * 3 + c] = static_cast<f32>(srgb22ToLinear(img.pixels[i * 4 + c] / 255.0));
    return true;
}

bool fitSkyDome(const PanoramaImage& img, f64 energy, SkyDomeFit& out) {
    out = SkyDomeFit{};
    if (!img.valid()) return false;
    energy = std::max(energy, 0.0);

    // Mean radiance of each row (azimuth-averaged), with the row's elevation and the solid angle it covers.
    struct Row { f64 sinElev; f64 weight; f64 rgb[3]; };
    std::vector<Row> rows(img.height);
    for (u32 y = 0; y < img.height; ++y) {
        const f64 polar = (static_cast<f64>(y) + 0.5) / img.height * kPi;   // 0 at the top
        Row& r = rows[y];
        r.sinElev = std::cos(polar);
        r.weight = std::sin(polar);
        r.rgb[0] = r.rgb[1] = r.rgb[2] = 0.0;
        for (u32 x = 0; x < img.width; ++x)
            for (int c = 0; c < 3; ++c) r.rgb[c] += static_cast<f64>(img.rgb[(static_cast<usize>(y) * img.width + x) * 3 + static_cast<usize>(c)]);
        for (int c = 0; c < 3; ++c) r.rgb[c] = r.rgb[c] / img.width * energy;
    }
    auto average = [&](f64 lo, f64 hi, f64 mean[3]) {
        f64 sum[3] = {0, 0, 0}, wsum = 0;
        for (const Row& r : rows)
            if (r.sinElev >= lo && r.sinElev <= hi) { for (int c = 0; c < 3; ++c) sum[c] += r.rgb[c] * r.weight; wsum += r.weight; }
        for (int c = 0; c < 3; ++c) mean[c] = wsum > 0 ? sum[c] / wsum : 0.0;
        return wsum > 0;
    };
    constexpr f64 sin60 = 0.8660254037844386, sin15 = 0.2588190451025207;
    f64 zen[3], hor[3], up[3], down[3];
    if (!average(sin60, 1.0, zen)) average(0.5, 1.0, zen);
    if (!average(0.0, sin15, hor)) average(-0.1, 0.3, hor);
    average(0.0, 1.0, up);
    average(-1.0, -1e-9, down);
    for (int c = 0; c < 3; ++c) out.groundAlbedo[c] = up[c] > 1e-6 ? std::clamp(down[c] / up[c], 0.0, 1.0) : 0.0;

    // The dome's colour at elevation e is C(e) = H + (Z - H) * t(e), t = pow(sin e * 0.5 + 0.5, k): H (the "horizon" colour) is only reached
    // straight down and Z (the zenith) straight up. For a given k the two band averages (the zenith cap and the horizon band) fix H and Z per
    // channel exactly, as a 2x2 solve on the bands' mean t; k is the grid value whose dome follows the upper rows' luminance best.
    auto lum = [](const f64 c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; };
    auto meanT = [&](f64 lo, f64 hi, f64 k) {
        f64 sum = 0, wsum = 0;
        for (const Row& r : rows)
            if (r.sinElev >= lo && r.sinElev <= hi) { sum += std::pow(r.sinElev * 0.5 + 0.5, k) * r.weight; wsum += r.weight; }
        return wsum > 0 ? sum / wsum : 0.5;
    };
    auto solve = [&](f64 k, f64 Z[3], f64 H[3]) {
        const f64 tc = meanT(sin60, 1.0, k), th = meanT(0.0, sin15, k);
        const f64 det = tc - th;
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(det) < 1e-9) { Z[c] = zen[c]; H[c] = hor[c]; continue; }
            Z[c] = std::max((zen[c] * (1.0 - th) - hor[c] * (1.0 - tc)) / det, 0.0);
            H[c] = std::max((hor[c] - Z[c] * th) / (1.0 - th), 0.0);
        }
    };
    f64 bestK = 0.65, bestErr = 1e300, bestZ[3] = {zen[0], zen[1], zen[2]}, bestH[3] = {hor[0], hor[1], hor[2]};
    if (std::fabs(lum(zen) - lum(hor)) > 1e-9) {
        for (f64 k = 0.1; k <= 8.0; k += 0.05) {
            f64 Z[3], H[3];
            solve(k, Z, H);
            f64 err = 0;
            for (const Row& r : rows) {
                if (r.sinElev < 0.0) continue;
                const f64 t = std::pow(r.sinElev * 0.5 + 0.5, k);
                const f64 d = lum(H) + (lum(Z) - lum(H)) * t - lum(r.rgb);
                err += d * d * r.weight;
            }
            if (err < bestErr) {
                bestErr = err; bestK = k;
                for (int c = 0; c < 3; ++c) { bestZ[c] = Z[c]; bestH[c] = H[c]; }
            }
        }
    }
    out.domeExponent = bestK;
    for (int c = 0; c < 3; ++c) {
        out.zenith[c] = std::pow(std::max(bestZ[c], 0.0), 1.0 / 2.2);
        out.horizon[c] = std::pow(std::max(bestH[c], 0.0), 1.0 / 2.2);
    }
    return true;
}

}  // namespace aver::fmt
