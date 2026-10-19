#pragma once
// The particle editor's edit logic, with no ImGui and no window: add / remove / duplicate / reorder emitters, add / remove /
// reorder / enable modules, bind a module parameter to a user parameter, add / rename / remove user parameters, add / remove
// events, edit curve stops, and Compute bounds. Every function takes the data it edits by reference, returns whether it
// changed anything, and leaves the data UNCHANGED when it returns false. Every edit is clamped to what the .ocparticle parser
// accepts (OcParticle.cpp), so a typed value can never be one the save path would go on to refuse: tests/editor's
// ParticleStackModelTest pins that by round-tripping every result through writeOcparticleSystem / parseOcparticleSystem.
//
// Header-only so ParticleEditor.cpp, the UI translation unit and the test share one definition.
#include "aver/particles/EmitterSim.hpp"
#include "aver/particles/ParticleModules.hpp"
#include "aver/particles/ParticleTypes.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aver::editor::pxmodel {

using particles::Curve4;
using particles::ModuleKind;
using particles::ParticleEffect;
using particles::ParticleSystemDef;
using particles::UserParam;

// ---- names ----
// Emitter and parameter names are single tokens in the file (ONEVENT names its child, @Name binds a parameter), so no spaces.
inline bool validEmitterName(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}
// A parameter name additionally excludes '.', which separates the component in @Name.x.
inline bool validParamName(const std::string& n) {
    if (!validEmitterName(n) || n.find('.') != std::string::npos || n.find('-') != std::string::npos) return false;
    return !(n[0] >= '0' && n[0] <= '9');
}
inline std::string sanitizeName(std::string n) {
    for (char& c : n) if (c == ' ' || c == '\t') c = '_';
    return n;
}
inline bool emitterNameTaken(const ParticleSystemDef& d, const std::string& n, int ignore = -1) {
    for (usize i = 0; i < d.emitters.size(); ++i)
        if (static_cast<int>(i) != ignore && d.emitters[i].name == n) return true;
    return false;
}
inline std::string uniqueEmitterName(const ParticleSystemDef& d, std::string base, int ignore = -1) {
    base = sanitizeName(std::move(base));
    if (base.empty()) base = "Emitter";
    if (!emitterNameTaken(d, base, ignore)) return base;
    for (int k = 2; k < 1000; ++k) {
        std::string c = base + std::to_string(k);
        if (!emitterNameTaken(d, c, ignore)) return c;
    }
    return base;
}
inline bool paramNameTaken(const ParticleSystemDef& d, const std::string& n, int ignore = -1) {
    for (usize i = 0; i < d.params.size(); ++i)
        if (static_cast<int>(i) != ignore && d.params[i].name == n) return true;
    return false;
}

// ---- emitters ----
inline int addEmitter(ParticleSystemDef& d, std::string name = {}) {
    if (d.emitters.size() >= particles::kMaxEmittersPerSystem) return -1;
    ParticleEffect e;
    e.name = uniqueEmitterName(d, name.empty() ? std::string("Emitter") : name);
    d.emitters.push_back(std::move(e));
    return static_cast<int>(d.emitters.size()) - 1;
}

inline int duplicateEmitter(ParticleSystemDef& d, int idx) {
    if (idx < 0 || static_cast<usize>(idx) >= d.emitters.size() || d.emitters.size() >= particles::kMaxEmittersPerSystem) return -1;
    ParticleEffect e = d.emitters[static_cast<usize>(idx)];
    e.name = uniqueEmitterName(d, e.name.empty() ? std::string("Emitter") : e.name + "_copy");
    e.events.clear();   // a copy does not inherit the original's event rules (they would double every child spawn)
    d.emitters.insert(d.emitters.begin() + idx + 1, std::move(e));
    for (ParticleEffect& o : d.emitters)
        for (particles::EventSpawn& ev : o.events)
            if (ev.emitter > static_cast<u32>(idx)) ++ev.emitter;
    return idx + 1;
}

