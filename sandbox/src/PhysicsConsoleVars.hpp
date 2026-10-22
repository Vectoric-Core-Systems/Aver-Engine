#pragma once
// phys.* console variables and commands (wave 11, WORLD_STATE_DEBUG): the live solver settings of
// physics_world_abi.h as `phys.<name>`, the read-only `phys.stat.*` family from aver_phys_stats, and the
// phys.stats / phys.config / phys.rebuild / phys.profile commands, and the state-recording commands phys.snapshot / phys.restore /
// phys.hash / phys.hashlog (physics_state_abi.h), the determinism tools phys.build / phys.report, and phys.shapecache.
//
// NOT HEADER-ONLY, unlike registerOcclusionVars, and included by exactly ONE translation unit (PhysicsSettingsPage.cpp):
// EditorConsole.hpp reaches every SandboxApp* file, several at the Debug COFF section cap, and these lambdas would add
// their sections to each one that references buildVarTable(). EditorConsole.hpp only declares the two functions.
//
// One loop over aver_phys_setting_count() produces every variable, so a setting added to the physics table
// appears here with its unit, range and default and nothing else to edit. `--set phys.velocitySteps 14` then
// works from the command line exactly like `--set voxi.x` (SandboxMain's --set path walks this table).
#include "EditorConsole.hpp"

#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_state_abi.h"
#include "aver/physics/physics_world_abi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace aver::editor {

