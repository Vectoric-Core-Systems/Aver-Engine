// .ocvehicle reader, writer and validator. The grammar is in OcVehicle.hpp.
//
// THE PARSER IS FORGIVING ABOUT WHAT IT DOES NOT KNOW and strict about what it does: an unknown record or key
// is skipped (one warning for the file, so a file from a newer build still loads its known parts), a newer
// version is refused, and a number that is not a number refuses the file with the line it is on. The writer emits
// the shortest text that reads back to the same float, in a fixed record order, so write(parse(write(s))) is
// write(s) byte for byte.
#include "aver/vehicle/OcVehicle.hpp"

#include "aver/core/CharConv.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/detail/TextScan.hpp"

#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace aver::vehicle {

using namespace aver::fmt::detail;

namespace {

// ---- numbers ----------------------------------------------------------------------------------------

bool parseF(std::string_view s, f32& out) {
    s = trim(s);
    if (s.empty()) return false;
    f32 v{};
    const auto r = aver::fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool isNumber(std::string_view s) { f32 v; return parseF(s, v); }

// The shortest decimal text that reads back to exactly this float (6 to 9 significant digits).
std::string num(f32 v) {
    if (v == 0.0f) return "0";
    char buf[48];
    for (int p = 6; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, static_cast<f64>(v));
        f32 back{};
        if (parseF(buf, back) && back == v) break;
    }
    return buf;
}

bool eq(std::string_view a, const char* b) { return equalsCI(a, b); }

// ---- parsing ----------------------------------------------------------------------------------------

struct Ctx {
    std::string err;
    usize line = 0;
    u32 unknownKeys = 0, unknownRecords = 0;
};

bool bad(Ctx& c, const char* rec, const std::string& msg) {
    c.err = "line " + std::to_string(c.line) + ": " + rec + ": " + msg;
    return false;
}

struct Args {
    const std::vector<std::string_view>& t;
    usize i;
    Ctx& c;
    const char* rec;

    bool more() const { return i < t.size(); }
    std::string_view next() { return t[i++]; }
    bool nextIsNumber() const { return i < t.size() && isNumber(t[i]); }

    bool f(f32& out) {
        if (i >= t.size()) return bad(c, rec, "a value is missing");
        if (!parseF(t[i], out)) return bad(c, rec, "'" + std::string(t[i]) + "' is not a number");
        ++i;
        return true;
    }
    bool u(u32& out) {
        f32 v{};
        if (!f(v)) return false;
        if (v < 0.0f || v != std::floor(v) || v > 1.0e6f) return bad(c, rec, "expected a whole number, got " + num(v));
        out = static_cast<u32>(v);
        return true;
    }
    bool flag(bool& out) {
        f32 v{};
        if (!f(v)) return false;
        out = v != 0.0f;
        return true;
    }
    bool vec3(Vec3& out) { return f(out.x) && f(out.y) && f(out.z); }
    bool word(std::string& out) {
        if (i >= t.size()) return bad(c, rec, "a value is missing");
        out = std::string(t[i++]);
        return true;
    }
    // Numbers until the next token that is not one, as pairs.
    bool pairs(std::vector<Vec2>& out) {
        std::vector<f32> v;
        while (nextIsNumber()) { f32 x{}; if (!f(x)) return false; v.push_back(x); }
        if (v.size() % 2 != 0) return bad(c, rec, "points come in pairs");
        for (usize k = 0; k < v.size(); k += 2) out.push_back(Vec2(v[k], v[k + 1]));
        return true;
    }
    bool list(std::vector<f32>& out) {
        while (nextIsNumber()) { f32 x{}; if (!f(x)) return false; out.push_back(x); }
        return true;
    }
    void unknown() { ++c.unknownKeys; }
};

bool recBody(Args a, VehicleSpec& s) {
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "half")) { if (!a.vec3(s.body.half)) return false; }
        else if (eq(k, "clearance")) { if (!a.f(s.body.clearance)) return false; }
        else if (eq(k, "com")) { if (!a.vec3(s.body.com)) return false; }
        else if (eq(k, "mass")) { if (!a.f(s.body.massKg)) return false; }
        else if (eq(k, "pitchroll")) { if (!a.f(s.body.pitchRollDeg)) return false; }
        else a.unknown();
    }
    return true;
}

bool recEngine(Args a, VehicleSpec& s) {
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "torque")) { if (!a.f(s.engine.torque)) return false; }
        else if (eq(k, "minrpm")) { if (!a.f(s.engine.minRpm)) return false; }
        else if (eq(k, "maxrpm")) { if (!a.f(s.engine.maxRpm)) return false; }
        else if (eq(k, "inertia")) { if (!a.f(s.engine.inertia)) return false; }
        else if (eq(k, "damping")) { if (!a.f(s.engine.damping)) return false; }
        else a.unknown();
    }
    return true;
}

bool recGearbox(Args a, VehicleSpec& s) {
    GearboxDef& g = s.gearbox;
    g.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "auto")) g.manual = false;
        else if (eq(k, "manual")) g.manual = true;
        else if (eq(k, "forward")) { g.forward.clear(); if (!a.list(g.forward)) return false; }
        else if (eq(k, "reverse")) { g.reverse.clear(); if (!a.list(g.reverse)) return false; }
        else if (eq(k, "switch")) { if (!a.f(g.switchTime)) return false; }
        else if (eq(k, "clutchrelease")) { if (!a.f(g.clutchRelease)) return false; }
        else if (eq(k, "latency")) { if (!a.f(g.latency)) return false; }
        else if (eq(k, "up")) { if (!a.f(g.up)) return false; }
        else if (eq(k, "down")) { if (!a.f(g.down)) return false; }
        else if (eq(k, "clutch")) { if (!a.f(g.clutch)) return false; }
        else a.unknown();
    }
    return true;
}

