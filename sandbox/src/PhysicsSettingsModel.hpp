#pragma once
// The model behind Project Settings > Physics (docs/plans/wave11/WORLD_STATE_DEBUG.md): rows built from the physics ABI's
// settings table, validation, the "changed" test, canonical omission of defaults and the restart-badge logic.
// ImGui-free on purpose, like the other *Model.hpp files, so PhysicsSettingsModelTest can drive it with a
// ProjectDesc and the real physics module and no window.
//
// The project keeps only what the author CHANGED: a value equal to the default is removed, so a manifest never
// carries a PHYSICS.* line that restates Jolt (OcProject's canonical form). Setting a value also applies it to the
// physics module (live settings at once; limits as the pending config the next world reads).
#include "aver/core/Types.hpp"
#include "aver/formats/OcProject.hpp"
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_world_abi.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace aver::editor {

#if AVER_MODULE_PHYSICS

// One live solver setting as the page shows it.
struct PhysicsSettingRow {
    int key = 0;
    std::string name;          // "velocitySteps"
    std::string projectKey;    // "PHYSICS.VELOCITYSTEPS"
    std::string unit;          // "count", "cm", "cm/s", "s", "degrees", "fraction", "bool"
    float min = 0, max = 0, def = 0;
    bool isBool = false;
    bool isCount = false;      // whole numbers
};

// One init-time limit (fixed for a world's lifetime).
struct PhysicsLimitRow {
    int key = 0;               // AVER_PHYS_CF_*
    std::string name;          // "maxBodies"
    std::string projectKey;    // "PHYSICS.MAXBODIES"
    float min = 0, max = 0, def = 0;
};

inline std::string physicsUpperKey(const std::string& camel) {
    std::string s = "PHYSICS.";
    for (const char c : camel) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::string physicsNumberText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

// Every setting in the ABI's table order.
inline std::vector<PhysicsSettingRow> physicsSettingRows() {
    std::vector<PhysicsSettingRow> rows;
    const int n = aver_phys_setting_count();
    rows.reserve(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        PhysicsSettingRow r;
        r.key = k;
        r.name = aver_phys_setting_name(k);
        r.projectKey = physicsUpperKey(r.name);
        r.unit = aver_phys_setting_unit(k);
        aver_phys_setting_range(k, &r.min, &r.max, &r.def);
        r.isBool = r.unit == "bool";
        r.isCount = r.unit == "count";
        rows.push_back(std::move(r));
    }
    return rows;
}

// The five limits. Their project keys are the names OcProject has always used (TEMPALLOCMB, not TEMPMB).
inline std::vector<PhysicsLimitRow> physicsLimitRows() {
    static const char* const kProjectKeys[AVER_PHYS_CF_COUNT] = {
        "PHYSICS.MAXBODIES", "PHYSICS.MAXBODYPAIRS", "PHYSICS.MAXCONTACTS", "PHYSICS.TEMPALLOCMB", "PHYSICS.THREADS"};
    std::vector<PhysicsLimitRow> rows;
    for (int k = 0; k < AVER_PHYS_CF_COUNT; ++k) {
        PhysicsLimitRow r;
        r.key = k;
        r.name = aver_phys_config_name(k);
        r.projectKey = kProjectKeys[k];
        aver_phys_config_range(k, &r.min, &r.max, &r.def);
        rows.push_back(std::move(r));
    }
    return rows;
}

// Tooltip text: the unit, the range and Jolt's default, so a row always says what its number means.
inline std::string physicsSettingTooltip(const PhysicsSettingRow& r) {
    std::string t = r.projectKey + "\nunit: " + r.unit;
    if (!r.isBool) t += "\nrange: " + physicsNumberText(r.min) + " .. " + physicsNumberText(r.max);
    t += "\ndefault: " + physicsNumberText(r.def);
    if (r.name == "deterministic")
        t += "\nTurning this off is faster but BREAKS state snapshots and replays.";
    if (r.name == "velocitySteps")
        t += "\nBelow 2, friction stops working (Jolt).";
    return t;
}

inline std::string physicsLimitTooltip(const PhysicsLimitRow& r) {
    std::string t = r.projectKey + "\nrange: " + physicsNumberText(r.min) + " .. " + physicsNumberText(r.max) +
                    "\ndefault: " + physicsNumberText(r.def);
    if (r.key == AVER_PHYS_CF_THREADS) t += "\n-1 = automatic (cores - 2), 0 = single-threaded, N = N workers.";
    t += "\nFixed for a world's lifetime: applies when the world is next created.";
    return t;
}

// Whether `v` is a legal value for the row (finite, inside the range); `why` receives the reason.
inline bool physicsSettingValid(const PhysicsSettingRow& r, float v, std::string* why = nullptr) {
    if (std::isnan(v) || std::isinf(v)) { if (why) *why = "not a number"; return false; }
    if (v < r.min || v > r.max) {
        if (why) *why = "must be within " + physicsNumberText(r.min) + " .. " + physicsNumberText(r.max);
        return false;
    }
    return true;
}

// The value brought into the row's range and, for a count or a bool, to a whole number.
inline float physicsSettingClamp(const PhysicsSettingRow& r, float v) {
    if (std::isnan(v)) return r.def;
    v = std::min(std::max(v, r.min), r.max);
    if (r.isCount || r.isBool) v = std::round(v);
    return v;
}

namespace detail {
inline bool nearDefault(float v, float def) {
    return std::fabs(v - def) <= 1e-6f * std::max(1.0f, std::fabs(def));
}
inline std::vector<std::pair<std::string, f32>>::iterator findStated(fmt::ProjectDesc& p, const std::string& name) {
    return std::find_if(p.physicsSettings.begin(), p.physicsSettings.end(),
                        [&](const std::pair<std::string, f32>& e) { return e.first == name; });
}
} // namespace detail

inline bool physicsSettingStated(const fmt::ProjectDesc& p, const PhysicsSettingRow& r) {
    for (const auto& e : p.physicsSettings) if (e.first == r.name) return true;
    return false;
}

// The value the project gives the setting: what it states, else Jolt's default.
inline float physicsSettingValue(const fmt::ProjectDesc& p, const PhysicsSettingRow& r) {
    for (const auto& e : p.physicsSettings) if (e.first == r.name) return e.second;
    return r.def;
}

// True when the project states the setting AND it differs from the default (the row's "changed" dot).
inline bool physicsSettingChanged(const fmt::ProjectDesc& p, const PhysicsSettingRow& r) {
    return physicsSettingStated(p, r) && !detail::nearDefault(physicsSettingValue(p, r), r.def);
}

inline int physicsChangedCount(const fmt::ProjectDesc& p, const std::vector<PhysicsSettingRow>& rows) {
    int n = 0;
    for (const PhysicsSettingRow& r : rows) if (physicsSettingChanged(p, r)) ++n;
    return n;
}

// Sets a setting on the project (clamped; a value at the default removes the entry) and, when `applyLive`, on the
// physics module. Returns true when the project changed. NaN is refused and changes nothing.
inline bool setPhysicsSettingValue(fmt::ProjectDesc& p, const PhysicsSettingRow& r, float v, bool applyLive = true) {
    if (std::isnan(v)) return false;
    const float c = physicsSettingClamp(r, v);
    if (applyLive) aver_phys_setting_set(r.key, c);
    auto it = detail::findStated(p, r.name);
    if (detail::nearDefault(c, r.def)) {
        if (it == p.physicsSettings.end()) return false;
        p.physicsSettings.erase(it);
        return true;
    }
    if (it != p.physicsSettings.end()) {
        if (it->second == c) return false;
        it->second = c;
        return true;
    }
    p.physicsSettings.emplace_back(r.name, c);
    return true;
}

inline bool resetPhysicsSetting(fmt::ProjectDesc& p, const PhysicsSettingRow& r, bool applyLive = true) {
    if (applyLive) aver_phys_setting_reset(r.key);
    auto it = detail::findStated(p, r.name);
    if (it == p.physicsSettings.end()) return false;
    p.physicsSettings.erase(it);
    return true;
}

// ---- limits ---------------------------------------------------------------------------------------------

// The ProjectDesc field behind a limit, and the value that means "unstated" there.
inline int* physicsLimitField(fmt::ProjectDesc& p, int key, int& unstated) {
    unstated = -1;
    switch (key) {
        case AVER_PHYS_CF_MAX_BODIES:     return &p.physMaxBodies;
        case AVER_PHYS_CF_MAX_BODY_PAIRS: return &p.physMaxBodyPairs;
        case AVER_PHYS_CF_MAX_CONTACTS:   return &p.physMaxContacts;
        case AVER_PHYS_CF_TEMP_MB:        return &p.physTempAllocatorMb;
        case AVER_PHYS_CF_THREADS:        unstated = -2; return &p.physThreads;
        default:                          return nullptr;
    }
}

inline bool physicsLimitStated(const fmt::ProjectDesc& p, const PhysicsLimitRow& r) {
    int unstated = 0;
    const int* f = physicsLimitField(const_cast<fmt::ProjectDesc&>(p), r.key, unstated);
    return f && *f != unstated;
}

inline float physicsLimitValue(const fmt::ProjectDesc& p, const PhysicsLimitRow& r) {
    int unstated = 0;
    const int* f = physicsLimitField(const_cast<fmt::ProjectDesc&>(p), r.key, unstated);
    return f && *f != unstated ? static_cast<float>(*f) : r.def;
}

inline bool physicsLimitChanged(const fmt::ProjectDesc& p, const PhysicsLimitRow& r) {
    return physicsLimitStated(p, r) && physicsLimitValue(p, r) != r.def;
}

// Sets a limit on the project (a value at the default is stored as unstated) and, when `applyLive`, as the
// physics module's PENDING config. Returns true when the project changed.
inline bool setPhysicsLimit(fmt::ProjectDesc& p, const PhysicsLimitRow& r, float v, bool applyLive = true) {
    if (std::isnan(v)) return false;
    const float c = std::round(std::min(std::max(v, r.min), r.max));
    if (applyLive) aver_phys_config_set(r.key, c);
    int unstated = 0;
    int* f = physicsLimitField(p, r.key, unstated);
    if (!f) return false;
    const int next = c == r.def ? unstated : static_cast<int>(c);
    if (*f == next) return false;
    *f = next;
    return true;
}

inline bool resetPhysicsLimit(fmt::ProjectDesc& p, const PhysicsLimitRow& r, bool applyLive = true) {
    if (applyLive) aver_phys_config_reset(r.key);
    int unstated = 0;
    int* f = physicsLimitField(p, r.key, unstated);
    if (!f || *f == unstated) return false;
    *f = unstated;
    return true;
}

// The red "restart world" badge: a world exists and was built with other limits than the pending ones.
inline bool physicsRestartBadge() { return aver_phys_config_differs() != 0; }

// The "Rebuild world" button is live only when the rebuild would be accepted (nothing in the world).
inline bool physicsRebuildEnabled() { return aver_phys_world_empty() != 0; }

#endif  // AVER_MODULE_PHYSICS

// The page's draw call (PhysicsSettingsPage.cpp): the five groups below gravity and tick rate. `dpi` scales
// widget widths; `dirty` is set when the project changed. A no-op in a build without ImGui or physics.
void drawPhysicsSettingsGroups(fmt::ProjectDesc& project, bool& dirty, float dpi);

} // namespace aver::editor