inline bool removeEmitter(ParticleSystemDef& d, int idx) {
    if (idx < 0 || static_cast<usize>(idx) >= d.emitters.size() || d.emitters.size() <= 1) return false;
    d.emitters.erase(d.emitters.begin() + idx);
    for (ParticleEffect& o : d.emitters) {
        auto& evs = o.events;
        evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const particles::EventSpawn& ev) { return ev.emitter == static_cast<u32>(idx); }), evs.end());
        for (particles::EventSpawn& ev : evs)
            if (ev.emitter > static_cast<u32>(idx)) --ev.emitter;
    }
    return true;
}

// Moves emitter `from` to position `to`, remapping every event rule's target.
inline bool moveEmitter(ParticleSystemDef& d, int from, int to) {
    const int n = static_cast<int>(d.emitters.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    std::vector<u32> map(static_cast<usize>(n));
    for (int i = 0; i < n; ++i) map[static_cast<usize>(i)] = static_cast<u32>(i);
    // The new order of old indices.
    std::vector<int> order(static_cast<usize>(n));
    for (int i = 0; i < n; ++i) order[static_cast<usize>(i)] = i;
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, from);
    std::vector<ParticleEffect> moved;
    moved.reserve(static_cast<usize>(n));
    for (int i = 0; i < n; ++i) {
        map[static_cast<usize>(order[static_cast<usize>(i)])] = static_cast<u32>(i);
        moved.push_back(std::move(d.emitters[static_cast<usize>(order[static_cast<usize>(i)])]));
    }
    d.emitters = std::move(moved);
    for (ParticleEffect& o : d.emitters)
        for (particles::EventSpawn& ev : o.events)
            if (ev.emitter < static_cast<u32>(n)) ev.emitter = map[ev.emitter];
    return true;
}

inline bool renameEmitter(ParticleSystemDef& d, int idx, const std::string& name) {
    if (idx < 0 || static_cast<usize>(idx) >= d.emitters.size()) return false;
    const std::string n = sanitizeName(name);
    if (!validEmitterName(n) || emitterNameTaken(d, n, idx) || d.emitters[static_cast<usize>(idx)].name == n) return false;
    d.emitters[static_cast<usize>(idx)].name = n;
    return true;
}