bool diffWord(Args& a, DiffMode& out) {
    std::string w;
    if (!a.word(w)) return false;
    if (eq(w, "open")) out = DiffMode::Open;
    else if (eq(w, "lsd")) out = DiffMode::LimitedSlip;
    else if (eq(w, "locked")) out = DiffMode::Locked;
    else return bad(a.c, a.rec, "'" + w + "' is not open, lsd or locked");
    return true;
}

bool recCenter(Args a, VehicleSpec& s) {
    s.centerPresent = true;
    if (!a.more()) return bad(a.c, a.rec, "open, lsd <ratio> or locked");
    const std::string_view k = a.next();
    if (eq(k, "open")) s.center = DiffMode::Open;
    else if (eq(k, "locked")) s.center = DiffMode::Locked;
    else if (eq(k, "lsd")) {
        s.center = DiffMode::LimitedSlip;
        if (a.nextIsNumber() && !a.f(s.centerLsd)) return false;
    } else return bad(a.c, a.rec, "'" + std::string(k) + "' is not open, lsd or locked");
    return true;
}

bool recAxle(Args a, VehicleSpec& s) {
    u32 idx = 0;
    if (!a.u(idx)) return false;
    if (idx >= kMaxWheels) return bad(a.c, a.rec, "axle index out of range");
    if (s.axles.size() <= idx) s.axles.resize(idx + 1);
    AxleDef& d = s.axles[idx];
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "final")) { if (!a.f(d.finalDrive)) return false; }
        else if (eq(k, "diff")) { if (!diffWord(a, d.diff)) return false; }
        else if (eq(k, "split")) { if (!a.f(d.split)) return false; }
        else if (eq(k, "lsd")) { if (!a.f(d.lsd)) return false; }
        else if (eq(k, "share")) { if (!a.f(d.share)) return false; }
        else if (eq(k, "antiroll")) { if (!a.f(d.antiroll)) return false; }
        else a.unknown();
    }
    return true;
}

bool recTyre(Args a, VehicleSpec& s) {
    TyreCurve t;
    if (!a.word(t.name)) return false;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "long")) { t.longitudinal.clear(); if (!a.pairs(t.longitudinal)) return false; }
        else if (eq(k, "lat")) { t.lateral.clear(); if (!a.pairs(t.lateral)) return false; }
        else a.unknown();
    }
    s.tyres.push_back(std::move(t));
    return true;
}

bool recWheelClass(Args a, VehicleSpec& s) {
    WheelClass w;
    if (!a.word(w.name)) return false;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "radius")) { if (!a.f(w.radius)) return false; }
        else if (eq(k, "width")) { if (!a.f(w.width)) return false; }
        else if (eq(k, "susp")) { if (!a.f(w.suspMin) || !a.f(w.suspMax)) return false; }
        else if (eq(k, "hz")) { if (!a.f(w.suspHz)) return false; }
        else if (eq(k, "damping")) { if (!a.f(w.suspDamping)) return false; }
        else if (eq(k, "brake")) { if (!a.f(w.brakeNm)) return false; }
        else if (eq(k, "handbrake")) { if (!a.f(w.handbrakeNm)) return false; }
        else if (eq(k, "inertia")) { if (!a.f(w.inertia)) return false; }
        else if (eq(k, "spindamping")) { if (!a.f(w.spinDamping)) return false; }
        else if (eq(k, "tyre")) { if (!a.word(w.tyre)) return false; }
        else a.unknown();
    }
    s.classes.push_back(std::move(w));
    return true;
}

bool recWheel(Args a, VehicleSpec& s) {
    WheelDef w;
    if (!a.word(w.name)) return false;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "class")) { if (!a.word(w.wheelClass)) return false; }
        else if (eq(k, "hub")) { if (!a.vec3(w.hub)) return false; }
        else if (eq(k, "steer")) { if (!a.f(w.steerDeg)) return false; }
        else if (eq(k, "driven")) { if (!a.flag(w.driven)) return false; }
        else if (eq(k, "handbrake")) { if (!a.flag(w.handbrake)) return false; }
        else if (eq(k, "brake")) { if (!a.f(w.brakeNm)) return false; }
        else if (eq(k, "node")) { if (!a.word(w.node)) return false; }
        else if (eq(k, "bone")) { if (!a.word(w.bone)) return false; }
        else a.unknown();
    }
    s.wheels.push_back(std::move(w));
    return true;
}

