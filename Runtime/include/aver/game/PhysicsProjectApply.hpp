// PhysicsProjectApply: applies the manifest's PHYSICS.* keys. Header-only like GameTick.hpp, since each host re-evaluates
// AVER_MODULE_PHYSICS in its own translation unit.
// Two moments, because physics starts before any project is known: configureProjectPhysics runs after the manifest parse and
// before the first level; applyProjectPhysicsKeys runs after the level load (gravity and fixed step need a live world).
// A project overrides only what it STATES: keys the previous project stated but this one does not revert to their defaults.
#pragma once
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_state_abi.h"
#  include "aver/physics/physics_world_abi.h"

#  include <cctype>
#  include <cstdint>
#  include <string>

namespace aver::game {

// What the last project stated, so the next one can undo it.
struct PhysicsAppliedKeys {
    std::uint64_t settings = 0;   // bit per AVER_PHYS_WS_* key
    std::uint32_t config = 0;     // bit per AVER_PHYS_CF_* key
    bool shapeCache = false;      // the previous project stated PHYSICS.SHAPECACHE
};
inline PhysicsAppliedKeys& physicsAppliedKeys() {
    static PhysicsAppliedKeys keys;
    return keys;
}

// "velocitySteps" -> "PHYSICS.VELOCITYSTEPS".
inline std::string physicsProjectKey(const char* camel) {
    std::string s = "PHYSICS.";
    for (const char* p = camel; *p; ++p) s += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    return s;
}

// Live solver settings, applied to a live world at once.
inline int applyProjectPhysicsSettings(const fmt::ProjectDesc& project) {
    PhysicsAppliedKeys& applied = physicsAppliedKeys();
    std::uint64_t stated = 0;
    int n = 0;
    for (const auto& [name, value] : project.physicsSettings) {
        const int key = aver_phys_setting_find(name.c_str());
        if (key < 0 || key >= 64) {
            AVER_WARN("[Project] {} is not a physics setting this build knows", physicsProjectKey(name.c_str()));
            continue;
        }
        if (!aver_phys_setting_set(key, value)) {
            AVER_WARN("[Project] {} {} refused", physicsProjectKey(name.c_str()), value);
            continue;
        }
        stated |= std::uint64_t{1} << key;
        float now = value;
        aver_phys_setting_get(key, &now);
        if (now != value)
            AVER_WARN("[Project] {} {} is outside the allowed range; using {}", physicsProjectKey(name.c_str()), value, now);
        ++n;
    }
    const int count = aver_phys_setting_count();
    for (int k = 0; k < count && k < 64; ++k)
        if ((applied.settings >> k & 1u) && !(stated >> k & 1u)) aver_phys_setting_reset(k);
    applied.settings = stated;
    return n;
}

// Init-time limits, read by the next world. A limit this project does not state goes back to its default if a previous one did.
inline int applyProjectPhysicsLimits(const fmt::ProjectDesc& project) {
    struct Row { int key; int value; bool stated; const char* name; };
    const Row rows[] = {
        {AVER_PHYS_CF_MAX_BODIES,     project.physMaxBodies,       project.physMaxBodies > 0,       "PHYSICS.MAXBODIES"},
        {AVER_PHYS_CF_MAX_BODY_PAIRS, project.physMaxBodyPairs,    project.physMaxBodyPairs > 0,    "PHYSICS.MAXBODYPAIRS"},
        {AVER_PHYS_CF_MAX_CONTACTS,   project.physMaxContacts,     project.physMaxContacts > 0,     "PHYSICS.MAXCONTACTS"},
        {AVER_PHYS_CF_TEMP_MB,        project.physTempAllocatorMb, project.physTempAllocatorMb > 0, "PHYSICS.TEMPALLOCMB"},
        {AVER_PHYS_CF_THREADS,        project.physThreads,         project.physThreads > -2,        "PHYSICS.THREADS"},
    };
    PhysicsAppliedKeys& applied = physicsAppliedKeys();
    std::uint32_t stated = 0;
    int n = 0;
    for (const Row& r : rows) {
        if (!r.stated) {
            if (applied.config >> r.key & 1u) aver_phys_config_reset(r.key);
            continue;
        }
        if (!aver_phys_config_set(r.key, static_cast<float>(r.value))) {
            AVER_WARN("[Project] {} {} refused", r.name, r.value);
            continue;
        }
        stated |= std::uint32_t{1} << r.key;
        float now = static_cast<float>(r.value);
        aver_phys_config_get(r.key, &now);
        if (now != static_cast<float>(r.value))
            AVER_WARN("[Project] {} {} is outside the allowed range; using {}", r.name, r.value, static_cast<int>(now));
        ++n;
    }
    applied.config = stated;
    return n;
}

// PHYSICS.SHAPECACHE is read by the level loaders as they build mesh shapes, so it must be in place before the first level.
// Without it the cache is off; AVER_PHYS_SHAPE_CACHE=1 still turns it on for tools.
inline void applyProjectPhysicsShapeCache(const fmt::ProjectDesc& project) {
    PhysicsAppliedKeys& applied = physicsAppliedKeys();
    if (project.physShapeCache >= 0) {
        aver_phys_shape_cache_enable(project.physShapeCache);
        applied.shapeCache = true;
    } else if (applied.shapeCache) {
        aver_phys_shape_cache_enable(0);
        applied.shapeCache = false;
    }
}

// Before the first level. A world built with other limits is rebuilt only when empty; a non-empty one keeps its limits
// until it is next created. True when the world was rebuilt.
inline bool configureProjectPhysics(const fmt::ProjectDesc& project, const char* hostTag) {
    applyProjectPhysicsSettings(project);
    applyProjectPhysicsLimits(project);
    applyProjectPhysicsShapeCache(project);
    // No world yet: the pending config is what the first aver_phys_init reads.
    if (!aver_phys_ready() || !aver_phys_config_differs()) return false;
    if (!aver_phys_world_empty()) {
        AVER_INFO("[Project] PHYSICS limits (MAXBODIES, MAXBODYPAIRS, MAXCONTACTS, TEMPALLOCMB, THREADS) "
                  "apply when the world is next created");
        return false;
    }
    if (aver_phys_rebuild()) {
        AVER_INFO("[{}] physics world rebuilt for the project's limits", hostTag);
        return true;
    }
    AVER_WARN("[{}] physics world could not be rebuilt for the project's limits", hostTag);
    return false;
}

// After the level load. Both setters are no-ops before aver_phys_init() succeeds, so readiness is checked here, not trusted
// from the caller: a project that states physics before the world exists is told the keys apply once it does.
inline void applyProjectPhysicsKeys(const fmt::ProjectDesc& project) {
    applyProjectPhysicsSettings(project);
    applyProjectPhysicsLimits(project);
    if (aver_phys_ready()) {
        if (project.hasGravity)
            aver_phys_set_gravity(project.gravity[0], project.gravity[1], project.gravity[2]);
        // The setter refuses a step outside (0, 0.5] by returning 0; checked so a bad step never reads as applied.
        if (project.fixedStep > 0.0f && !aver_phys_set_fixed_step(project.fixedStep))
            AVER_WARN("[Project] PHYSICS.FIXEDSTEP {} refused -- must be within (0, 0.5] seconds",
                      project.fixedStep);
        // Limits are fixed for a world's lifetime.
        if (aver_phys_config_differs())
            AVER_INFO("[Project] PHYSICS.MAXBODIES / MAXBODYPAIRS / MAXCONTACTS / TEMPALLOCMB / THREADS "
                      "change when the world is next created");
    } else if (project.hasGravity || project.fixedStep > 0.0f) {
        AVER_INFO("[Project] physics settings will apply once the world exists");
    }
}

} // namespace aver::game
#endif
