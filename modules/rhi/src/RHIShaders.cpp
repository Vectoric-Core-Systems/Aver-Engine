// The HLSL every GPU consumer shares: the constant-buffer layouts the backend uploads, the vertex
// structures it feeds, the colour-space helpers, the sky and the camera post.
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

namespace aver::rhi {

// Writes sunDirection from an elevation and an azimuth bearing, both in degrees.
void SkyAtmosphere::setSunAngles(f32 elevationDeg, f32 azimuthDeg) {
    constexpr f32 kDeg = 3.14159265358979f / 180.0f;
    const f32 el = elevationDeg * kDeg, az = azimuthDeg * kDeg;
    const f32 ce = std::cos(el);
    sunDirection[0] = ce * std::cos(az);
    sunDirection[1] = ce * std::sin(az);
    sunDirection[2] = std::sin(el);
}

// Reads sunDirection back as an elevation and an azimuth bearing, both in degrees.
void SkyAtmosphere::sunAngles(f32& elevationDeg, f32& azimuthDeg) const {
    constexpr f32 kRad = 180.0f / 3.14159265358979f;
    const f32 x = sunDirection[0], y = sunDirection[1], z = sunDirection[2];
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) { elevationDeg = 0.0f; azimuthDeg = 0.0f; return; }
    elevationDeg = std::asin(z / len) * kRad;
    azimuthDeg = std::atan2(y, x) * kRad;
}

// Converts a colour temperature in Kelvin to linear sRGB, normalised so the brightest channel is 1.
void blackbodySrgb(f32 kelvin, f32 outRgb[3]) {
    const f32 t = kelvin < 1000.0f ? 1000.0f : (kelvin > 15000.0f ? 15000.0f : kelvin);
    const f32 t2 = t * t;
    const f32 u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2) /
                  (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const f32 v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2) /
                  (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);
    const f32 d = 2.0f * u - 8.0f * v + 4.0f;
    const f32 x = 3.0f * u / d;
    const f32 y = 2.0f * v / d;
    const f32 z = 1.0f - x - y;

    const f32 Y = 1.0f;
    const f32 X = (y > 1e-6f) ? (Y / y) * x : 0.0f;
    const f32 Z = (y > 1e-6f) ? (Y / y) * z : 0.0f;

    f32 rgb[3] = {
         3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
         0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z,
    };
    f32 m = 0.0f;
    for (f32 c : rgb) if (c > m) m = c;
    for (int i = 0; i < 3; ++i) {
        const f32 c = m > 1e-6f ? rgb[i] / m : 1.0f;
        outRgb[i] = c < 0.0f ? 0.0f : c;
    }
}

namespace {

/// THE EMBEDDED COPY ABOVE IS THE ORACLE, NOT THE SOURCE, AND ONLY FOR AS LONG AS THIS MOVE TAKES.
//
// Moving four and a half thousand lines of HLSL out of C++ literals is a text move, and a text move
// has exactly one failure worth fearing: the file and the literal differ by something invisible --
// a trailing space, a lost blank line, a CRLF -- and the shader still compiles while quietly saying
// something else. Screenshots cannot see that reliably; the run-to-run noise floor in this engine is
// thousands of bytes.
//
// So the two are compared BYTE FOR BYTE at first use. If they match, the compiled shader is
// necessarily identical and no rendering test is needed for the extraction step at all. If they do
// not, this says which file and by how much, and falls back to the literal so the engine still runs
// while it is sorted out. Once every block is moved and this has been seen to pass, the literals and
// this function go, and shaderFile() is called directly.
const std::string& colorHlsl() {
    // No static of its own: shaderFile returns a reference into the loader's cache,
    // which reloadShaderFiles() clears. Caching here would outlive that and go stale.
    return shaderFile("color.hlsli");
}
} // namespace

// The HLSL below hardcodes these; the C++ side reads the constants. Neither can move alone.
static_assert(kMeshShaderTrisPerGroup == 64, "AVER_MS_TRIS in the prelude is written out as 64");
static_assert(kMeshGeometryConstantRegister == 5, "MeshCB in the prelude is written out as b5");
static_assert(kObjectConstantRegister == 1, "PerObject in the prelude is written out as b1");
static_assert(kObjectConstantDwords == 32,
              "PerObject below is 32 dwords: world 16, base colour 4, material 4, model 4, emissive 4");
static_assert(kClusterAmplificationGroupSize == 32, "AVER_MSC_GROUP in the cluster shader is written out as 32");
static_assert(kFeatureFrameConstantRegister == 4, "ClusterFrameCB in the cluster shader is written out as b4");

// The shared HLSL prelude: colour helpers, the constant-buffer layouts the backend uploads, the
// vertex structures, the sky and the plain surface shading. Read from shaders/, composed once and
// cached until the shader files change.
const char* sharedShaderPrelude() {
    // REBUILT WHEN THE SHADER FILES MOVE. A plain function-local static is initialised ONCE, which
    // made hot reload a convincing lie: the watcher fired, the cache dropped, the pipelines were
    // rebuilt -- and every one of them recompiled this same stale text, so a deliberately broken
    // shader produced no error and a correct picture. Keyed on the revision, like
    // voxiShaderPrelude() already keys on the material-graph one.
    static std::string s;
    static u64 built = ~0ull;
    if (built != shaderFileRevision()) {
        s = colorHlsl() + shaderFile("shared_prelude.hlsl");
        built = shaderFileRevision();
    }
    return s.c_str();
}

// Builds the -D list that pins the prelude's mesh-geometry SRV registers past a layout's own.
std::string meshGeometryDefines(const PipelineLayout& layout) {
    const u32 base = declaredSrvCount(layout);
    return "AVER_MS_VTX_REG=" + std::to_string(base) +
           ";AVER_MS_IDX_REG=" + std::to_string(base + 1);
}

// The camera post chain: bloom, auto-exposure and the composite. Same file-backed, revision-keyed
// caching as sharedShaderPrelude above.
const char* postShaderSource() {
    // REBUILT WHEN THE SHADER FILES MOVE. A plain function-local static is initialised ONCE, which
    // made hot reload a convincing lie: the watcher fired, the cache dropped, the pipelines were
    // rebuilt -- and every one of them recompiled this same stale text, so a deliberately broken
    // shader produced no error and a correct picture. Keyed on the revision, like
    // voxiShaderPrelude() already keys on the material-graph one.
    static std::string s;
    static u64 built = ~0ull;
    if (built != shaderFileRevision()) {
        s = colorHlsl() + shaderFile("post.hlsl");
        built = shaderFileRevision();
    }
    return s.c_str();
}

} // namespace aver::rhi