// ---- modules ----
inline int addModule(ParticleEffect& e, ModuleKind kind) {
    if (e.modules.size() >= particles::kMaxModulesPerEmitter || static_cast<u32>(kind) >= static_cast<u32>(ModuleKind::Count)) return -1;
    e.modules.push_back(particles::makeDefaultModule(kind));
    return static_cast<int>(e.modules.size()) - 1;
}
inline bool removeModule(ParticleEffect& e, int idx) {
    if (idx < 0 || static_cast<usize>(idx) >= e.modules.size()) return false;
    e.modules.erase(e.modules.begin() + idx);
    return true;
}
inline bool moveModule(ParticleEffect& e, int from, int to) {
    const int n = static_cast<int>(e.modules.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    particles::Module m = e.modules[static_cast<usize>(from)];
    e.modules.erase(e.modules.begin() + from);
    e.modules.insert(e.modules.begin() + to, m);
    return true;
}
inline bool setModuleEnabled(ParticleEffect& e, int idx, bool on) {
    if (idx < 0 || static_cast<usize>(idx) >= e.modules.size() || e.modules[static_cast<usize>(idx)].enabled == on) return false;
    e.modules[static_cast<usize>(idx)].enabled = on;
    return true;
}
// Sets a literal value (clamped where the module needs it); a parameter bound to a user parameter becomes a literal again.
inline bool setModuleParam(ParticleEffect& e, int mi, int pi, f32 value) {
    if (mi < 0 || static_cast<usize>(mi) >= e.modules.size() || pi < 0 || pi >= static_cast<int>(particles::kMaxModuleParams)) return false;
    particles::Module& m = e.modules[static_cast<usize>(mi)];
    const particles::ModuleInfo& info = particles::moduleInfo(m.kind);
    if (pi >= static_cast<int>(info.paramCount) || !std::isfinite(value)) return false;
    if (m.kind == ModuleKind::UpdateCurlNoise && pi == 3) value = std::clamp(std::floor(value), 1.0f, 4.0f);   // octaves
    particles::ModuleParam& p = m.p[pi];
    if (p.value == value && p.userSlot < 0) return false;
    p.value = value;
    p.userSlot = -1;
    p.comp = 0;
    return true;
}
inline u32 userParamComponents(const UserParam& p) { return p.kind == 0 ? 1u : (p.kind == 1 ? 3u : 4u); }
inline bool bindModuleParam(ParticleSystemDef& d, int emitter, int mi, int pi, int slot, int comp) {
    if (emitter < 0 || static_cast<usize>(emitter) >= d.emitters.size()) return false;
    ParticleEffect& e = d.emitters[static_cast<usize>(emitter)];
    if (mi < 0 || static_cast<usize>(mi) >= e.modules.size() || slot < 0 || static_cast<usize>(slot) >= d.params.size()) return false;
    particles::Module& m = e.modules[static_cast<usize>(mi)];
    if (pi < 0 || pi >= static_cast<int>(particles::moduleInfo(m.kind).paramCount)) return false;
    if (comp < 0 || static_cast<u32>(comp) >= userParamComponents(d.params[static_cast<usize>(slot)])) return false;
    particles::ModuleParam& p = m.p[pi];
    if (p.userSlot == slot && p.comp == comp) return false;
    p.userSlot = static_cast<i8>(slot);
    p.comp = static_cast<u8>(comp);
    return true;
}
inline bool unbindModuleParam(ParticleEffect& e, int mi, int pi) {
    if (mi < 0 || static_cast<usize>(mi) >= e.modules.size() || pi < 0 || pi >= static_cast<int>(particles::kMaxModuleParams)) return false;
    particles::ModuleParam& p = e.modules[static_cast<usize>(mi)].p[pi];
    if (p.userSlot < 0) return false;
    p.userSlot = -1;
    p.comp = 0;
    return true;
}

// ---- user parameters ----
inline int addUserParam(ParticleSystemDef& d, std::string name, u8 kind) {
    if (d.params.size() >= particles::kMaxUserParams || kind > 2) return -1;
    name = sanitizeName(std::move(name));
    if (!validParamName(name) || paramNameTaken(d, name)) {
        // Offer the next free generic name rather than refusing a blank "add".
        std::string base = validParamName(name) ? name : "Param";
        name = base;
        for (int k = 2; paramNameTaken(d, name) && k < 1000; ++k) name = base + std::to_string(k);
        if (!validParamName(name)) return -1;
    }
    UserParam p;
    p.name = name;
    p.kind = kind;
    if (kind == 0) p.v[0] = 1.0f;
    if (kind == 2) for (f32& c : p.v) c = 1.0f;
    d.params.push_back(p);
    return static_cast<int>(d.params.size()) - 1;
}
inline bool renameUserParam(ParticleSystemDef& d, int idx, const std::string& name) {
    if (idx < 0 || static_cast<usize>(idx) >= d.params.size()) return false;
    const std::string n = sanitizeName(name);
    if (!validParamName(n) || paramNameTaken(d, n, idx) || d.params[static_cast<usize>(idx)].name == n) return false;
    d.params[static_cast<usize>(idx)].name = n;
    return true;
}
// Removing a parameter turns every module parameter that referenced it back into its literal and shifts higher slots down.
inline bool removeUserParam(ParticleSystemDef& d, int idx) {
    if (idx < 0 || static_cast<usize>(idx) >= d.params.size()) return false;
    d.params.erase(d.params.begin() + idx);
    for (ParticleEffect& e : d.emitters)
        for (particles::Module& m : e.modules)
            for (particles::ModuleParam& p : m.p) {
                if (p.userSlot == idx) { p.userSlot = -1; p.comp = 0; }
                else if (p.userSlot > idx) --p.userSlot;
            }
    return true;
}
inline bool setUserParamValue(ParticleSystemDef& d, int idx, const f32 v[4]) {
    if (idx < 0 || static_cast<usize>(idx) >= d.params.size()) return false;
    UserParam& p = d.params[static_cast<usize>(idx)];
    bool changed = false;
    for (u32 k = 0; k < userParamComponents(p); ++k) {
        if (!std::isfinite(v[k])) return false;
        changed = changed || p.v[k] != v[k];
    }
    if (!changed) return false;
    for (u32 k = 0; k < userParamComponents(p); ++k) p.v[k] = v[k];
    return true;
}
// Changing a parameter's kind keeps its values and unbinds components that no longer exist.
inline bool setUserParamKind(ParticleSystemDef& d, int idx, u8 kind) {
    if (idx < 0 || static_cast<usize>(idx) >= d.params.size() || kind > 2 || d.params[static_cast<usize>(idx)].kind == kind) return false;
    UserParam& p = d.params[static_cast<usize>(idx)];
    p.kind = kind;
    const u32 comps = userParamComponents(p);
    for (ParticleEffect& e : d.emitters)
        for (particles::Module& m : e.modules)
            for (particles::ModuleParam& mp : m.p)
                if (mp.userSlot == idx && mp.comp >= comps) { mp.userSlot = -1; mp.comp = 0; }
    return true;
}

// ---- events ----
// True when a rule parent -> child would close a loop (the parser refuses cycles).
inline bool eventWouldCycle(const ParticleSystemDef& d, usize parent, usize child) {
    if (parent == child) return true;
    std::vector<usize> stack{child};
    std::vector<u8> seen(d.emitters.size(), 0);
    while (!stack.empty()) {
        const usize e = stack.back();
        stack.pop_back();
        if (e == parent) return true;
        if (e >= d.emitters.size() || seen[e]) continue;
        seen[e] = 1;
        for (const particles::EventSpawn& ev : d.emitters[e].events) stack.push_back(ev.emitter);
    }
    return false;
}
inline bool addEvent(ParticleSystemDef& d, int parent, u8 on, int child, u32 count, f32 inherit) {
    if (parent < 0 || child < 0 || static_cast<usize>(parent) >= d.emitters.size() || static_cast<usize>(child) >= d.emitters.size()) return false;
    if (on > 1 || count == 0 || count > 1000 || !std::isfinite(inherit) || eventWouldCycle(d, static_cast<usize>(parent), static_cast<usize>(child))) return false;
    if (d.emitters[static_cast<usize>(child)].name.empty() || d.emitters[static_cast<usize>(child)].name.find(' ') != std::string::npos) return false;
    particles::EventSpawn ev;
    ev.on = on;
    ev.emitter = static_cast<u32>(child);
    ev.count = count;
    ev.inheritVelocity = std::clamp(inherit, 0.0f, 4.0f);
    d.emitters[static_cast<usize>(parent)].events.push_back(ev);
    return true;
}
inline bool removeEvent(ParticleSystemDef& d, int parent, int idx) {
    if (parent < 0 || static_cast<usize>(parent) >= d.emitters.size()) return false;
    auto& evs = d.emitters[static_cast<usize>(parent)].events;
    if (idx < 0 || static_cast<usize>(idx) >= evs.size()) return false;
    evs.erase(evs.begin() + idx);
    return true;
}

// ---- curves ----
// Stops are kept strictly increasing in time (the parser refuses otherwise) and there are 0 or 2..4 of them.
inline void clearCurve(Curve4& c) { c = Curve4{}; }
inline bool addCurvePoint(Curve4& c, f32 t, const f32 v[4]) {
    if (c.n >= 4 || !std::isfinite(t)) return false;
    t = std::clamp(t, 0.0f, 1.0f);
    for (u32 i = 0; i < c.n; ++i)
        if (std::fabs(c.t[i] - t) < 1e-3f) return false;
    // A first point makes a flat two-stop curve so it is a valid curve at once.
    if (c.n == 0) {
        c.n = 2;
        c.t[0] = 0.0f; c.t[1] = 1.0f;
        for (int k = 0; k < 4; ++k) { c.v[0][k] = v[k]; c.v[1][k] = v[k]; }
        return true;
    }
    u32 at = c.n;
    for (u32 i = 0; i < c.n; ++i) if (t < c.t[i]) { at = i; break; }
    for (u32 i = c.n; i > at; --i) { c.t[i] = c.t[i - 1]; for (int k = 0; k < 4; ++k) c.v[i][k] = c.v[i - 1][k]; }
    c.t[at] = t;
    for (int k = 0; k < 4; ++k) c.v[at][k] = v[k];
    ++c.n;
    return true;
}
inline bool removeCurvePoint(Curve4& c, int idx) {
    if (c.n < 3 && c.n != 0) { clearCurve(c); return idx >= 0; }   // below two stops there is no curve: back to the start/end pair
    if (idx < 0 || static_cast<u32>(idx) >= c.n) return false;
    for (u32 i = static_cast<u32>(idx); i + 1 < c.n; ++i) { c.t[i] = c.t[i + 1]; for (int k = 0; k < 4; ++k) c.v[i][k] = c.v[i + 1][k]; }
    --c.n;
    c.t[c.n] = 0.0f;
    for (int k = 0; k < 4; ++k) c.v[c.n][k] = 0.0f;
    return true;
}
// Moves/edits a stop. Its time is clamped between its neighbours' (strictly), the end stops keep their ends in [0, 1].
inline bool setCurvePoint(Curve4& c, int idx, f32 t, const f32 v[4]) {
    if (idx < 0 || static_cast<u32>(idx) >= c.n || !std::isfinite(t)) return false;
    for (int k = 0; k < 4; ++k) if (!std::isfinite(v[k])) return false;
    const u32 i = static_cast<u32>(idx);
    const f32 lo = i == 0 ? 0.0f : c.t[i - 1] + 1e-3f;
    const f32 hi = i + 1 == c.n ? 1.0f : c.t[i + 1] - 1e-3f;
    if (lo > hi) return false;
    t = std::clamp(t, lo, hi);
    bool changed = c.t[i] != t;
    for (int k = 0; k < 4; ++k) changed = changed || c.v[i][k] != v[k];
    if (!changed) return false;
    c.t[i] = t;
    for (int k = 0; k < 4; ++k) c.v[i][k] = v[k];
    return true;
}

// ---- Compute bounds ----
// Runs EmitterSim for 3 s of simulated time at 60 Hz from the emitter's origin and returns the radius the particles reach,
// plus the largest half-size, times 1.2 (what BOUNDS wants). Capped at 4096 live particles so a huge GPU emitter stays cheap.
inline f32 computeBoundsRadius(const ParticleSystemDef& d, usize emitter) {
    if (emitter >= d.emitters.size()) return 0.0f;
    ParticleEffect fx = d.emitters[emitter];
    fx.maxParticles = std::min<u32>(std::max(1u, fx.maxParticles), 4096u);
    particles::EmitterSim sim;
    sim.init(fx, d.params, 12345u);
    const f32 dt = 1.0f / 60.0f;
    f32 accum = 0.0f, maxR = 0.0f, maxSize = 0.0f;
    bool burst = true;
    for (int frame = 0; frame < 180; ++frame) {
        u32 toSpawn = 0;
        if (burst) { toSpawn += fx.burstCount; burst = false; }
        if (fx.emissionRate > 0.0f) {
            accum += fx.emissionRate * dt;
            const u32 whole = static_cast<u32>(accum);
            toSpawn += whole;
            accum -= static_cast<f32>(whole);
        }
        if (toSpawn > 0) {
            particles::SpawnRequest r;
            r.count = toSpawn;
            sim.spawn(r);
        }
        sim.step(dt, dt * static_cast<f32>(frame), nullptr);
        const auto& ps = sim.particles();
        for (usize i = 0; i < ps.size(); ++i) {
            if (!particles::particleAlive(ps[i])) continue;
            maxR = std::max(maxR, ps[i].position.size());
            const f32 t = ps[i].lifetime > 0.0f ? std::clamp(ps[i].age / ps[i].lifetime, 0.0f, 1.0f) : 1.0f;
            maxSize = std::max(maxSize, particles::evalSize(fx, t) * ps[i].sizeScale);
        }
        if (sim.trailRing() != 0)
            for (const Vec4& p : sim.trail()) maxR = std::max(maxR, Vec3{p.x, p.y, p.z}.size());
    }
    return (maxR + maxSize * 0.5f) * 1.2f;
}

} // namespace aver::editor::pxmodel
