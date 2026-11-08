// TraceCaptureTest -- timeline capture: armed/running lifecycle, scopes on several threads, ring
// overwrite, GPU span layout, and that the written file is well-formed Chrome trace JSON.
#include "aver/core/Log.hpp"
#include "aver/core/TraceCapture.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) AVER_INFO("   PASS  {}", what);
    else      { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

// A structural JSON checker: balanced containers, valid strings and literals. Not a value extractor.
struct JsonCheck {
    const std::string& s; size_t i = 0;
    explicit JsonCheck(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool str() {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') { ++i; continue; }
            if (s[i] == '"') { ++i; return true; }
            if (static_cast<unsigned char>(s[i]) < 0x20) return false;
        }
        return false;
    }
    bool val() {
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '{') {
            ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws(); if (!str()) return false; ws();
                if (i >= s.size() || s[i++] != ':') return false;
                if (!val()) return false; ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                return i < s.size() && s[i++] == '}';
            }
        }
        if (s[i] == '[') {
            ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                if (!val()) return false; ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                return i < s.size() && s[i++] == ']';
            }
        }
        if (s[i] == '"') return str();
        for (const char* lit : {"true", "false", "null"}) {
            const size_t n = std::string(lit).size();
            if (s.compare(i, n, lit) == 0) { i += n; return true; }
        }
        const size_t st = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '.' || s[i] == 'e' || s[i] == '+')) ++i;
        return i > st;
    }
    bool ok() { if (!val()) return false; ws(); return i == s.size(); }
};

static size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

int main() {
    const fs::path out = fs::temp_directory_path() / "aver-trace-test.json";
    std::error_code ec;
    fs::remove(out, ec);

    check(!trace::capturing(), "idle by default");
    { AVER_TRACE_SCOPE("idle"); }
    trace::beginFrame(); trace::endFrame();
    check(!trace::capturing() && trace::lastWrittenPath().empty(), "frame calls with nothing armed do nothing");
    { const std::string e = trace::toChromeJson(); JsonCheck j(e); check(j.ok(), "an empty capture serialises to valid JSON"); }

    check(trace::begin(3, out.string()), "begin arms a capture");
    check(!trace::begin(2, out.string()), "a second begin while armed is refused");
    check(trace::armed() && !trace::capturing(), "armed, not yet recording");
    trace::setThreadName("main");
    for (int f = 0; f < 3; ++f) {
        trace::beginFrame();
        check(trace::capturing(), "recording inside a frame");
        {
            AVER_TRACE_SCOPE("update");
            { AVER_TRACE_SCOPE("inner \"quoted\" \\ name"); }
        }
        std::thread t([] { AVER_TRACE_SCOPE("worker"); });
        t.join();
        if (f == 1) {
            std::vector<trace::GpuSpan> gpu;
            gpu.push_back({"Shadows", 1.0, 0xFFFFFFFFu});
            gpu.push_back({"Cascade0", 0.4, 0});
            gpu.push_back({"Lighting", 2.0, 0xFFFFFFFFu});
            trace::addGpuReport(gpu, trace::detail::nowUs());
        }
        trace::endFrame();
    }
    check(!trace::capturing() && !trace::armed(), "capture ends after the requested frames");
    check(trace::framesRecorded() == 3, "three frames recorded");
    check(trace::lastWrittenPath() == out.string(), "the file path is reported");

    const std::string json = slurp(out);
    check(!json.empty(), "the trace file was written");
    JsonCheck jc(json);
    check(jc.ok(), "the trace file is well-formed JSON");
    check(json.find("\"traceEvents\"") != std::string::npos, "has a traceEvents array");
    check(count(json, "\"name\":\"Frame\"") == 3, "one Frame event per frame");
    check(count(json, "\"name\":\"update\"") == 3, "CPU scope recorded every frame");
    check(count(json, "\"name\":\"worker\"") == 3, "scopes from another thread are recorded");
    check(json.find("inner \\\"quoted\\\" \\\\ name") != std::string::npos, "names are escaped");
    check(count(json, "\"cat\":\"gpu\"") == 3 && json.find("\"name\":\"Cascade0\"") != std::string::npos, "GPU spans appear");
    check(json.find("\"ringWrapped\":false") != std::string::npos, "no wrap on a small capture");

    auto tsOf = [&](const std::string& name) {
        const size_t p = json.find("\"name\":\"" + name + "\"");
        const size_t t = json.find("\"ts\":", p);
        return std::stoull(json.substr(t + 5));
    };
    check(tsOf("Cascade0") == tsOf("Shadows"), "a child span starts with its parent");
    check(tsOf("Lighting") - tsOf("Shadows") == 1000, "top-level GPU spans are laid out back to back (1.0 ms)");

    // Ring overwrite: capacity 256 (the floor), far more events than that.
    fs::remove(out, ec);
    check(trace::begin(1, out.string(), 256), "small ring capture armed");
    trace::beginFrame();
    for (int i = 0; i < 2000; ++i) { AVER_TRACE_SCOPE("spin"); }
    trace::endFrame();
    const std::string j2 = slurp(out);
    JsonCheck jc2(j2);
    check(jc2.ok(), "wrapped capture is still valid JSON");
    check(j2.find("\"ringWrapped\":true") != std::string::npos, "wrap is reported");
    check(count(j2, "\"ph\":\"X\"") <= 256, "no more events than the ring holds");

    // stop() writes a partial capture.
    fs::remove(out, ec);
    check(trace::begin(100, out.string()), "long capture armed");
    trace::beginFrame(); { AVER_TRACE_SCOPE("partial"); } trace::endFrame();
    check(trace::stop(), "stop ends a running capture");
    check(!trace::stop(), "stop with nothing active reports so");
    check(fs::exists(out), "stop wrote the partial file");

    fs::remove(out, ec);
    AVER_INFO("[trace-test] RESULT: {}", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
