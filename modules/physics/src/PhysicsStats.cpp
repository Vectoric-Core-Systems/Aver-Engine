// PhysicsStats.cpp -- the fixed step's timing and error bookkeeping, and aver_phys_stats (physics_world_abi.h).
// The Update call runs here (stepWorldOnce), so the collision-step setting, the kept EPhysicsUpdateError and the
// wall-clock pair live in one place.
#include "PhysicsConfig.hpp"
#include "PhysicsProfile.hpp"
#include "PhysicsOrigin.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// The error bits are logged ONCE per world (the warnBodyLimit pattern) as a single line naming each cause and
// its cure, with the limit the world is running at now. A world that overflows every step must not bury the log.
void noteUpdateError(World& w, JPH::EPhysicsUpdateError err) {
    const uint32_t bits = static_cast<uint32_t>(err);
    w.stats.updateErrors |= bits;
    ++w.stats.updateErrorSteps;
    if (w.stats.updateErrorLogged) return;
    w.stats.updateErrorLogged = true;
    std::string msg;
    auto add = [&](const char* what, const char* key, int cf) {
        if (!msg.empty()) msg += "; ";
        msg += what;
        msg += ": raise ";
        msg += key;
        msg += " (now ";
        msg += std::to_string(static_cast<long long>(w.activeConfig[cf]));
        msg += ") and rebuild the world";
    };
    if (bits & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ManifoldCacheFull))
        add("manifold cache full", "PHYSICS.MAXCONTACTS", AVER_PHYS_CF_MAX_CONTACTS);
    if (bits & static_cast<uint32_t>(JPH::EPhysicsUpdateError::BodyPairCacheFull))
        add("body pair cache full", "PHYSICS.MAXBODYPAIRS", AVER_PHYS_CF_MAX_BODY_PAIRS);
    if (bits & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ContactConstraintsFull))
        add("contact constraints full", "PHYSICS.MAXCONTACTS", AVER_PHYS_CF_MAX_CONTACTS);
    AVER_WARN("[Physics] simulation dropped contacts, {}", msg);
}

} // namespace

