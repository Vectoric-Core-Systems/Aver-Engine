// PanoramaSkyTest -- G4 R3, no device. The Radiance .hdr reader (flat and run-length scanlines, both vertical orders, refusals), the equirectangular
// panorama fitted onto Aver's authored sky dome (zenith, horizon, ground albedo and the blend exponent), and Godot's environment parameter mapping
// (aver/formats/GodotEnvironment.hpp).
#include "aver/core/Log.hpp"
#include "aver/formats/GodotEnvironment.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/PanoramaSky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void rgbe(const float c[3], u8 out[4]) {
    const float m = std::max(c[0], std::max(c[1], c[2]));
    if (m < 1e-32f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e = 0;
    const float f = std::frexp(m, &e) * 256.0f / m;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<u8>(c[i] * f);
    out[3] = static_cast<u8>(e + 128);
}

static std::vector<u8> header(const char* resolution, const char* format = "32-bit_rle_rgbe") {
    const std::string h = std::string("#?RADIANCE\nFORMAT=") + format + "\n\n" + resolution + "\n";
    return std::vector<u8>(h.begin(), h.end());
}

// A flat (uncompressed) file: rows are given top to bottom as `-Y` stores them.
static std::vector<u8> flatFile(const std::vector<std::vector<float>>& rowsRgb, int w, const char* res = nullptr) {
    char r[64];
    std::snprintf(r, sizeof r, "-Y %d +X %d", static_cast<int>(rowsRgb.size()), w);
    std::vector<u8> f = header(res ? res : r);
    for (const auto& row : rowsRgb)
        for (int x = 0; x < w; ++x) {
            u8 px[4];
            rgbe(&row[0], px);
            f.insert(f.end(), px, px + 4);
        }
    return f;
}

// The same rows as new-style run-length scanlines: a literal run for the first pixel, a repeat run for the rest, per channel.
static std::vector<u8> rleFile(const std::vector<std::vector<float>>& rowsRgb, int w) {
    char r[64];
    std::snprintf(r, sizeof r, "-Y %d +X %d", static_cast<int>(rowsRgb.size()), w);
    std::vector<u8> f = header(r);
    for (const auto& row : rowsRgb) {
        u8 px[4];
        rgbe(&row[0], px);
        f.push_back(2); f.push_back(2); f.push_back(static_cast<u8>(w >> 8)); f.push_back(static_cast<u8>(w & 255));
        for (int c = 0; c < 4; ++c) {
            f.push_back(1); f.push_back(px[c]);                       // one literal
            f.push_back(static_cast<u8>(128 + (w - 1))); f.push_back(px[c]);   // w - 1 repeats
        }
    }
    return f;
}