bool recTrack(Args a, VehicleSpec& s) {
    std::string side;
    if (!a.word(side)) return false;
    int idx = eq(side, "left") ? 0 : eq(side, "right") ? 1 : -1;
    if (idx < 0) return bad(a.c, a.rec, "'" + side + "' is not left or right");
    TrackDef& t = s.tracks[idx];
    t.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "wheels")) {
            t.wheels.clear();
            std::vector<f32> v;
            if (!a.list(v)) return false;
            for (f32 x : v) {
                if (x < 0.0f || x != std::floor(x)) return bad(a.c, a.rec, "a wheel index must be a whole number");
                t.wheels.push_back(static_cast<u32>(x));
            }
        }
        else if (eq(k, "driven")) { if (!a.u(t.driven)) return false; }
        else if (eq(k, "inertia")) { if (!a.f(t.inertia)) return false; }
        else if (eq(k, "damping")) { if (!a.f(t.damping)) return false; }
        else if (eq(k, "brake")) { if (!a.f(t.brakeNm)) return false; }
        else if (eq(k, "final")) { if (!a.f(t.finalDrive)) return false; }
        else if (eq(k, "steergain")) { if (!a.f(t.steerGain)) return false; }
        else if (eq(k, "pivot")) { if (!a.f(t.pivot)) return false; }
        else a.unknown();
    }
    return true;
}

bool recAssist(Args a, VehicleSpec& s) {
    AssistDef& d = s.assist;
    d.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "abs")) { if (!a.flag(d.abs)) return false; }
        else if (eq(k, "absslip")) { if (!a.f(d.absSlip)) return false; }
        else if (eq(k, "absrelease")) { if (!a.f(d.absRelease)) return false; }
        else if (eq(k, "tcs")) { if (!a.flag(d.tcs)) return false; }
        else if (eq(k, "tcsslip")) { if (!a.f(d.tcsSlip)) return false; }
        else if (eq(k, "steerin")) { if (!a.f(d.steerIn)) return false; }
        else if (eq(k, "steerout")) { if (!a.f(d.steerOut)) return false; }
        else if (eq(k, "throttlein")) { if (!a.f(d.throttleIn)) return false; }
        else if (eq(k, "throttleout")) { if (!a.f(d.throttleOut)) return false; }
        else if (eq(k, "brakein")) { if (!a.f(d.brakeIn)) return false; }
        else if (eq(k, "reversebrake")) { if (!a.flag(d.reverseBrake)) return false; }
        else if (eq(k, "reversespeed")) { if (!a.f(d.reverseSpeed)) return false; }
        else if (eq(k, "holdbrake")) { if (!a.f(d.holdBrake)) return false; }
        else if (eq(k, "holdspeed")) { if (!a.f(d.holdSpeed)) return false; }
        else if (eq(k, "skidslip")) { if (!a.f(d.skidSlip)) return false; }
        else if (eq(k, "skidangle")) { if (!a.f(d.skidAngle)) return false; }
        else a.unknown();
    }
    return true;
}

bool recAero(Args a, VehicleSpec& s) {
    s.aeroPresent = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "drag")) { if (!a.f(s.drag)) return false; }
        else if (eq(k, "downforce")) { if (!a.f(s.downforce)) return false; }
        else a.unknown();
    }
    return true;
}

bool recSeat(Args a, VehicleSpec& s) {
    s.seat.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "driver")) { if (!a.vec3(s.seat.driver)) return false; }
        else if (eq(k, "exit")) { if (!a.vec3(s.seat.exit)) return false; }
        else a.unknown();
    }
    return true;
}

bool audioKeyword(std::string_view k) {
    for (const char* w : {"engine", "idle", "high", "pitchlow", "pitchhigh", "skid", "impact", "shift"})
        if (eq(k, w)) return true;
    return false;
}

bool recAudio(Args a, VehicleSpec& s) {
    AudioDef& d = s.audio;
    d.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "engine")) { if (!a.word(d.idle)) return false; }
        else if (eq(k, "idle")) {
            if (!a.f(d.idleRpm)) return false;
            // The high loop's path follows the idle rpm.
            if (a.more() && !audioKeyword(a.t[a.i]) && !a.nextIsNumber() && !a.word(d.high)) return false;
        }
        else if (eq(k, "high")) {
            if (a.more() && !a.nextIsNumber() && !audioKeyword(a.t[a.i]) && !a.word(d.high)) return false;
            if (a.nextIsNumber() && !a.f(d.highRpm)) return false;
        }
        else if (eq(k, "pitchlow")) { if (!a.f(d.pitchLow)) return false; }
        else if (eq(k, "pitchhigh")) { if (!a.f(d.pitchHigh)) return false; }
        else if (eq(k, "skid")) { if (!a.word(d.skid)) return false; }
        else if (eq(k, "impact")) { if (!a.word(d.impact)) return false; }
        else if (eq(k, "shift")) { if (!a.word(d.shift)) return false; }
        else a.unknown();
    }
    return true;
}

bool recEffect(Args a, VehicleSpec& s) {
    EffectDef& d = s.effects;
    d.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "skid")) { if (!a.word(d.skid)) return false; }
        else if (eq(k, "rate")) { if (!a.f(d.skidRate)) return false; }
        else if (eq(k, "impact")) { if (!a.word(d.impact)) return false; }
        else if (eq(k, "count")) { if (!a.u(d.impactCount)) return false; }
        else a.unknown();
    }
    return true;
}

bool recDamage(Args a, VehicleSpec& s) {
    DamageDef& d = s.damage;
    d.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "health")) { if (!a.f(d.health)) return false; }
        else if (eq(k, "crash")) { if (!a.f(d.crash)) return false; }
        else if (eq(k, "energy")) { if (!a.f(d.energy)) return false; }
        else if (eq(k, "radius")) { if (!a.f(d.radius)) return false; }
        else if (eq(k, "shell")) { if (!a.word(d.shell)) return false; }
        else a.unknown();
    }
    return true;
}

