#include "aver/formats/RenderJob.hpp"
#include "aver/core/JsonText.hpp"

#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdio>

namespace aver::fmt {

namespace {

template <class T> T clampTo(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string jsonString(const std::string& s) {
    return aver::jsonQuoted(s);
}

std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

} // namespace

const char* renderOutputName(RenderOutputKind k) {
    switch (k) {
    case RenderOutputKind::Exr: return "exr";
    case RenderOutputKind::Mp4: return "mp4";
    default: return "png";
    }
}

RenderJob& RenderJob::normalise() {
    width = clampTo<u32>(width, 16, 16384);
    height = clampTo<u32>(height, 16, 16384);
    if (output == RenderOutputKind::Mp4) {   // the H.264 encoder takes even dimensions
        width &= ~1u;
        height &= ~1u;
    }
    frameStart = std::max(0, frameStart);
    if (frameEnd >= 0 && frameEnd < frameStart) frameEnd = frameStart;
    fps = (fps > 0.0f) ? clampTo(fps, 1.0f, 1000.0f) : 0.0f;
    warmupFrames = clampTo(warmupFrames, 0, 100000);
    settleFrames = clampTo(settleFrames, 0, 1000);
    temporalSamples = clampTo(temporalSamples, 1, 64);
    shutter = clampTo(shutter, 0.0f, 1.0f);
    ptSamples = clampTo(ptSamples, 1, 1600);   // PtSceneView stops accumulating at 1600
    bitrateKbps = clampTo(bitrateKbps, 500, 200000);
    if (name.empty()) name = "seq";
    return *this;
}

bool parseRenderJob(std::string_view text, RenderJob& out, std::string* why) {
    JsonValue root;
    if (!parseJson(text, root, why)) return false;
    if (!root.isObject()) {
        if (why) *why = "a render job is a JSON object";
        return false;
    }
    RenderJob j;
    const auto str = [&](const JsonValue& v, const char* k, std::string& dst) {
        if (v[k].isString()) dst = std::string(v[k].asString());
    };
    const auto i32v = [&](const JsonValue& v, const char* k, i32& dst) {
        if (v[k].isNumber()) dst = static_cast<i32>(v[k].asInt());
    };
    str(root, "level", j.level);
    str(root, "sequence", j.sequence);
    i32v(root, "frameStart", j.frameStart);
    i32v(root, "frameEnd", j.frameEnd);
    if (root["width"].isNumber()) j.width = static_cast<u32>(std::max<i64>(0, root["width"].asInt()));
    if (root["height"].isNumber()) j.height = static_cast<u32>(std::max<i64>(0, root["height"].asInt()));
    if (root["fps"].isNumber()) j.fps = root["fps"].asFloat();
    i32v(root, "warmupFrames", j.warmupFrames);
    i32v(root, "settleFrames", j.settleFrames);
    i32v(root, "temporalSamples", j.temporalSamples);
    if (root["shutter"].isNumber()) j.shutter = root["shutter"].asFloat();
    i32v(root, "ptSamples", j.ptSamples);

    const JsonValue& o = root["output"];
    if (o.isObject()) {
        const std::string_view kind = o["kind"].asString("png");
        if (kind == "png") j.output = RenderOutputKind::Png;
        else if (kind == "exr") j.output = RenderOutputKind::Exr;
        else if (kind == "mp4") j.output = RenderOutputKind::Mp4;
        else {
            if (why) *why = "output.kind must be png, exr or mp4";
            return false;
        }
        str(o, "dir", j.dir);
        str(o, "name", j.name);
        if (o["exrFloat"].isBool()) j.exrFloat = o["exrFloat"].asBool();
        i32v(o, "bitrateKbps", j.bitrateKbps);
    }
    out = j.normalise();
    return true;
}

bool loadRenderJob(const std::string& path, RenderJob& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) {
        if (why) *why = "could not read " + path;
        return false;
    }
    return parseRenderJob(text, out, why);
}

std::string serialiseRenderJob(const RenderJob& j) {
    std::string s = "{\n";
    s += "  \"level\": " + jsonString(j.level) + ",\n";
    s += "  \"sequence\": " + jsonString(j.sequence) + ",\n";
    s += "  \"frameStart\": " + std::to_string(j.frameStart) + ",\n";
    s += "  \"frameEnd\": " + std::to_string(j.frameEnd) + ",\n";
    s += "  \"width\": " + std::to_string(j.width) + ",\n";
    s += "  \"height\": " + std::to_string(j.height) + ",\n";
    s += "  \"fps\": " + num(j.fps) + ",\n";
    s += "  \"warmupFrames\": " + std::to_string(j.warmupFrames) + ",\n";
    s += "  \"settleFrames\": " + std::to_string(j.settleFrames) + ",\n";
    s += "  \"temporalSamples\": " + std::to_string(j.temporalSamples) + ",\n";
    s += "  \"shutter\": " + num(j.shutter) + ",\n";
    s += "  \"ptSamples\": " + std::to_string(j.ptSamples) + ",\n";
    s += "  \"output\": {\n";
    s += std::string("    \"kind\": \"") + renderOutputName(j.output) + "\",\n";
    s += "    \"dir\": " + jsonString(j.dir) + ",\n";
    s += "    \"name\": " + jsonString(j.name) + ",\n";
    s += std::string("    \"exrFloat\": ") + (j.exrFloat ? "true" : "false") + ",\n";
    s += "    \"bitrateKbps\": " + std::to_string(j.bitrateKbps) + "\n";
    s += "  }\n}\n";
    return s;
}

bool saveRenderJob(const std::string& path, const RenderJob& job, std::string* why) {
    if (!writeFileTextAtomic(path, serialiseRenderJob(job))) {
        if (why) *why = "could not write " + path;
        return false;
    }
    return true;
}

} // namespace aver::fmt
