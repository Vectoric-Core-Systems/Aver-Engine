// Runs the `averdesign` tool as a process and turns its JSON into an ActorScript and class list.
#include "aver/formats/AverDesign.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/Json.hpp"

#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace aver::fmt {
namespace {

std::string g_exePath;          // empty means "search the PATH"
int  g_available = -1;          // -1 unknown, 0 no, 1 yes

#ifdef _WIN32
// UTF-8 to UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

// Runs a command line and captures stdout and stderr. Two pipe hazards, both handled: the read end
// must NOT be inheritable or the drain never sees EOF, and stdin must be NUL rather than null or a
// child that reads stdin blocks forever.
bool runCapture(const std::wstring& cmdline, std::string& out, int& exitCode) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;   // CreateProcessW may write into its command line
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }

    out.clear();
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    exitCode = static_cast<int>(code);
    return true;
}

// A quoted command-line argument, so a path with spaces stays one argument.
std::wstring quoted(const std::string& s) { return L"\"" + widen(s) + L"\""; }
#endif

const char* kToolName = "averdesign.exe";

// The executable to launch: the configured path, or the bare name for a PATH search.
std::string resolveExe() {
    return g_exePath.empty() ? std::string(kToolName) : g_exePath;
}

// Classifies a base type name by suffix. The SAME rule the built-in scanner uses; the two backends
// must classify a file identically.
ActorKind kindFromBase(std::string_view base) {
    auto endsWith = [&](std::string_view suf) {
        return base.size() >= suf.size() && base.compare(base.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (endsWith("GameInstance"))     return ActorKind::GameInstance;
    if (endsWith("GameMode"))         return ActorKind::GameMode;
    if (endsWith("PlayerController")) return ActorKind::PlayerController;
    // "Controller" on its own, which this backend was MISSING while ActorScript.cpp:215 has it.
    // The comment above promises the two backends classify a file identically, and they did not:
    // a class named `FpsController` came back PlayerController from the built-in scanner and
    // Unknown from the Roslyn one, so which tools recognised it depended on which backend the
    // project happened to use. Kept BELOW the PlayerController line, since that is the longer
    // suffix and must win.
    if (endsWith("Controller"))       return ActorKind::PlayerController;
    if (endsWith("Character"))        return ActorKind::Character;
    if (endsWith("Pawn"))             return ActorKind::Pawn;
    if (endsWith("Actor"))            return ActorKind::Actor;
    return ActorKind::Unknown;
}

// Reads a [begin, end] pair into a span, leaving it alone if the key is not one.
void readSpan(const JsonValue& obj, std::string_view key, ActorValueSpan& into) {
    const JsonValue& a = obj[key];
    if (!a.isArray() || a.size() != 2) return;
    into.begin = static_cast<usize>(a[0].asInt());
    into.end   = static_cast<usize>(a[1].asInt());
}

// Reads a 3-element array into `into`, leaving it alone if the key is not one.
void readVec3(const JsonValue& obj, std::string_view key, f32 into[3]) {
    const JsonValue& a = obj[key];
    if (!a.isArray() || a.size() != 3) return;
    for (usize i = 0; i < 3; ++i) into[i] = a[i].asFloat(into[i]);
}

} // namespace

// Sets where the tool is, and forgets the cached availability answer.
void setAverDesignPath(std::string exePath) {
    if (exePath == g_exePath) return;
    g_exePath = std::move(exePath);
    g_available = -1;
}

// Where the tool is, as last set.
const std::string& averDesignPath() { return g_exePath; }

// True when the tool can run at all. Cached; `recheck` forces a fresh probe.
bool averDesignAvailable(bool recheck) {
    if (!recheck && g_available >= 0) return g_available == 1;
#ifdef _WIN32
    std::string out;
    int code = -1;
    const std::wstring cmd = quoted(resolveExe()) + L" --probe";
    const bool ran = runCapture(cmd, out, code);
    g_available = (ran && code == 0 && out.rfind("averdesign", 0) == 0) ? 1 : 0;
    if (g_available == 1)
        AVER_INFO("[averdesign] available: {}", resolveExe());
    else
        AVER_INFO("[averdesign] not available: '{}' {}; the built-in scanner is the only backend",
                  resolveExe(),
                  ran ? "did not answer as expected" : "could not be started");
#else
    // The spawn above is Win32 and there is no other platform in this tree to test a posix one
    // against, so this backend reports itself absent.
    g_available = 0;
#endif
    return g_available == 1;
}

// Parses `csPath` through the tool. False with `err` set on any failure.
bool parseActorFileRoslyn(const std::string& csPath, RoslynParse& out, std::string* err) {
    auto fail = [&](std::string why) { if (err) *err = std::move(why); return false; };
#ifndef _WIN32
    (void)csPath; (void)out;
    return fail("averdesign is only wired up on Windows in this build");
#else
    if (!averDesignAvailable()) return fail("averdesign is not available");

    std::string raw;
    int code = -1;
    if (!runCapture(quoted(resolveExe()) + L" " + quoted(csPath), raw, code))
        return fail("averdesign could not be started");
    if (code != 0)
        return fail("averdesign exited " + std::to_string(code) + ": " + raw);

    JsonValue doc;
    std::string why;
    if (!parseJson(raw, doc, &why))
        return fail("averdesign's output could not be read: " + why);
    if (!doc.isObject()) return fail("averdesign produced something that is not an object");

    // ---- the generated region -----------------------------------------------------------------
    out.script = ActorScript{};
    out.script.backend = ActorParserBackend::Roslyn;
    const std::string_view status = doc["status"].asString("Malformed");
    if      (status == "Ok")            out.script.status = ActorParseStatus::Ok;
    else if (status == "NoRegion")      out.script.status = ActorParseStatus::NoRegion;
    else if (status == "UnknownSchema") out.script.status = ActorParseStatus::UnknownSchema;
    else                                out.script.status = ActorParseStatus::Malformed;
    out.script.error       = std::string(doc["error"].asString());
    out.script.regionBegin = static_cast<usize>(doc["regionBegin"].asInt());
    out.script.regionEnd   = static_cast<usize>(doc["regionEnd"].asInt());

    const JsonValue& models = doc["models"];
    for (usize i = 0; i < models.size(); ++i) {
        const JsonValue& m = models[i];
        ActorModel a;
        // The id crosses as a STRING: it is a u64 and JSON numbers are doubles, so a numeric round
        // trip would change the one field every rewrite is matched by.
        const std::string_view idText = m["objectId"].asString("0");
        a.objectId = std::strtoull(std::string(idText).c_str(), nullptr, 10);
        a.property = std::string(m["property"].asString());
        a.meshPath = std::string(m["meshPath"].asString());
        a.material = std::string(m["material"].asString());
        readVec3(m, "pos", a.pos);
        readVec3(m, "rot", a.rot);
        readVec3(m, "scale", a.scale);
        a.begin = static_cast<usize>(m["begin"].asInt());
        a.end   = static_cast<usize>(m["end"].asInt());
        out.script.models.push_back(std::move(a));
    }

    // ---- what each class declares ---------------------------------------------------------------
    out.classes.clear();
    const JsonValue& classes = doc["classes"];
    for (usize i = 0; i < classes.size(); ++i) {
        const JsonValue& c = classes[i];
        ActorClassInfo k;
        k.className = std::string(c["className"].asString());
        k.typeName  = std::string(c["typeName"].asString());
        k.baseType  = std::string(c["baseType"].asString());
        k.kind      = kindFromBase(k.baseType);

        k.hasMesh  = c["hasMesh"].asBool();
        k.meshPath = std::string(c["meshPath"].asString());
        k.material = std::string(c["material"].asString());
        readSpan(c, "meshPathSpan", k.meshPathSpan);
        readSpan(c, "materialSpan", k.materialSpan);

        // An absent key means NOT STATED — zero is a legal height — which is what asFloat's
        // fallback preserves.
        k.capsuleHeight = c["capsuleHeight"].asFloat(0.0f);
        k.capsuleRadius = c["capsuleRadius"].asFloat(0.0f);
        k.eyeHeight     = c["eyeHeight"].asFloat(0.0f);
        readSpan(c, "capsuleHeightSpan", k.capsuleHeightSpan);
        readSpan(c, "capsuleRadiusSpan", k.capsuleRadiusSpan);
        readSpan(c, "eyeHeightSpan",     k.eyeHeightSpan);

        k.hasCamera     = c["hasCamera"].asBool();
        k.cameraFovDeg  = c["cameraFovDeg"].asFloat(0.0f);
        k.cameraNearCm  = c["cameraNearCm"].asFloat(0.0f);
        k.cameraFarCm   = c["cameraFarCm"].asFloat(0.0f);
        readSpan(c, "cameraFovSpan",  k.cameraSpan[0]);
        readSpan(c, "cameraNearSpan", k.cameraSpan[1]);
        readSpan(c, "cameraFarSpan",  k.cameraSpan[2]);

        k.hasPointLight      = c["hasPointLight"].asBool();
        k.lightIntensityLux  = c["lightIntensityLux"].asFloat(0.0f);
        k.lightRangeCm       = c["lightRangeCm"].asFloat(0.0f);
        readSpan(c, "lightIntensitySpan", k.lightSpan[0]);
        readSpan(c, "lightRangeSpan",     k.lightSpan[1]);

        out.classes.push_back(std::move(k));
    }
    return true;
#endif
}

} // namespace aver::fmt