bool recCamera(Args a, VehicleSpec& s) {
    CameraDef& d = s.camera;
    d.present = true;
    while (a.more()) {
        const std::string_view k = a.next();
        if (eq(k, "modes")) {
            d.modes.clear();
            // A mode name ends the list at `arm` or `eye`, or at `hood` when numbers follow (the hood POINT).
            while (a.more()) {
                const std::string_view m = a.t[a.i];
                if (eq(m, "arm") || eq(m, "eye")) break;
                if (eq(m, "hood") && a.i + 1 < a.t.size() && isNumber(a.t[a.i + 1])) break;
                d.modes.push_back(std::string(m));
                ++a.i;
            }
        }
        else if (eq(k, "arm")) { if (!a.f(d.arm)) return false; }
        else if (eq(k, "eye")) { if (!a.vec3(d.eye)) return false; }
        else if (eq(k, "hood")) { if (!a.vec3(d.hood)) return false; }
        else a.unknown();
    }
    return true;
}

}  // namespace

bool parseOcVehicle(std::string_view text, VehicleSpec& out, std::string& err) {
    out = VehicleSpec{};
    Ctx c;
    bool sawHeader = false;
    usize pos = 0;
    while (pos <= text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view rawLine = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++c.line;
        const std::string_view body = trim(truncateHash(rawLine));
        if (body.empty()) continue;
        const std::vector<std::string_view> t = splitWhitespace(body);
        const std::string_view key = t[0];

        if (!sawHeader) {
            if (!eq(key, "OCVEHICLE")) { err = "line " + std::to_string(c.line) + ": not an .ocvehicle (the first record must be OCVEHICLE 1)"; return false; }
            f32 v{};
            if (t.size() < 2 || !parseF(t[1], v)) { err = "line " + std::to_string(c.line) + ": OCVEHICLE needs a version"; return false; }
            if (v > static_cast<f32>(kOcVehicleVersion)) {
                err = "version " + num(v) + " is newer than this build reads (" + std::to_string(kOcVehicleVersion) + ")";
                return false;
            }
            sawHeader = true;
            continue;
        }

        bool ok = true;
        Args a{t, 1, c, ""};
        if (eq(key, "NAME")) {
            // The rest of the line, spaces and all.
            const usize at = body.find(key);
            out.name = std::string(trim(body.substr(at + key.size())));
        }
        else if (eq(key, "KIND")) {
            a.rec = "KIND";
            std::string w;
            ok = a.word(w);
            if (ok) {
                if (eq(w, "wheeled")) out.kind = Kind::Wheeled;
                else if (eq(w, "tracked")) out.kind = Kind::Tracked;
                else ok = bad(c, "KIND", "'" + w + "' is not wheeled or tracked");
            }
        }
        else if (eq(key, "BODY"))       { a.rec = "BODY";       ok = recBody(a, out); }
        else if (eq(key, "BOX")) {
            a.rec = "BOX";
            BoxDef b;
            ok = a.f(b.cx) && a.f(b.cy) && a.f(b.cz) && a.f(b.hx) && a.f(b.hy) && a.f(b.hz);
            if (ok) out.boxes.push_back(b);
        }
        else if (eq(key, "ENGINE"))     { a.rec = "ENGINE";     ok = recEngine(a, out); }
        else if (eq(key, "CURVE")) {
            a.rec = "CURVE";
            out.engine.curve.clear();
            ok = a.pairs(out.engine.curve);
        }
        else if (eq(key, "GEARBOX"))    { a.rec = "GEARBOX";    ok = recGearbox(a, out); }
        else if (eq(key, "CENTER"))     { a.rec = "CENTER";     ok = recCenter(a, out); }
        else if (eq(key, "AXLE"))       { a.rec = "AXLE";       ok = recAxle(a, out); }
        else if (eq(key, "TYRE"))       { a.rec = "TYRE";       ok = recTyre(a, out); }
        else if (eq(key, "WHEELCLASS")) { a.rec = "WHEELCLASS"; ok = recWheelClass(a, out); }
        else if (eq(key, "WHEEL"))      { a.rec = "WHEEL";      ok = recWheel(a, out); }
        else if (eq(key, "TRACK"))      { a.rec = "TRACK";      ok = recTrack(a, out); }
        else if (eq(key, "ASSIST"))     { a.rec = "ASSIST";     ok = recAssist(a, out); }
        else if (eq(key, "STEERCURVE")) {
            a.rec = "STEERCURVE";
            out.steer.points.clear();
            ok = a.pairs(out.steer.points);
        }
        else if (eq(key, "AERO"))       { a.rec = "AERO";       ok = recAero(a, out); }
        else if (eq(key, "SEAT"))       { a.rec = "SEAT";       ok = recSeat(a, out); }
        else if (eq(key, "AUDIO"))      { a.rec = "AUDIO";      ok = recAudio(a, out); }
        else if (eq(key, "EFFECT"))     { a.rec = "EFFECT";     ok = recEffect(a, out); }
        else if (eq(key, "DAMAGE"))     { a.rec = "DAMAGE";     ok = recDamage(a, out); }
        else if (eq(key, "CAMERA"))     { a.rec = "CAMERA";     ok = recCamera(a, out); }
        else if (eq(key, "HUD")) {
            a.rec = "HUD";
            ok = a.word(out.hud);
        }
        else ++c.unknownRecords;
        if (!ok) { err = c.err; return false; }
    }
    if (!sawHeader) { err = "empty file: the first record must be OCVEHICLE 1"; return false; }
    if (c.unknownRecords || c.unknownKeys)
        AVER_WARN("[Vehicle] .ocvehicle: {} unknown record(s) and {} unknown key(s) ignored", c.unknownRecords, c.unknownKeys);
    return true;
}

