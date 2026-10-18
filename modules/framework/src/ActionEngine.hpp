#pragma once
// Trigger and modifier evaluation for the named-action layer (FrameworkAbi.cpp, NAMED ACTIONS).
// Pure functions over a value and a dt: no input state, no ABI types, so the semantics in
// docs/plans/wave2/ENHANCED_INPUT.md section 2 are testable and readable in one place. Internal to
// Aver.Framework (not exported, not in include/).
#include "aver/formats/OcInput.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aver::fwinput {

struct V3 {
    float c[3] = {0.0f, 0.0f, 0.0f};
};

// Trigger result, ranked None < Ongoing < Triggered.
enum class Rank : int { None = 0, Ongoing = 1, Triggered = 2 };

// Smoothing output per modifier slot (only a `smooth` modifier uses it).
struct ModRuntime {
    float y[3] = {0.0f, 0.0f, 0.0f};
};

// Accessibility timing assist (access.input.*): hold times shrink, tap and combo windows grow. Null = unchanged.
struct Timing {
    float hold = 1.0f, window = 1.0f;
};

// Everything a trigger remembers between updates. `handles` caches the action handles a chord or combo
// names (0 = not registered yet; retried every update).
struct TriggerRuntime {
    bool  was = false;      // actuated last update
    bool  latched = false;  // hold-once fired, tap expired, pulse limit reached
    float t = 0.0f;         // hold/tap time actuated, pulse accumulator, combo step timer
    i32   count = 0;        // pulse count
    i32   step = 0;         // combo progress
    std::vector<int32_t> handles;
};

// What a chord or combo needs to know about other actions during one update.
struct TriggerContext {
    virtual ~TriggerContext() = default;
    virtual int32_t find(const std::string& name) = 0;     // handle, or 0
    virtual Rank    stateOf(int32_t handle) = 0;           // that action's state THIS update
    virtual bool    rose(int32_t handle) = 0;              // Triggered now, not Triggered last update
};

// True when any channel's magnitude exceeds `act`.
bool actuated(const V3& v, float act);

// Applies `mods` in order. `rt` (one slot per modifier) lets `smooth` advance by `dt`; with rt null a
// smooth modifier passes the value through.
V3 applyModifiers(const std::vector<fmt::OcModifierSpec>& mods, V3 v, float dt, std::vector<ModRuntime>* rt);

// Advances one trigger by one update.
Rank stepTrigger(const fmt::OcTriggerSpec& spec, TriggerRuntime& rt, const V3& v, float dt, TriggerContext& ctx,
                 const Timing* timing = nullptr);

// Steps EVERY trigger (their memory must advance) and merges: explicit triggers OR together (max),
// the implicit chord ANDs (min), and an owner with no triggers is a plain `down`. `rts` is parallel
// to `specs` and is resized if it is not.
Rank combineTriggers(const std::vector<fmt::OcTriggerSpec>& specs, std::vector<TriggerRuntime>& rts,
                     const V3& v, float dt, TriggerContext& ctx, const Timing* timing = nullptr);

// AVER_FW_INPUT_EVENT_* mask for a state transition.
int eventsFor(Rank prev, Rank now);

}  // namespace aver::fwinput