namespace aver::physics::detail {

void stepWorldOnce() {
    World& w = *g_world;
    AVER_PHYS_SCOPE("PhysicsStep");   // Jolt's own "Update" scope and everything under it nest here
    debugStepBegin();
    const auto t0 = Clock::now();
    const JPH::EPhysicsUpdateError err =
        w.system.Update(w.simStep(), collisionStepsSetting(), w.temp.get(), w.jobs.get());
    const double ms = msSince(t0);
    debugStepEnd();
    diagOnStepEnd();

    ++w.stepsTotal;
    ++w.stats.stepsThisCall;
    StepStats& s = w.stats;
    s.stepMsLast = ms;
    if (ms > s.stepMsMax) s.stepMsMax = ms;
    // 0.5 s time constant, the same smoothing PlayProfile uses; the first step seeds it.
    const double a = 1.0 - std::exp(-static_cast<double>(w.fixedStep) / 0.5);
    s.stepMsAvg = w.stepsTotal <= 1 ? ms : s.stepMsAvg + (ms - s.stepMsAvg) * a;
    if (err != JPH::EPhysicsUpdateError::None) noteUpdateError(w, err);
}

StepCallScope::StepCallScope() : t0_(Clock::now()) {
    if (g_world) g_world->stats.stepsThisCall = 0;
}

StepCallScope::~StepCallScope() {
    if (!g_world) return;
    StepStats& s = g_world->stats;
    s.stepsLastCall = s.stepsThisCall;
    s.contactsLastCall = static_cast<int32_t>(g_world->contacts.size());   // no job is running: the call is over
    s.callMsLast = msSince(t0_);
    profile::mergeCall(s.stepsThisCall);   // no job is running: the call is over
    // A hitch in the step itself explains itself when the profiler is on: the same AVER_HITCH_MS threshold the HitchMarks
    // scopes use, and the largest scopes of the last published window (not of the slow step alone).
    if (profile::enabled() && s.callMsLast > hitchThresholdMs() && hitchThresholdMs() > 0.0) {
        AVER_INFO("[Physics] slow step call {:.1f} ms ({} fixed step(s)); largest scopes: {}", s.callMsLast, s.stepsThisCall,
                  profile::topSummary());
    }
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_stats(double* outValues, int32_t n) {
    if (!outValues || n <= 0) return 0;
    const int32_t w = n < AVER_PHYS_ST_COUNT ? n : AVER_PHYS_ST_COUNT;
    double v[AVER_PHYS_ST_COUNT] = {};
    if (g_world) {
        World& world = *g_world;
        const JPH::BodyManager::BodyStats b = world.system.GetBodyStats();
        // World-joint anchors (PhysicsJoints.cpp makeAnchor) are bodies to Jolt's manager but not to this module: leave them out so the
        // counts are what they were before the floating origin.
        const double anchors = static_cast<double>(originJointsCount());
        v[AVER_PHYS_ST_BODIES] = b.mNumBodies - anchors;
        v[AVER_PHYS_ST_MAX_BODIES] = b.mMaxBodies;
        v[AVER_PHYS_ST_STATIC] = b.mNumBodiesStatic - anchors;
        v[AVER_PHYS_ST_DYNAMIC] = b.mNumBodiesDynamic;
        v[AVER_PHYS_ST_ACTIVE_DYNAMIC] = b.mNumActiveBodiesDynamic;
        v[AVER_PHYS_ST_KINEMATIC] = b.mNumBodiesKinematic;
        v[AVER_PHYS_ST_ACTIVE_KINEMATIC] = b.mNumActiveBodiesKinematic;
        v[AVER_PHYS_ST_SOFT] = b.mNumSoftBodies;
        v[AVER_PHYS_ST_ACTIVE_SOFT] = b.mNumActiveSoftBodies;
        v[AVER_PHYS_ST_CONSTRAINTS] = static_cast<double>(world.system.GetConstraints().size());
        v[AVER_PHYS_ST_CHARACTERS] = static_cast<double>(world.characters.size());
        v[AVER_PHYS_ST_VEHICLES] = static_cast<double>(world.vehicles.size());
        const StepStats& s = world.stats;
        v[AVER_PHYS_ST_STEPS_LAST_CALL] = s.stepsLastCall;
        v[AVER_PHYS_ST_STEPS_TOTAL] = static_cast<double>(world.stepsTotal);
        v[AVER_PHYS_ST_STEP_MS_LAST] = s.stepMsLast;
        v[AVER_PHYS_ST_STEP_MS_AVG] = s.stepMsAvg;
        v[AVER_PHYS_ST_STEP_MS_MAX] = s.stepMsMax;
        v[AVER_PHYS_ST_CALL_MS_LAST] = s.callMsLast;
        v[AVER_PHYS_ST_CONTACTS_LAST_CALL] = s.contactsLastCall;
        v[AVER_PHYS_ST_UPDATE_ERRORS] = s.updateErrors;
        v[AVER_PHYS_ST_UPDATE_ERROR_STEPS] = static_cast<double>(s.updateErrorSteps);
        if (world.temp) {
            v[AVER_PHYS_ST_TEMP_PEAK_BYTES] = static_cast<double>(world.temp->peak());
            v[AVER_PHYS_ST_TEMP_SIZE_BYTES] = static_cast<double>(world.temp->size());
        }
        v[AVER_PHYS_ST_WORKER_THREADS] = s.workers;
        v[AVER_PHYS_ST_ACCUMULATOR_S] = world.accumulator;
        v[AVER_PHYS_ST_TOPOLOGY_EPOCH] = world.topologyEpoch;
    }
    for (int32_t i = 0; i < w; ++i) outValues[i] = v[i];
    return w;
}

int32_t aver_phys_stats_reset_window(void) {
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return 0; }
    StepStats& s = g_world->stats;
    s.stepMsMax = 0.0;
    s.updateErrors = 0;
    s.updateErrorSteps = 0;
    if (g_world->temp) g_world->temp->resetPeak();
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

// aver_phys_diag_report and aver_phys_diag_log_dir are in PhysicsDeterminism.cpp (the diagnostics preset).

} // extern "C"