// ---- writing ----------------------------------------------------------------------------------------

namespace {

const char* diffWordOf(DiffMode m) {
    switch (m) {
        case DiffMode::Open: return "open";
        case DiffMode::Locked: return "locked";
        case DiffMode::LimitedSlip: default: return "lsd";
    }
}

void appendPairs(std::string& s, const std::vector<Vec2>& v) {
    for (const Vec2& p : v) { s += ' '; s += num(p.x); s += ' '; s += num(p.y); }
}

void appendList(std::string& s, const std::vector<f32>& v) {
    for (f32 x : v) { s += ' '; s += num(x); }
}

std::string word(const std::string& s, const char* fallback) {
    std::string w;
    for (char ch : s) w += isSpace(ch) ? '_' : ch;
    return w.empty() ? std::string(fallback) : w;
}

}  // namespace

std::string writeOcVehicle(const VehicleSpec& s) {
    std::string o = "OCVEHICLE 1\n";
    if (!s.name.empty()) o += "NAME " + s.name + "\n";
    o += s.kind == Kind::Tracked ? "KIND tracked\n" : "KIND wheeled\n";
    o += "BODY half " + num(s.body.half.x) + ' ' + num(s.body.half.y) + ' ' + num(s.body.half.z) +
         " clearance " + num(s.body.clearance) +
         " com " + num(s.body.com.x) + ' ' + num(s.body.com.y) + ' ' + num(s.body.com.z) +
         " mass " + num(s.body.massKg) + " pitchroll " + num(s.body.pitchRollDeg) + "\n";
    for (const BoxDef& b : s.boxes)
        o += "BOX " + num(b.cx) + ' ' + num(b.cy) + ' ' + num(b.cz) + ' ' + num(b.hx) + ' ' + num(b.hy) + ' ' + num(b.hz) + "\n";
    o += "ENGINE torque " + num(s.engine.torque) + " minrpm " + num(s.engine.minRpm) + " maxrpm " + num(s.engine.maxRpm) +
         " inertia " + num(s.engine.inertia) + " damping " + num(s.engine.damping) + "\n";
    if (!s.engine.curve.empty()) { o += "CURVE"; appendPairs(o, s.engine.curve); o += '\n'; }
    if (s.gearbox.present) {
        const GearboxDef& g = s.gearbox;
        o += g.manual ? "GEARBOX manual" : "GEARBOX auto";
        if (!g.forward.empty()) { o += " forward"; appendList(o, g.forward); }
        if (!g.reverse.empty()) { o += " reverse"; appendList(o, g.reverse); }
        o += " switch " + num(g.switchTime) + " clutchrelease " + num(g.clutchRelease) + " latency " + num(g.latency) +
             " up " + num(g.up) + " down " + num(g.down) + " clutch " + num(g.clutch) + "\n";
    }
    if (s.centerPresent) {
        o += "CENTER ";
        o += diffWordOf(s.center);
        if (s.center == DiffMode::LimitedSlip) o += ' ' + num(s.centerLsd);
        o += '\n';
    }
    for (usize i = 0; i < s.axles.size(); ++i) {
        const AxleDef& a = s.axles[i];
        o += "AXLE " + std::to_string(i) + " final " + num(a.finalDrive) + " diff " + diffWordOf(a.diff) + " split " + num(a.split) +
             " lsd " + num(a.lsd) + " share " + num(a.share) + " antiroll " + num(a.antiroll) + "\n";
    }
    for (const TyreCurve& t : s.tyres) {
        o += "TYRE " + word(t.name, "tyre");
        if (!t.longitudinal.empty()) { o += " long"; appendPairs(o, t.longitudinal); }
        if (!t.lateral.empty()) { o += " lat"; appendPairs(o, t.lateral); }
        o += '\n';
    }
    for (const WheelClass& c : s.classes) {
        o += "WHEELCLASS " + word(c.name, "class") + " radius " + num(c.radius) + " width " + num(c.width) +
             " susp " + num(c.suspMin) + ' ' + num(c.suspMax) + " hz " + num(c.suspHz) + " damping " + num(c.suspDamping) +
             " brake " + num(c.brakeNm) + " handbrake " + num(c.handbrakeNm) + " inertia " + num(c.inertia) +
             " spindamping " + num(c.spinDamping);
        if (!c.tyre.empty()) o += " tyre " + word(c.tyre, "tyre");
        o += '\n';
    }
    for (usize i = 0; i < s.wheels.size(); ++i) {
        const WheelDef& w = s.wheels[i];
        o += "WHEEL " + word(w.name, ("w" + std::to_string(i)).c_str()) + " class " + word(w.wheelClass, "class") +
             " hub " + num(w.hub.x) + ' ' + num(w.hub.y) + ' ' + num(w.hub.z) + " steer " + num(w.steerDeg) +
             " driven " + (w.driven ? "1" : "0") + " handbrake " + (w.handbrake ? "1" : "0");
        if (w.brakeNm >= 0.0f) o += " brake " + num(w.brakeNm);
        if (!w.node.empty()) o += " node " + word(w.node, "node");
        if (!w.bone.empty()) o += " bone " + word(w.bone, "bone");
        o += '\n';
    }
    for (int side = 0; side < 2; ++side) {
        const TrackDef& t = s.tracks[side];
        if (!t.present) continue;
        o += side == 0 ? "TRACK left wheels" : "TRACK right wheels";
        for (u32 w : t.wheels) o += ' ' + std::to_string(w);
        o += " driven " + std::to_string(t.driven) + " inertia " + num(t.inertia) + " damping " + num(t.damping) +
             " brake " + num(t.brakeNm) + " final " + num(t.finalDrive) + " steergain " + num(t.steerGain) + " pivot " + num(t.pivot) + "\n";
    }
    if (s.assist.present) {
        const AssistDef& d = s.assist;
        o += std::string("ASSIST abs ") + (d.abs ? "1" : "0") + " absslip " + num(d.absSlip) + " absrelease " + num(d.absRelease) +
             " tcs " + (d.tcs ? "1" : "0") + " tcsslip " + num(d.tcsSlip) + " steerin " + num(d.steerIn) + " steerout " + num(d.steerOut) +
             " throttlein " + num(d.throttleIn) + " throttleout " + num(d.throttleOut) + " brakein " + num(d.brakeIn) +
             " reversebrake " + (d.reverseBrake ? "1" : "0") + " reversespeed " + num(d.reverseSpeed) + " holdbrake " + num(d.holdBrake) +
             " holdspeed " + num(d.holdSpeed) + " skidslip " + num(d.skidSlip) + " skidangle " + num(d.skidAngle) + "\n";
    }
    if (!s.steer.points.empty()) { o += "STEERCURVE"; appendPairs(o, s.steer.points); o += '\n'; }
    if (s.aeroPresent) o += "AERO drag " + num(s.drag) + " downforce " + num(s.downforce) + "\n";
    if (s.seat.present)
        o += "SEAT driver " + num(s.seat.driver.x) + ' ' + num(s.seat.driver.y) + ' ' + num(s.seat.driver.z) +
             " exit " + num(s.seat.exit.x) + ' ' + num(s.seat.exit.y) + ' ' + num(s.seat.exit.z) + "\n";
    if (s.audio.present) {
        const AudioDef& d = s.audio;
        o += "AUDIO";
        if (!d.idle.empty()) o += " engine " + word(d.idle, "");
        o += " idle " + num(d.idleRpm);
        if (!d.high.empty()) o += ' ' + word(d.high, "");
        o += " high " + num(d.highRpm) + " pitchlow " + num(d.pitchLow) + " pitchhigh " + num(d.pitchHigh);
        if (!d.skid.empty()) o += " skid " + word(d.skid, "");
        if (!d.impact.empty()) o += " impact " + word(d.impact, "");
        if (!d.shift.empty()) o += " shift " + word(d.shift, "");
        o += '\n';
    }
    if (s.effects.present) {
        const EffectDef& d = s.effects;
        o += "EFFECT";
        if (!d.skid.empty()) o += " skid " + word(d.skid, "");
        o += " rate " + num(d.skidRate);
        if (!d.impact.empty()) o += " impact " + word(d.impact, "");
        o += " count " + std::to_string(d.impactCount) + "\n";
    }
    if (s.damage.present) {
        const DamageDef& d = s.damage;
        o += "DAMAGE health " + num(d.health) + " crash " + num(d.crash) + " energy " + num(d.energy) + " radius " + num(d.radius);
        if (!d.shell.empty()) o += " shell " + word(d.shell, "");
        o += '\n';
    }
    if (s.camera.present) {
        const CameraDef& d = s.camera;
        o += "CAMERA";
        if (!d.modes.empty()) { o += " modes"; for (const std::string& m : d.modes) o += ' ' + word(m, ""); }
        o += " arm " + num(d.arm) + " eye " + num(d.eye.x) + ' ' + num(d.eye.y) + ' ' + num(d.eye.z) +
             " hood " + num(d.hood.x) + ' ' + num(d.hood.y) + ' ' + num(d.hood.z) + "\n";
    }
    if (!s.hud.empty()) o += "HUD " + word(s.hud, "") + "\n";
    return o;
}

