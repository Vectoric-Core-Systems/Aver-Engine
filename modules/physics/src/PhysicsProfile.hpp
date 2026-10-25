// PhysicsProfile.hpp -- the profiler bridge (physics_world_abi.h, aver_phys_profile_*).
//
// STD-ONLY ON PURPOSE: AverProfileHook.cpp lives in the Jolt target (modules/physics.jolt), which links neither Aver.Core nor
// Aver.Physics, and includes this file by relative path for the Sink below. Everything Jolt-shaped stays in PhysicsProfile.cpp.
//
// How it fits: with JPH_EXTERNAL_PROFILE every JPH_PROFILE scope in Jolt, and every AVER_PHYS_SCOPE of ours, constructs a
// JPH::ExternalProfileMeasurement. The hook reads g_sink; null (the default) is a relaxed load and a branch, and nothing else.
// aver_phys_profile_enable(1) installs the collector's sink; each thread then keeps its own scope tree, and the main thread
// merges them by NAME TEXT (the same literal in two translation units may have two addresses) when no job is running.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace aver::physics::profile {

// What the hook calls. userData is the 56 bytes of ExternalProfileMeasurement's storage after the hook's own pointer.
struct Sink {
    void (*begin)(const char* name, uint32_t color, uint8_t* userData);
    void (*end)(uint8_t* userData);
};

#ifdef JPH_EXTERNAL_PROFILE
// Defined in AverProfileHook.cpp (inside the Jolt target, because PhysicsTest links Jolt directly and the symbol must resolve there too).
extern std::atomic<const Sink*> g_sink;
#endif

// One published row: a scope, in depth-first order so a reader indents by `depth`.
struct Row {
    std::string name;
    double inclusiveMs = 0.0;     // average per fixed step over the window
    double exclusiveMs = 0.0;     // inclusive minus the children's
    double maxMs = 0.0;           // largest per-step inclusive of any call in the window
    double callsPerStep = 0.0;
    int32_t parent = -1;          // index into the published table; -1 = root
    int32_t depth = 0;
};

bool compiled();                  // the hook is built in (JPH_EXTERNAL_PROFILE)
bool enabled();
bool setEnabled(bool on);         // false when the build has no hook
void reset();                     // drops the table and the window; counters restart
// Called at the end of every aver_phys_step with the fixed steps that call ran: merges the thread trees and, every 60 steps,
// publishes. Main thread, no job running. A no-op while disabled.
void mergeCall(int fixedSteps);
int rowCount();
bool getRow(int index, Row& out);
// "Name 1.2 ms | Name 0.8 ms | ..." for the three largest exclusive scopes of the last window ("" when there is no table).
std::string topSummary();

} // namespace aver::physics::profile
