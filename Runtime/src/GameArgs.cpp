#include "GameAppInternal.hpp"

namespace aver::game {

namespace app_detail {

// ASCII case-insensitive equality, for the record scanner below and the one token it reads.
bool equalsAsciiCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

} // namespace app_detail

namespace {

// Reads the value that follows a flag, or returns the fallback. Bounds-checked so a trailing flag
// with no value is ignored rather than reading past argv.
const char* valueAfter(int argc, char** argv, int i, const char* fallback) {
    return (i + 1 < argc) ? argv[i + 1] : fallback;
}

u32 parseU32(const char* s, u32 fallback) {
    if (!s || !*s) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || v == 0) return fallback;
    return static_cast<u32>(v);
}

// Check for .ocproject file extension (case-insensitive)
bool isOcproject(const char* p) {
    const usize n = std::strlen(p);
    if (n < 11) return false;
    const char* ext = p + n - 10;
    static const char* kExt = ".ocproject";
    for (int i = 0; i < 10; ++i) {
        char a = ext[i], b = kExt[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Check for .ocworld or .ocmap file extension (case-insensitive)
bool isLevelFile(const char* p) {
    const std::string_view s(p);
    const auto endsWithCI = [&](std::string_view ext) {
        return s.size() > ext.size() && equalsAsciiCI(s.substr(s.size() - ext.size()), ext);
    };
    return endsWithCI(".ocworld") || endsWithCI(".ocmap");
}

// Find owning .ocproject by walking up max 8 directories
std::string ownerProjectOf(const std::string& mapPath) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(mapPath).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up) {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (!it->is_directory(ec) && isOcproject(it->path().string().c_str()))
                return it->path().string();
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

} // namespace

GameConfig parseArgs(int argc, char** argv) {
    GameConfig c;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--frames") == 0)      { c.maxFrames = parseU32(valueAfter(argc, argv, i, nullptr), 0); ++i; }
        else if (std::strcmp(a, "--width") == 0)       { c.width  = parseU32(valueAfter(argc, argv, i, nullptr), c.width);  ++i; }
        else if (std::strcmp(a, "--height") == 0)      { c.height = parseU32(valueAfter(argc, argv, i, nullptr), c.height); ++i; }
        else if (std::strcmp(a, "--backend") == 0)     { c.backend = valueAfter(argc, argv, i, ""); ++i; }
        else if (std::strcmp(a, "--title") == 0)       { c.title   = valueAfter(argc, argv, i, c.title.c_str()); ++i; }
        else if (std::strcmp(a, "--headless") == 0)    { c.headless = true; }
        else if (std::strcmp(a, "--warp") == 0)        { c.useWarp = true; }
        else if (std::strcmp(a, "--debug-layer") == 0) { c.debugLayer = true; }
        else if (std::strcmp(a, "--project") == 0)     { c.projectPath = valueAfter(argc, argv, i, ""); ++i; }
        else if (std::strcmp(a, "--culture") == 0)     { c.culture = valueAfter(argc, argv, i, ""); ++i; }
        else if (std::strcmp(a, "--input-echo") == 0)  { c.inputEcho = true; }
        else if (std::strcmp(a, "--trace-opens") == 0) { c.traceOpens = true; }
        // Prints scene contents for verification (both hosts match)
        else if (std::strcmp(a, "--scene-census") == 0) { c.sceneCensus = true; }
        // Stats interval, optional. Consumed only if it parses as a number.
        else if (std::strcmp(a, "--stats") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            const f32 secs = v ? static_cast<f32>(std::atof(v)) : 0.0f;
            if (secs > 0.0f) { c.statsIntervalSec = secs; ++i; }
            else             { c.statsIntervalSec = 5.0f; }
        }
        else if (std::strcmp(a, "--pcg-volume-test") == 0) { c.pcgVolumeTest = true; }
        // Chunk streaming with optional frame count
        else if (std::strcmp(a, "--chunk-stream") == 0) {
            c.chunkStreamAutoFrames = (i + 1 < argc && argv[i + 1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (std::strcmp(a, "--no-chunk-stream") == 0) { c.chunkStreamAutoFrames = 0; }
        else if (std::strcmp(a, "--no-particle-gi") == 0)  { c.noParticleGi = true; }
        else if (std::strcmp(a, "--particle-test") == 0)   { c.particleTest = true; }

        else if (std::strcmp(a, "--particles-cpu") == 0)   { c.particleGpuArg = 0; }
        else if (std::strcmp(a, "--particles-gpu") == 0)   { c.particleGpuArg = 2; }
        else if (std::strcmp(a, "--screenshot") == 0)      { c.screenshotPath = valueAfter(argc, argv, i, ""); ++i; }
        // Disable VSync for measurement parity
        else if (std::strcmp(a, "--no-vsync") == 0) { c.vsyncOff = true; }
        // See GameConfig::noMouseCapture's own comment.
        else if (std::strcmp(a, "--no-mouse-capture") == 0) { c.noMouseCapture = true; }
        // Camera wobble: DEG and PERIOD both required
        else if (std::strcmp(a, "--cam-wobble") == 0 && i + 2 < argc) {
            c.camWobbleDeg = static_cast<f32>(std::atof(argv[i + 1]));
            const int period = std::atoi(argv[i + 2]);
            c.camWobblePeriod = period > 0 ? static_cast<u32>(period) : 0u;
            i += 2;
        }
        // AverSR quality level selection
        else if (std::strcmp(a, "--aversr") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if      (v && equalsAsciiCI(v, "off"))         { c.averSrArg = 0;  ++i; }
            else if (v && equalsAsciiCI(v, "quality"))     { c.averSrArg = 1;  ++i; }
            else if (v && equalsAsciiCI(v, "balanced"))    { c.averSrArg = 2;  ++i; }
            else if (v && equalsAsciiCI(v, "performance")) { c.averSrArg = 3;  ++i; }
            else if (v && equalsAsciiCI(v, "auto"))        { c.averSrArg = -1; ++i; }
        }
        // Temporal AA + upscale on/off; --no-taa leaves FSR 1 (the editor's flags, same meaning)
        else if (std::strcmp(a, "--taa") == 0)    { c.taaArg = 1; }
        else if (std::strcmp(a, "--no-taa") == 0) { c.taaArg = 0; }
        // Override frame interpolation setting
        else if (std::strcmp(a, "--frame-interp") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if (v) { c.frameInterpArg = std::atoi(v) != 0 ? 1 : 0; ++i; }
        }
        else if (std::strcmp(a, "--frame-interp-trajectory") == 0) {
            const char* v = valueAfter(argc, argv, i, nullptr);
            if (v) {
                c.frameInterpTrajectory = equalsAsciiCI(v, "quadratic") ? 1 : equalsAsciiCI(v, "neural") ? 2 : 0;
                ++i;
            }
        }
        // Bare .ocproject or .ocworld path is recognized
        else if (isOcproject(a))                       { c.projectPath = a; }
        // Opens level instead of start map
        else if (isLevelFile(a))                       { c.levelPath = a; }
        // Anything else is deliberately ignored: see the header.
    }
    // Level path implies its project (explicit --project wins)
    if (!c.levelPath.empty() && c.projectPath.empty()) {
        c.projectPath = ownerProjectOf(c.levelPath);
        if (c.projectPath.empty())
            AVER_WARN("[Game] '{}' is not inside a project (no .ocproject above it); its "
                      "placements will not resolve", c.levelPath);
    }
    return c;
}

} // namespace aver::game