// ---- validating -------------------------------------------------------------------------------------

const WheelClass* findClass(const VehicleSpec& spec, const std::string& name) {
    for (const WheelClass& c : spec.classes) if (c.name == name) return &c;
    return nullptr;
}

const TyreCurve* findTyre(const VehicleSpec& spec, const std::string& name) {
    for (const TyreCurve& t : spec.tyres) if (t.name == name) return &t;
    return nullptr;
}

namespace {

bool fin(f32 v) { return std::isfinite(v); }
bool fin3(const Vec3& v) { return fin(v.x) && fin(v.y) && fin(v.z); }

bool ascending(const std::vector<Vec2>& v, f32 lo, f32 hi) {
    for (usize i = 0; i < v.size(); ++i) {
        if (!fin(v[i].x) || !fin(v[i].y) || v[i].y < 0.0f || v[i].x < lo || v[i].x > hi) return false;
        if (i > 0 && !(v[i].x > v[i - 1].x)) return false;
    }
    return true;
}

bool fail(std::string& why, std::string msg) { why = std::move(msg); return false; }

}  // namespace

bool validate(const VehicleSpec& s, std::string& why) {
    why.clear();
    if (s.wheels.empty()) return fail(why, "the vehicle has no wheels");
    if (s.wheels.size() > kMaxWheels) return fail(why, "more than 16 wheels");
    const BodyDef& b = s.body;
    if (!fin3(b.half) || !(b.half.x > 0.0f && b.half.y > 0.0f && b.half.z > 0.0f)) return fail(why, "BODY half extents must be positive");
    if (!fin(b.clearance) || b.clearance < 0.0f) return fail(why, "BODY clearance must not be negative");
    if (!fin3(b.com)) return fail(why, "BODY com is not a number");
    if (!fin(b.massKg) || !(b.massKg > 0.0f)) return fail(why, "BODY mass must be positive");
    if (!fin(b.pitchRollDeg)) return fail(why, "BODY pitchroll is not a number");
    if (s.boxes.size() > kMaxBoxes) return fail(why, "more than 8 BOX records");
    for (const BoxDef& x : s.boxes)
        if (!fin(x.cx) || !fin(x.cy) || !fin(x.cz) || !(x.hx > 0.0f && x.hy > 0.0f && x.hz > 0.0f && fin(x.hx) && fin(x.hy) && fin(x.hz)))
            return fail(why, "a BOX needs finite centres and positive half extents");

    const EngineDef& e = s.engine;
    if (!fin(e.torque) || !(e.torque > 0.0f)) return fail(why, "ENGINE torque must be positive");
    if (!fin(e.minRpm) || e.minRpm < 0.0f || !fin(e.maxRpm) || !(e.maxRpm > e.minRpm)) return fail(why, "ENGINE needs 0 <= minrpm < maxrpm");
    if (!fin(e.inertia) || e.inertia < 0.0f || !fin(e.damping) || e.damping < 0.0f) return fail(why, "ENGINE inertia and damping must not be negative");
    if (!e.curve.empty()) {
        if (e.curve.size() < 2 || e.curve.size() > kMaxCurvePoints) return fail(why, "CURVE needs 2 to 32 points");
        if (!ascending(e.curve, 0.0f, 1.0f)) return fail(why, "CURVE x values must ascend within 0..1 and torque must not be negative");
    }
    const GearboxDef& g = s.gearbox;
    if (g.forward.size() > 16 || g.reverse.size() > 4) return fail(why, "GEARBOX has too many ratios");
    for (f32 r : g.forward) if (!fin(r) || !(r > 0.0f)) return fail(why, "GEARBOX forward ratios must be positive");
    for (f32 r : g.reverse) if (!fin(r) || !(r > 0.0f)) return fail(why, "GEARBOX reverse ratios must be positive magnitudes");
    if (!fin(g.switchTime) || g.switchTime < 0.0f || !fin(g.clutchRelease) || g.clutchRelease < 0.0f || !fin(g.latency) || g.latency < 0.0f ||
        !fin(g.clutch) || g.clutch < 0.0f)
        return fail(why, "GEARBOX timings must not be negative");
    if (!(g.down > 0.0f && g.down < g.up && g.up < 1.0f)) return fail(why, "GEARBOX needs 0 < down < up < 1");
    if (s.centerPresent && s.center == DiffMode::LimitedSlip && !(fin(s.centerLsd) && s.centerLsd > 1.0f)) return fail(why, "CENTER lsd ratio must exceed 1");
    for (const AxleDef& a : s.axles)
        if (!fin(a.finalDrive) || !(a.finalDrive > 0.0f) || !fin(a.split) || a.split < 0.0f || a.split > 1.0f || !fin(a.lsd) || !(a.lsd > 1.0f) ||
            !fin(a.share) || a.share < 0.0f || !fin(a.antiroll) || a.antiroll < 0.0f)
            return fail(why, "an AXLE has a number out of range");
    for (const TyreCurve& t : s.tyres) {
        if (t.longitudinal.size() < 2 || t.longitudinal.size() > kMaxTyrePoints || t.lateral.size() < 2 || t.lateral.size() > kMaxTyrePoints)
            return fail(why, "TYRE " + t.name + " needs 2 to 16 points on each curve");
        if (!ascending(t.longitudinal, 0.0f, 1.0e6f) || !ascending(t.lateral, 0.0f, 1.0e6f))
            return fail(why, "TYRE " + t.name + " curves must ascend and not go negative");
    }
    for (const WheelClass& c : s.classes) {
        if (!fin(c.radius) || !(c.radius > 0.0f) || !fin(c.width) || !(c.width > 0.0f)) return fail(why, "WHEELCLASS " + c.name + " radius and width must be positive");
        if (!fin(c.suspMin) || c.suspMin < 0.0f || !fin(c.suspMax) || !(c.suspMax > c.suspMin)) return fail(why, "WHEELCLASS " + c.name + " needs 0 <= susp min < max");
        if (!fin(c.suspHz) || !(c.suspHz > 0.0f) || !fin(c.suspDamping) || c.suspDamping < 0.0f) return fail(why, "WHEELCLASS " + c.name + " hz must be positive and damping not negative");
        if (!fin(c.brakeNm) || c.brakeNm < 0.0f || !fin(c.handbrakeNm) || c.handbrakeNm < 0.0f) return fail(why, "WHEELCLASS " + c.name + " brake torques must not be negative");
        if (!fin(c.inertia) || !(c.inertia > 0.0f) || !fin(c.spinDamping) || c.spinDamping < 0.0f) return fail(why, "WHEELCLASS " + c.name + " inertia must be positive");
        if (!c.tyre.empty() && !findTyre(s, c.tyre)) return fail(why, "WHEELCLASS " + c.name + " names the undefined tyre " + c.tyre);
    }
    bool anyDriven = false;
    for (const WheelDef& w : s.wheels) {
        if (!findClass(s, w.wheelClass)) return fail(why, "WHEEL " + w.name + " names the undefined class '" + w.wheelClass + "'");
        if (!fin3(w.hub) || !fin(w.steerDeg)) return fail(why, "WHEEL " + w.name + " has a number that is not finite");
        if (w.brakeNm >= 0.0f && !fin(w.brakeNm)) return fail(why, "WHEEL " + w.name + " brake is not a number");
        anyDriven = anyDriven || w.driven;
    }
    if (s.kind == Kind::Wheeled) {
        if (!anyDriven) return fail(why, "no wheel is driven, so the engine would be connected to nothing");
    } else {
        std::vector<int> owner(s.wheels.size(), -1);
        for (int side = 0; side < 2; ++side) {
            const TrackDef& t = s.tracks[side];
            const char* name = side == 0 ? "left" : "right";
            if (!t.present || t.wheels.empty()) return fail(why, std::string("a tracked vehicle needs a TRACK ") + name);
            bool drivenIn = false;
            for (u32 w : t.wheels) {
                if (w >= s.wheels.size()) return fail(why, std::string("the TRACK ") + name + " names a wheel that does not exist");
                if (owner[w] != -1) return fail(why, "a wheel is on two tracks");
                owner[w] = side;
                drivenIn = drivenIn || w == t.driven;
            }
            if (!drivenIn) return fail(why, std::string("the driven wheel of the TRACK ") + name + " is not one of its wheels");
            if (!fin(t.inertia) || !(t.inertia > 0.0f) || !fin(t.damping) || t.damping < 0.0f || !fin(t.brakeNm) || t.brakeNm < 0.0f ||
                !fin(t.finalDrive) || !(t.finalDrive > 0.0f) || !fin(t.steerGain) || t.steerGain < 0.0f || !fin(t.pivot) || t.pivot < 0.0f)
                return fail(why, std::string("the TRACK ") + name + " has a number out of range");
        }
        for (usize i = 0; i < owner.size(); ++i)
            if (owner[i] == -1) return fail(why, "wheel " + std::to_string(i) + " is on neither track");
    }
    const AssistDef& a = s.assist;
    if (!fin(a.absSlip) || !fin(a.absRelease) || !fin(a.tcsSlip) || !fin(a.steerIn) || !fin(a.steerOut) || !fin(a.throttleIn) || !fin(a.throttleOut) ||
        !fin(a.brakeIn) || !fin(a.reverseSpeed) || !fin(a.holdBrake) || !fin(a.holdSpeed) || !fin(a.skidSlip) || !fin(a.skidAngle) ||
        a.absSlip < 0.0f || a.absRelease < 0.0f || a.tcsSlip < 0.0f || a.steerIn < 0.0f || a.steerOut < 0.0f || a.throttleIn < 0.0f ||
        a.throttleOut < 0.0f || a.brakeIn < 0.0f || a.reverseSpeed < 0.0f || a.holdBrake < 0.0f || a.holdSpeed < 0.0f || a.skidSlip < 0.0f || a.skidAngle < 0.0f)
        return fail(why, "ASSIST has a number out of range");
    if (s.steer.points.size() > kMaxSteerPoints) return fail(why, "STEERCURVE has more than 4 points");
    for (const Vec2& p : s.steer.points)
        if (!fin(p.x) || p.x < 0.0f || !fin(p.y) || p.y < 0.0f) return fail(why, "STEERCURVE speeds and fractions must not be negative");
    if (!fin(s.drag) || s.drag < 0.0f || !fin(s.downforce) || s.downforce < 0.0f) return fail(why, "AERO values must not be negative");
    if (!fin3(s.seat.driver) || !fin3(s.seat.exit)) return fail(why, "SEAT has a number that is not finite");
    if (!fin(s.damage.health) || s.damage.health < 0.0f || !fin(s.damage.crash) || s.damage.crash < 0.0f || !fin(s.damage.radius) || s.damage.radius < 0.0f) return fail(why, "DAMAGE has a number out of range");
    return true;
}

const VehicleSpec* SpecCache::get(const std::string& path, const ReadFn& read) {
    const auto it = entries_.find(path);
    if (it != entries_.end()) return it->second.get();
    std::unique_ptr<VehicleSpec> spec;
    std::string text, err;
    if (read && read(path, text)) {
        auto parsed = std::make_unique<VehicleSpec>();
        if (!parseOcVehicle(text, *parsed, err)) AVER_WARN("[Vehicle] {}: {}", path, err);
        else if (!validate(*parsed, err)) AVER_WARN("[Vehicle] {}: {}", path, err);
        else spec = std::move(parsed);
    } else {
        AVER_WARN("[Vehicle] {}: the file could not be read", path);
    }
    return entries_.emplace(path, std::move(spec)).first->second.get();
}

void SpecCache::invalidate(const std::string& path) { entries_.erase(path); }
void SpecCache::clear() { entries_.clear(); }

}  // namespace aver::vehicle