static fmt::PanoramaImage image(int w, int h, const std::vector<std::vector<float>>& rows) {
    fmt::PanoramaImage img;
    img.width = static_cast<u32>(w);
    img.height = static_cast<u32>(h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.rgb.insert(img.rgb.end(), rows[static_cast<usize>(y)].begin(), rows[static_cast<usize>(y)].begin() + 3);
    return img;
}

int main() {
    AVER_INFO("PanoramaSkyTest");

    AVER_INFO("-- the .hdr reader --");
    {
        const std::vector<std::vector<float>> rows = {{1.0f, 0.5f, 0.25f}, {4.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {16.0f, 8.0f, 1.0f}};
        std::string err;
        fmt::PanoramaImage a, b;
        const std::vector<u8> flat = flatFile(rows, 16), rle = rleFile(rows, 16);
        check(fmt::decodeRadianceHdr(flat.data(), flat.size(), a, &err) && a.width == 16 && a.height == 4, "a flat file decodes: " + err);
        check(fmt::decodeRadianceHdr(rle.data(), rle.size(), b, &err) && b.width == 16 && b.height == 4, "the run-length file decodes: " + err);
        check(near(a.rgb[0], 1.0, 1e-6) && near(a.rgb[1], 0.5, 1e-6) && near(a.rgb[2], 0.25, 1e-6), "row 0 is the first row of a -Y file: (1, 0.5, 0.25)");
        check(near(a.rgb[(1 * 16 + 5) * 3], 4.0, 1e-6) && near(a.rgb[(1 * 16 + 5) * 3 + 2], 0.0, 1e-6), "row 1, any column, is (4, 2, 0)");
        check(near(a.rgb[(2 * 16) * 3 + 1], 0.0, 1e-9), "an RGBE with exponent 0 is black");
        check(near(a.rgb[(3 * 16 + 15) * 3], 16.0, 1e-5), "the last pixel of the last row");
        check(a.rgb.size() == b.rgb.size() && std::equal(a.rgb.begin(), a.rgb.end(), b.rgb.begin()), "run-length and flat decode to the same pixels");

        const std::vector<u8> up = flatFile(rows, 16, "+Y 4 +X 16");
        fmt::PanoramaImage c;
        check(fmt::decodeRadianceHdr(up.data(), up.size(), c, &err) && near(c.rgb[(3 * 16) * 3], 1.0, 1e-6) && near(c.rgb[0], 16.0, 1e-5), "a +Y file stores its first row at the bottom");
        const std::vector<u8> mirrored = flatFile({{1.0f, 0.0f, 0.0f}}, 4, "-Y 1 -X 4");
        check(fmt::decodeRadianceHdr(mirrored.data(), mirrored.size(), c, &err) && c.width == 4, "a -X file decodes");
    }
    {
        fmt::PanoramaImage img;
        std::string err;
        const std::vector<u8> junk = {'n', 'o', 'p', 'e', '\n'};
        check(!fmt::decodeRadianceHdr(junk.data(), junk.size(), img, &err) && !err.empty(), "a file without the #? signature is refused with a reason");
        std::vector<u8> cut = flatFile({{1, 1, 1}, {1, 1, 1}}, 8);
        cut.resize(cut.size() - 10);
        check(!fmt::decodeRadianceHdr(cut.data(), cut.size(), img, &err), "a cut-short file is refused");
        const std::vector<u8> xyz = header("-Y 1 +X 1", "32-bit_rle_xyze");
        check(!fmt::decodeRadianceHdr(xyz.data(), xyz.size(), img, &err), "the XYZE format is refused");
        const std::vector<u8> bad = header("garbage");
        check(!fmt::decodeRadianceHdr(bad.data(), bad.size(), img, &err), "an unreadable resolution line is refused");
        check(!fmt::loadPanorama("no/such/file.hdr", img, &err) && !err.empty(), "a missing file is refused");
        check(!fmt::loadPanorama("sky.exr", img, &err) && err.find("EXR") != std::string::npos, "OpenEXR is named as unsupported");
    }

    AVER_INFO("-- the dome fit --");
    {
        // A uniform sky of 0.5 at energy 2 is a uniform 1.0: every colour 1.0 once encoded, the floor as bright as the sky.
        const std::vector<std::vector<float>> uni(16, {0.5f, 0.5f, 0.5f});
        fmt::SkyDomeFit fit;
        check(fmt::fitSkyDome(image(8, 16, uni), 2.0, fit), "a uniform panorama fits");
        check(near(fit.zenith[0], 1.0, 1e-9) && near(fit.horizon[1], 1.0, 1e-9) && near(fit.groundAlbedo[2], 1.0, 1e-9), "zenith, horizon and ground albedo are 1 (energy 2 doubles 0.5)");
        check(near(fit.domeExponent, 0.65, 1e-9), "a flat sky keeps the default exponent");
        check(near(fit.zenith[0], std::pow(1.0, 1.0 / 2.2), 1e-9), "and colours are encoded with the 2.2 power");
    }
    {
        // Upper half 2.0, lower half 0.5: the floor reflects a quarter of what the sky gives.
        std::vector<std::vector<float>> rows;
        for (int y = 0; y < 16; ++y) rows.push_back(y < 8 ? std::vector<float>{2.0f, 2.0f, 2.0f} : std::vector<float>{0.5f, 0.5f, 0.5f});
        fmt::SkyDomeFit fit;
        check(fmt::fitSkyDome(image(8, 16, rows), 1.0, fit), "a two-level panorama fits");
        check(near(fit.groundAlbedo[0], 0.25, 1e-9), "ground albedo is the lower hemisphere over the upper: 0.5 / 2");
        check(near(fit.zenith[1], std::pow(2.0, 1.0 / 2.2), 1e-9), "the zenith is the upper level, encoded");
    }
    {
        // A dome built from the model itself: C(e) = H + (Z - H) * pow(sin e / 2 + 0.5, 2), H 0.2, Z 3. The fit recovers it.
        const double H = 0.2, Z = 3.0, K = 2.0;
        const int h = 128;
        std::vector<std::vector<float>> rows;
        for (int y = 0; y < h; ++y) {
            const double s = std::cos((y + 0.5) / h * 3.14159265358979323846);
            const double c = s >= 0.0 ? H + (Z - H) * std::pow(s * 0.5 + 0.5, K) : 0.1;
            rows.push_back({static_cast<float>(c), static_cast<float>(c), static_cast<float>(c)});
        }
        fmt::SkyDomeFit fit;
        check(fmt::fitSkyDome(image(4, h, rows), 1.0, fit), "a model-built panorama fits");
        check(near(fit.domeExponent, K, 0.15), "the exponent is recovered (" + std::to_string(fit.domeExponent) + ")");
        check(near(std::pow(fit.zenith[1], 2.2), Z, 0.15) && near(std::pow(fit.horizon[1], 2.2), H, 0.06), "so are the zenith and the colour straight down");
    }
    {
        fmt::SkyDomeFit fit;
        check(!fmt::fitSkyDome(fmt::PanoramaImage{}, 1.0, fit), "an empty panorama is refused");
    }

    AVER_INFO("-- Godot's parameters --");
    {
        namespace ge = fmt::godotenv;
        check(ge::tonemapMode(0) == 3 && ge::tonemapMode(3) == 6 && ge::tonemapMode(4) == 7 && ge::tonemapMode(-1) == 3 && ge::tonemapMode(12) == 7, "Godot's tonemap_mode m is Aver's 3 + m (clamped)");
        check(near(ge::scaleAuthoredColour(0.5, 1.0), 0.5, 1e-12) && near(ge::scaleAuthoredColour(0.5, 0.0), 0.0, 1e-12) && near(std::pow(ge::scaleAuthoredColour(0.5, 4.0), 2.2), std::pow(0.5, 2.2) * 4.0, 1e-9),
              "an energy multiplier scales a colour in linear light");
        check(near(ge::exposure(2.0, 0.5), 1.0, 1e-12) && near(ge::exposure(-1.0, 2.0), 0.0, 1e-12), "exposure is tonemap_exposure times the camera multiplier, never negative");
        check(near(ge::fogDensityPerCm(0.01), 0.0001, 1e-12) && near(ge::heightFog(3.0, -0.5).falloffPerCm, 0.005, 1e-12) && near(ge::heightFog(3.0, 0.5).heightCm, 300.0, 1e-9), "fog density and height fog units");
        const ge::Dof p = ge::dofFromPractical(false, true, 2.0, 8.0, 0.2);
        check(p.on && near(p.focusCm, 800.0, 1e-9) && near(p.fStop, 2.0, 1e-9) && near(p.focalMm, 35.0, 1e-9), "a far blur is a lens focused at the far distance, f/2 at amount 0.2");
        check(!ge::dofFromPractical(false, false, 2.0, 8.0, 0.1).on && ge::dofFromPractical(true, false, 2.0, 8.0, 0.1).focusCm == 200.0, "no blur band is no DOF; a near-only blur focuses at the near distance");
        const ge::Dof q = ge::dofFromPhysical(5.0, 50.0, 2.8);
        check(q.on && near(q.focusCm, 500.0, 1e-9) && near(q.fStop, 2.8, 1e-9) && near(q.focalMm, 50.0, 1e-9), "a physical lens maps focus, aperture and focal length straight across");
    }

    AVER_INFO("-- the project keys --");
    {
        fmt::ProjectDesc p;
        std::string err;
        check(fmt::parseOcproject("OCPROJECT 1\nNAME P\nRENDER.TONEMAP 6\nRENDER.TONEMAPWHITE 4\nRENDER.ADJUSTBRIGHTNESS 1.25\nRENDER.ADJUSTCONTRAST 0.9\nRENDER.ADJUSTSATURATION 0\n", p, &err),
              "the tonemap white and the three adjustments parse: " + err);
        check(p.postTonemap == 6 && near(p.postTonemapWhite, 4.0, 1e-6) && near(p.postAdjustBrightness, 1.25, 1e-6) && near(p.postAdjustContrast, 0.9, 1e-6) &&
                  near(p.postAdjustSaturation, 0.0, 1e-9),
              "with their values (saturation 0 is a stated value)");
        check(p.hasRenderSettings(), "and they count as stated render settings");
        const std::string text = fmt::writeOcproject(p, "");
        fmt::ProjectDesc q;
        check(fmt::parseOcproject(text, q, &err) && q.postTonemap == 6 && near(q.postAdjustSaturation, 0.0, 1e-9) && fmt::writeOcproject(q, text) == text,
              "they write and parse back, and a second write is byte-identical");
        fmt::ProjectDesc none;
        check(fmt::parseOcproject("OCPROJECT 1\nNAME P\n", none, &err) && none.postTonemapWhite < 0.0f && none.postAdjustBrightness < 0.0f && !none.hasRenderSettings() &&
                  fmt::writeOcproject(none, "").find("RENDER.ADJUST") == std::string::npos && fmt::writeOcproject(none, "").find("RENDER.TONEMAP") == std::string::npos,
              "a project that states none writes none of them");
    }

    AVER_INFO("PanoramaSkyTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