namespace detail {

// Names of the aver_phys_stats slots, in AVER_PHYS_ST_* order.
inline constexpr const char* kPhysStatNames[] = {
    "bodies", "maxBodies", "static", "dynamic", "activeDynamic", "kinematic", "activeKinematic", "soft",
    "activeSoft", "constraints", "characters", "vehicles", "stepsLastCall", "stepsTotal", "stepMsLast",
    "stepMsAvg", "stepMsMax", "callMsLast", "contactsLastCall", "updateErrors", "updateErrorSteps",
    "tempPeakBytes", "tempSizeBytes", "workerThreads", "accumulatorS", "topologyEpoch",
};
static_assert(sizeof(kPhysStatNames) / sizeof(kPhysStatNames[0]) == AVER_PHYS_ST_COUNT,
              "one name per AVER_PHYS_ST_* slot");

// The slots that are fractional (milliseconds, seconds); the rest are counts.
inline bool physStatIsFloat(int k) {
    return k == AVER_PHYS_ST_STEP_MS_LAST || k == AVER_PHYS_ST_STEP_MS_AVG || k == AVER_PHYS_ST_STEP_MS_MAX ||
           k == AVER_PHYS_ST_CALL_MS_LAST || k == AVER_PHYS_ST_ACCUMULATOR_S;
}

inline double physStat(int k) {
    double v[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(v, AVER_PHYS_ST_COUNT);
    return v[k];
}

inline std::string physFmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

// The console's snapshot slots: a small number to the native snapshot handle. A handle that died with its world is simply
// captured into afresh (the native capture allocates a new one for a dead `reuse`).
inline std::map<int, int>& physSnapshotSlots() {
    static std::map<int, int> slots;
    return slots;
}

inline const char* physRestoreResultName(int r) {
    switch (r) {
        case AVER_PHYS_RESTORE_OK: return "ok";
        case AVER_PHYS_RESTORE_BAD_HANDLE: return "no such snapshot (empty slot, or its world is gone)";
        case AVER_PHYS_RESTORE_BAD_HEADER: return "not a snapshot";
        case AVER_PHYS_RESTORE_BUILD_MISMATCH: return "taken by another build";
        case AVER_PHYS_RESTORE_TOPOLOGY: return "the world changed: a captured body, character, vehicle or joint is gone, or a constraint was added or removed";
        case AVER_PHYS_RESTORE_CORRUPT: return "corrupt";
        default: return "unknown";
    }
}

// Parses an optional slot argument (default 1). False, with `err` set, for anything but a small whole number.
inline bool physSlotArg(const std::vector<std::string>& args, size_t at, int& slot, std::string& err) {
    slot = 1;
    if (args.size() <= at) return true;
    char* end = nullptr;
    const long v = std::strtol(args[at].c_str(), &end, 10);
    if (end == args[at].c_str() || *end != 0 || v < 0 || v > 99) { err = "The slot is a whole number from 0 to 99"; return false; }
    slot = static_cast<int>(v);
    return true;
}

} // namespace detail

void registerPhysicsVars(std::vector<ConsoleVar>& t) {
    const int n = aver_phys_setting_count();
    for (int k = 0; k < n; ++k) {
        float mn = 0.0f, mx = 0.0f, df = 0.0f;
        aver_phys_setting_range(k, &mn, &mx, &df);
        const std::string unit = aver_phys_setting_unit(k);
        const VarType type = unit == "bool" ? VarType::Bool : (unit == "count" ? VarType::U32 : VarType::F32);

        ConsoleVar v;
        v.name = std::string("phys.") + aver_phys_setting_name(k);
        v.type = type;
        v.help = std::string("Physics solver setting (Jolt), unit ") + unit + ", range " + detail::physFmt(mn) + ".." +
                 detail::physFmt(mx) + ", default " + detail::physFmt(df) +
                 ". Applies to the live world at once; the project key is PHYSICS." +
                 std::string(aver_phys_setting_name(k)) + " (upper-cased).";
        v.read = [k, type]() -> VarValue {
            float x = 0.0f;
            aver_phys_setting_get(k, &x);
            if (type == VarType::Bool) return vBool(x != 0.0f);
            if (type == VarType::U32) return vU32(static_cast<u32>(std::lround(x)));
            return vF32(x);
        };
        v.stage = [k](ConsoleBatch& b, VarValue val) {
            const float x = val.type == VarType::Bool ? (val.as.b ? 1.0f : 0.0f)
                          : (val.type == VarType::U32 ? static_cast<float>(val.as.u) : val.as.f);
            b.deviceSetters.push_back([k, x](rhi::IDevice&) { aver_phys_setting_set(k, x); });
        };
        v.validate = [mn, mx, type](const VarValue& val, std::string& err) -> bool {
            if (type == VarType::Bool) return true;
            const float x = val.type == VarType::U32 ? static_cast<float>(val.as.u) : val.as.f;
            if (std::isnan(x) || x < mn || x > mx) {
                err = "must be within " + detail::physFmt(mn) + ".." + detail::physFmt(mx);
                return false;
            }
            return true;
        };
        t.push_back(std::move(v));
    }

    // phys.stat.* -- read-only, from the last fixed steps.
    for (int k = 0; k < AVER_PHYS_ST_COUNT; ++k) {
        const bool isFloat = detail::physStatIsFloat(k);
        ConsoleVar v;
        v.name = std::string("phys.stat.") + detail::kPhysStatNames[k];
        v.type = isFloat ? VarType::F32 : VarType::U32;
        v.readOnly = true;
        v.help = "Physics statistic (read-only), from aver_phys_stats; all zero with no world. phys.stats prints them all.";
        v.read = [k, isFloat]() -> VarValue {
            const double x = detail::physStat(k);
            return isFloat ? vF32(static_cast<f32>(x)) : vU32(static_cast<u32>(x < 0.0 ? 0.0 : x));
        };
        t.push_back(std::move(v));
    }
}

void registerPhysicsCommands(std::vector<ConsoleCommandDesc>& t) {
    t.push_back({"phys.stats", "Print the physics statistics (bodies, step time, update errors, threads, temp memory)",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            double v[AVER_PHYS_ST_COUNT] = {};
            aver_phys_stats(v, AVER_PHYS_ST_COUNT);
            if (!aver_phys_ready()) { print(LogLevel::Warn, "There is no physics world."); return true; }
            for (int k = 0; k < AVER_PHYS_ST_COUNT; ++k) {
                char line[96];
                std::snprintf(line, sizeof line, "phys.stat.%s = %g", detail::kPhysStatNames[k], v[k]);
                // A dropped-contact bit set is the one line that should not be missed.
                print(k == AVER_PHYS_ST_UPDATE_ERRORS && v[k] != 0.0 ? LogLevel::Error : LogLevel::Info, line);
            }
            return true;
        }});
    t.push_back({"phys.config",
        "phys.config [<name> <value>] -- print the world limits (maxBodies, maxBodyPairs, maxContacts, tempAllocatorMb, "
        "threads) as pending and active, or set a pending one; it applies when the world is rebuilt (phys.rebuild)",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            if (args.size() >= 2) {
                int key = -1;
                for (int k = 0; k < AVER_PHYS_CF_COUNT; ++k) {
                    const char* nm = aver_phys_config_name(k);
                    if (nm && detail::ciEquals(nm, args[0])) { key = k; break; }
                }
                if (key < 0) { print(LogLevel::Error, "Unknown limit '" + args[0] + "'. Nothing was set."); return false; }
                f32 val = 0.0f; std::string err;
                if (!parseF32(args[1], val, err)) { print(LogLevel::Error, err + ". Nothing was set."); return false; }
                if (!aver_phys_config_set(key, val)) { print(LogLevel::Error, "Refused. Nothing was set."); return false; }
            }
            for (int k = 0; k < AVER_PHYS_CF_COUNT; ++k) {
                float pending = 0.0f, active = 0.0f;
                aver_phys_config_get(k, &pending);
                const bool live = aver_phys_config_active(k, &active) != 0;
                std::string line = std::string(aver_phys_config_name(k)) + " = " + detail::physFmt(pending);
                if (live && active != pending) line += "  (world has " + detail::physFmt(active) + "; phys.rebuild applies it)";
                print(LogLevel::Info, line);
            }
            return true;
        }});
    t.push_back({"phys.rebuild",
        "Rebuild the physics world with the pending limits (phys.config). Refused unless the world is empty.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            if (!aver_phys_world_empty()) {
                print(LogLevel::Error, "The world is not empty (bodies, characters, joints or vehicles exist); close the level first.");
                return false;
            }
            if (!aver_phys_rebuild()) { print(LogLevel::Error, "Rebuild failed; see the log."); return false; }
            print(LogLevel::Info, "Physics world rebuilt.");
            return true;
        }});
    t.push_back({"phys.profile",
        "phys.profile [on|off|reset] -- the physics profiler: Jolt's own scopes and the fixed-step stages as one tree. "
        "With no argument it prints the last published table (per fixed step, ms: inclusive / exclusive / max, calls).",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            if (!(aver_phys_build_flags() & AVER_PHYS_BUILD_PROFILER)) {
                print(LogLevel::Error, "This build has no profiler hook (AVER_PHYSICS_PROFILER is off).");
                return false;
            }
            if (!args.empty()) {
                if (detail::ciEquals("on", args[0])) aver_phys_profile_enable(1);
                else if (detail::ciEquals("off", args[0])) aver_phys_profile_enable(0);
                else if (detail::ciEquals("reset", args[0])) aver_phys_profile_reset();
                else { print(LogLevel::Error, "Expected on, off or reset."); return false; }
                print(LogLevel::Info, aver_phys_profile_is_enabled() ? "Physics profiler on." : "Physics profiler off.");
                return true;
            }
            const int n = aver_phys_profile_count();
            if (n == 0) {
                print(LogLevel::Info, aver_phys_profile_is_enabled() ? "No table yet: step a few frames." : "The profiler is off: phys.profile on");
                return true;
            }
            for (int i = 0; i < n; ++i) {
                uint8_t name[96] = {};
                double v[4] = {};
                int pd[2] = {};
                if (!aver_phys_profile_get(i, name, static_cast<int32_t>(sizeof name), v, pd)) break;
                char line[192];
                std::snprintf(line, sizeof line, "%*s%s  %.3f / %.3f / %.3f ms  x%.1f", pd[1] * 2, "",
                              reinterpret_cast<const char*>(name), v[0], v[1], v[2], v[3]);
                print(LogLevel::Info, line);
            }
            return true;
        }});
    t.push_back({"phys.snapshot",
        "phys.snapshot [slot] -- capture the whole physics state (bodies, contacts, constraints, characters, vehicle aids) into a "
        "numbered slot (default 1), overwriting it. phys.restore puts it back.",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            int slot = 1; std::string err;
            if (!detail::physSlotArg(args, 0, slot, err)) { print(LogLevel::Error, err); return false; }
            if (!aver_phys_ready()) { print(LogLevel::Error, "There is no physics world."); return false; }
            auto& slots = detail::physSnapshotSlots();
            const auto prev = slots.find(slot);
            const int h = aver_phys_snapshot_capture(prev == slots.end() ? 0 : prev->second, 0);
            if (!h) { print(LogLevel::Error, "The capture failed; see the log."); return false; }
            slots[slot] = h;
            char line[160];
            std::snprintf(line, sizeof line, "Snapshot %d: %d bytes, hash %016llx, fixed step %lld.", slot, aver_phys_snapshot_bytes(h),
                          static_cast<unsigned long long>(aver_phys_snapshot_hash(h)), static_cast<long long>(aver_phys_step_total()));
            print(LogLevel::Info, line);
            return true;
        }});
    t.push_back({"phys.restore",
        "phys.restore [slot] [strict] -- put the physics world back as phys.snapshot held it. Refused, with nothing changed, when a "
        "captured body, character, vehicle or joint is gone or a constraint was added or removed; strict also refuses when "
        "something was added.",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            int slot = 1; std::string err;
            if (!detail::physSlotArg(args, 0, slot, err)) { print(LogLevel::Error, err); return false; }
            const bool strict = args.size() > 1 && detail::ciEquals("strict", args[1]);
            if (args.size() > 1 && !strict) { print(LogLevel::Error, "The only option is strict."); return false; }
            const auto& slots = detail::physSnapshotSlots();
            const auto it = slots.find(slot);
            if (it == slots.end()) { print(LogLevel::Error, "Slot " + std::to_string(slot) + " is empty: phys.snapshot first."); return false; }
            if (!aver_phys_snapshot_restore(it->second, strict ? AVER_PHYS_RESTORE_STRICT : 0u)) {
                print(LogLevel::Error, std::string("Restore refused, nothing changed: ") +
                                           detail::physRestoreResultName(aver_phys_snapshot_last_result()));
                return false;
            }
            char line[96];
            std::snprintf(line, sizeof line, "Restored slot %d (fixed step %lld, hash %016llx).", slot,
                          static_cast<long long>(aver_phys_step_total()), static_cast<unsigned long long>(aver_phys_state_hash(0)));
            print(LogLevel::Info, line);
            return true;
        }});
    t.push_back({"phys.hash",
        "Print the hash of the whole physics state at this instant. Two runs, two machines or two thread counts that reach the same "
        "state print the same value; phys.hashlog finds the step where they stop doing so.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            if (!aver_phys_ready()) { print(LogLevel::Error, "There is no physics world."); return false; }
            char line[96];
            std::snprintf(line, sizeof line, "%016llx  (fixed step %lld)", static_cast<unsigned long long>(aver_phys_state_hash(0)),
                          static_cast<long long>(aver_phys_step_total()));
            print(LogLevel::Info, line);
            return true;
        }});
    t.push_back({"phys.build",
        "Print what this physics binary is: precision, Jolt's configuration string and the optional features compiled in. Two peers "
        "that exchange state, or a determinism claim, compare this line first.",
        [](SandboxApp&, Engine&, const std::vector<std::string>&, const ConsolePrint& print) -> bool {
            const uint32_t f = aver_phys_build_flags();
            std::string line = aver_phys_precision() == AVER_PHYS_PRECISION_DOUBLE ? "precision: double" : "precision: single";
            line += std::string("; debug renderer ") + ((f & AVER_PHYS_BUILD_DEBUG_RENDERER) ? "on" : "off");
            line += std::string("; profiler hook ") + ((f & AVER_PHYS_BUILD_PROFILER) ? "on" : "off");
            line += std::string("; cross-platform deterministic ") + ((f & AVER_PHYS_BUILD_CROSS_PLATFORM) ? "ON" : "off");
            line += std::string("; determinism log ") + ((f & AVER_PHYS_BUILD_DETERMINISM_LOG) ? "ON" : "off");
            line += std::string("; stats tracking ") + ((f & AVER_PHYS_BUILD_STATS_TRACKING) ? "ON" : "off");
            line += std::string("; asserts ") + ((f & AVER_PHYS_BUILD_ASSERTS) ? "on" : "off");
            print(LogLevel::Info, line);
            const int32_t need = aver_phys_build_string(nullptr, 0);
            std::string s(static_cast<size_t>(need > 0 ? need : 1), '\0');
            aver_phys_build_string(reinterpret_cast<uint8_t*>(s.data()), need);
            s.resize(std::strlen(s.c_str()));
            print(LogLevel::Info, s);
            return true;
        }});
    t.push_back({"phys.report",
        "phys.report broadphase|simulation|narrowphase|profile -- the diagnostics preset's reports (a build with AVER_PHYSICS_DIAGNOSTICS): "
        "the first three go to the log, profile writes Jolt's HTML dump to the working directory.",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            int kind = 0;
            if (args.size() == 1) {
                if (detail::ciEquals("broadphase", args[0])) kind = 1;
                else if (detail::ciEquals("simulation", args[0])) kind = 2;
                else if (detail::ciEquals("narrowphase", args[0])) kind = 3;
                else if (detail::ciEquals("profile", args[0])) kind = 4;
            }
            if (kind == 0) { print(LogLevel::Error, "Usage: phys.report broadphase|simulation|narrowphase|profile"); return false; }
            if (!aver_phys_ready()) { print(LogLevel::Error, "There is no physics world."); return false; }
            if (!aver_phys_diag_report(kind)) {
                print(LogLevel::Error, "This build cannot produce that report (configure with -DAVER_PHYSICS_DIAGNOSTICS=ON).");
                return false;
            }
            print(LogLevel::Info, kind == 4 ? "Profiler dump written to the working directory." : "Report written to the log.");
            return true;
        }});
    t.push_back({"phys.shapecache",
        "phys.shapecache [on|off] -- the physics shape binary cache (mesh and heightfield BVHs kept in Saved/DerivedDataCache/Physics). "
        "Off by default; PHYSICS.SHAPECACHE in the project does the same. It applies to levels loaded after the change.",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            if (!args.empty()) {
                if (detail::ciEquals("on", args[0])) aver_phys_shape_cache_enable(1);
                else if (detail::ciEquals("off", args[0])) aver_phys_shape_cache_enable(0);
                else { print(LogLevel::Error, "Usage: phys.shapecache [on|off]"); return false; }
            }
            print(LogLevel::Info, std::string("The physics shape cache is ") + (aver_phys_shape_cache_enabled() ? "on" : "off") + ".");
            return true;
        }});
    t.push_back({"phys.hashlog",
        "phys.hashlog on <path> | off -- write one step-and-hash line per fixed step to the file (it is created afresh), so two runs "
        "diff directly. Costs about a snapshot per step; off by default.",
        [](SandboxApp&, Engine&, const std::vector<std::string>& args, const ConsolePrint& print) -> bool {
            if (args.size() == 1 && detail::ciEquals("off", args[0])) {
                aver_phys_hash_log(nullptr);
                print(LogLevel::Info, "Hash log closed.");
                return true;
            }
            if (args.size() == 2 && detail::ciEquals("on", args[0])) {
                if (!aver_phys_hash_log(args[1].c_str())) { print(LogLevel::Error, "Could not open " + args[1] + " (is there a physics world?)."); return false; }
                print(LogLevel::Info, "Hash log on: " + args[1]);
                return true;
            }
            print(LogLevel::Error, "Usage: phys.hashlog on <path> | off");
            return false;
        }});
}

} // namespace aver::editor
#endif
