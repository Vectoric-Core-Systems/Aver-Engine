// PhysicsDeterminism.cpp -- the diagnostics preset's hooks and aver_phys_diag_report (physics_world_abi.h; wave 11
// WORLD_STATE_DEBUG phase F).
//
// The always-on determinism tools (the state hash, the per-step hash log, step_exact) live in PhysicsState.cpp and work in every
// build. This file is only what needs the heavy build, AVER_PHYSICS_DIAGNOSTICS (modules/physics.jolt/CMakeLists.txt):
//
//   * JPH_ENABLE_DETERMINISM_LOG: Jolt writes detlog.txt (in the working directory, or in the directory aver_phys_diag_log_dir
//     names) with the values it computes; diff two runs to find the first diverging VALUE rather than the first diverging step.
//     The file is opened when a world is created and closed when it is torn down, so a rebuild starts a new one.
//   * the stats-tracking defines: aver_phys_diag_report prints the broad-phase, narrow-phase and per-body simulation reports to
//     the engine log (Jolt sends them through JPH::Trace, which traceImpl already forwards).
//   * Jolt's INTERNAL profiler (JPH_PROFILE_ENABLED), which takes the place of the external hook in that preset: the profiler is
//     started with the first world, marks a frame at the end of every fixed step, and aver_phys_diag_report(4) writes its HTML
//     dump to the working directory. It is never stopped: worker threads hold records into it until the process ends.
//
// In every other build all of it compiles to nothing and aver_phys_diag_report answers "not available" (Unsupported), so the C#
// parity test sees no conditional ABI.
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_world_abi.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Physics/Collision/NarrowPhaseStats.h>
#include <Jolt/Physics/DeterminismLog.h>

#include <string>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace aver::physics::detail {

// Called by makeJobSystem, i.e. before the job system (whose workers register with the profiler as they start) and before the
// physics system exist.
void diagOnWorldInit() {
#ifdef JPH_PROFILE_ENABLED
    {
        using namespace JPH;
        if (Profiler::sInstance == nullptr) { JPH_PROFILE_START("Aver main"); }
    }
#endif
#ifdef JPH_ENABLE_DETERMINISM_LOG
    {
        using namespace JPH;
        JPH_DET_LOG_OPEN();
    }
#endif
}

// The end of every fixed step (stepWorldOnce).
void diagOnStepEnd() {
#ifdef JPH_PROFILE_ENABLED
    {
        using namespace JPH;
        if (Profiler::sInstance != nullptr) { JPH_PROFILE_NEXTFRAME(); }
    }
#endif
}

// The world teardown (stateOnShutdown, the first hook aver_phys_shutdown reaches).
void diagOnWorldShutdown() {
#ifdef JPH_ENABLE_DETERMINISM_LOG
    {
        using namespace JPH;
        JPH_DET_LOG_CLOSE();
    }
#endif
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_diag_log_dir(const char* dirUtf8) {
#ifdef JPH_ENABLE_DETERMINISM_LOG
    // Jolt keeps the pointer, so the string has to outlive every later Open.
    static std::string s_dir;
    if (dirUtf8 && dirUtf8[0]) { s_dir = dirUtf8; JPH::DeterminismLog::sBasePath = s_dir.c_str(); }
    else JPH::DeterminismLog::sBasePath = nullptr;
    aver::setAbiError(AbiError::Ok);
    return 1;
#else
    (void)dirUtf8;
    aver::setAbiError(AbiError::Unsupported);
    return 0;
#endif
}

int32_t aver_phys_diag_report(int32_t kind) {
    int32_t done = 0;
    if (g_world) {
        switch (kind) {
            case 1:
#ifdef JPH_TRACK_BROADPHASE_STATS
                g_world->system.ReportBroadphaseStats();
                done = 1;
#endif
                break;
            case 2:
#if defined(JPH_TRACK_SIMULATION_STATS) && defined(JPH_PROFILE_ENABLED)
                g_world->system.ReportSimulationStats();
                done = 1;
#endif
                break;
            case 3:
#ifdef JPH_TRACK_NARROWPHASE_STATS
                JPH::NarrowPhaseStat::sReportStats();
                done = 1;
#endif
                break;
            case 4:
#ifdef JPH_PROFILE_ENABLED
                if (JPH::Profiler::sInstance != nullptr) {
                    using namespace JPH;
                    JPH_PROFILE_DUMP("aver");
                    done = 1;
                }
#endif
                break;
            default:
                aver::setAbiError(AbiError::OutOfRange);
                return 0;
        }
    } else {
        aver::setAbiError(AbiError::NotInitialised);
        return 0;
    }
    if (!done) {
        AVER_INFO("[Physics] diagnostic report {} is not available in this build (needs the AVER_PHYSICS_DIAGNOSTICS preset)", kind);
        aver::setAbiError(AbiError::Unsupported);
        return 0;
    }
    aver::setAbiError(AbiError::Ok);
    return 1;
}

} // extern "C"
