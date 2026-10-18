// Trigger and modifier evaluation, see ActionEngine.hpp. Semantics: docs/plans/wave2/ENHANCED_INPUT.md 2.
#include "ActionEngine.hpp"

#include "aver/framework/framework_abi.h"

#include <algorithm>
#include <cmath>

namespace aver::fwinput {

namespace {

// Output channel i reads input channel kSwizzle[mode][i] (OcModifierSpec::mode order).
const int kSwizzle[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}, {0, 2, 1}, {1, 2, 0}, {2, 0, 1}};

// The pinned legacy actuation, used by an owner with no triggers.
constexpr float kDefaultActuation = 0.15f;

float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

Rank minRank(Rank a, Rank b) { return static_cast<int>(a) < static_cast<int>(b) ? a : b; }
Rank maxRank(Rank a, Rank b) { return static_cast<int>(a) > static_cast<int>(b) ? a : b; }

// Registers handles for a chord/combo's named actions; a name that is not an action yet stays 0 and is
// retried next update (the action may be registered later, e.g. by a graph's OnStart).
void resolveRefs(const fmt::OcTriggerSpec& spec, TriggerRuntime& rt, TriggerContext& ctx) {
    if (rt.handles.size() != spec.refs.size()) rt.handles.assign(spec.refs.size(), 0);
    for (usize i = 0; i < spec.refs.size(); ++i)
        if (rt.handles[i] == 0) rt.handles[i] = ctx.find(spec.refs[i]);
}

}  // namespace

bool actuated(const V3& v, float act) {
    return std::fabs(v.c[0]) > act || std::fabs(v.c[1]) > act || std::fabs(v.c[2]) > act;
}

V3 applyModifiers(const std::vector<fmt::OcModifierSpec>& mods, V3 v, float dt, std::vector<ModRuntime>* rt) {
    for (usize i = 0; i < mods.size(); ++i) {
        const fmt::OcModifierSpec& m = mods[i];
        switch (m.kind) {
            case fmt::OcModifierKind::DeadZone: {
                const float lo = m.v[0], span = m.v[1] - m.v[0];
                if (m.mode == 1) {   // axial: each channel on its own, sign kept
                    for (float& c : v.c) {
                        const float a = std::fabs(c);
                        c = a < lo || span <= 0.0f ? 0.0f : std::copysign(clamp01((a - lo) / span), c);
                    }
                } else {             // radial: the whole vector's length is remapped
                    const float len = std::sqrt(v.c[0] * v.c[0] + v.c[1] * v.c[1] + v.c[2] * v.c[2]);
                    if (len <= 0.0f || len < lo || span <= 0.0f) {
                        v = V3{};
                    } else {
                        const float k = clamp01((len - lo) / span) / len;
                        for (float& c : v.c) c *= k;
                    }
                }
                break;
            }
            case fmt::OcModifierKind::Scale:
                for (int k = 0; k < 3; ++k) v.c[k] *= m.v[k];
                break;
            case fmt::OcModifierKind::Negate:
                for (int k = 0; k < 3; ++k) if (m.mask & (1 << k)) v.c[k] = -v.c[k];
                break;
            case fmt::OcModifierKind::Swizzle: {
                const int* o = kSwizzle[m.mode >= 0 && m.mode < 6 ? m.mode : 0];
                const V3 in = v;
                for (int k = 0; k < 3; ++k) v.c[k] = in.c[o[k]];
                break;
            }
            case fmt::OcModifierKind::Smooth: {
                if (!rt || i >= rt->size()) break;   // stateless query: pass through
                ModRuntime& s = (*rt)[i];
                const float tau = m.v[0];
                const float a = tau <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / tau);
                for (int k = 0; k < 3; ++k) {
                    s.y[k] += (v.c[k] - s.y[k]) * a;
                    v.c[k] = s.y[k];
                }
                break;
            }
            case fmt::OcModifierKind::Curve:
                for (int k = 0; k < 3; ++k)
                    v.c[k] = v.c[k] == 0.0f ? 0.0f : std::copysign(std::pow(std::fabs(v.c[k]), m.v[k]), v.c[k]);
                break;
        }
    }
    return v;
}

Rank stepTrigger(const fmt::OcTriggerSpec& spec, TriggerRuntime& rt, const V3& v, float dt, TriggerContext& ctx,
                 const Timing* timing) {
    const bool A = actuated(v, spec.actuation);
    const float holdTime = timing ? spec.time * timing->hold : spec.time;
    const float windowTime = timing ? spec.time * timing->window : spec.time;
    Rank r = Rank::None;
    switch (spec.kind) {
        case fmt::OcTriggerKind::Down:
            r = A ? Rank::Triggered : Rank::None;
            break;
        case fmt::OcTriggerKind::Pressed:
            r = (A && !rt.was) ? Rank::Triggered : Rank::None;
            break;
        case fmt::OcTriggerKind::Released:
            r = A ? Rank::Ongoing : (rt.was ? Rank::Triggered : Rank::None);
            break;
        case fmt::OcTriggerKind::Hold:
            if (!A) {
                rt.t = 0.0f;
                rt.latched = false;
            } else {
                rt.t += dt;
                if (rt.latched) r = Rank::None;                       // `once` already fired
                else if (rt.t >= holdTime) { r = Rank::Triggered; rt.latched = spec.once; }
                else r = Rank::Ongoing;
            }
            break;
        case fmt::OcTriggerKind::HoldRelease:
            if (A) {
                rt.t += dt;
                r = Rank::Ongoing;
            } else {
                if (rt.was && rt.t >= holdTime) r = Rank::Triggered;
                rt.t = 0.0f;
            }
            break;
        case fmt::OcTriggerKind::Tap:
            if (A) {
                rt.t += dt;
                if (!rt.latched && rt.t > windowTime) rt.latched = true;   // held too long: expired
                r = rt.latched ? Rank::None : Rank::Ongoing;
            } else {
                if (rt.was && !rt.latched) r = Rank::Triggered;
                rt.t = 0.0f;
                rt.latched = false;
            }
            break;
        case fmt::OcTriggerKind::Pulse:
            if (!A) {
                rt.t = 0.0f; rt.count = 0; rt.latched = false;
            } else {
                // The press frame is time 0 (no dt added), so an onstart pulse fires then and the next
                // one lands exactly one interval later.
                if (!rt.was) { rt.t = 0.0f; rt.count = 0; rt.latched = false; }
                else rt.t += dt;
                if (rt.latched) {
                    r = Rank::None;
                } else if (rt.t >= static_cast<float>(spec.onStart ? rt.count : rt.count + 1) * spec.time) {
                    r = Rank::Triggered;
                    ++rt.count;
                    if (spec.limit > 0 && rt.count >= spec.limit) rt.latched = true;
                } else {
                    r = Rank::Ongoing;
                }
            }
            break;
        case fmt::OcTriggerKind::Chord: {
            resolveRefs(spec, rt, ctx);
            const int32_t h = rt.handles.empty() ? 0 : rt.handles[0];
            const Rank other = h ? ctx.stateOf(h) : Rank::None;
            r = (A && other == Rank::Triggered) ? Rank::Triggered : Rank::None;
            break;
        }
        case fmt::OcTriggerKind::Combo: {
            resolveRefs(spec, rt, ctx);
            const i32 n = static_cast<i32>(spec.refs.size());
            if (rt.step > 0) {
                rt.t += dt;
                if (rt.t > windowTime) { rt.step = 0; rt.t = 0.0f; }
            }
            bool riseNext = false, riseAny = false, rise0 = false;
            for (i32 j = 0; j < n; ++j) {
                const int32_t h = rt.handles[static_cast<usize>(j)];
                if (!h || !ctx.rose(h)) continue;
                riseAny = true;
                if (j == 0) rise0 = true;
            }
            if (rt.step < n) {
                const int32_t nextH = rt.handles[static_cast<usize>(rt.step)];
                riseNext = nextH != 0 && ctx.rose(nextH);
            }
            // Steps can repeat an action (Fire,Fire,Jump), so progress is judged by the expected step's
            // handle; any other step action rising restarts the sequence (at 1 if it was step 0).
            if (riseNext) { ++rt.step; rt.t = 0.0f; }
            else if (riseAny) { rt.step = rise0 ? 1 : 0; rt.t = 0.0f; }
            if (rt.step >= n) { r = Rank::Triggered; rt.step = 0; rt.t = 0.0f; }
            else r = rt.step > 0 ? Rank::Ongoing : Rank::None;
            break;
        }
    }
    rt.was = A;
    return r;
}

Rank combineTriggers(const std::vector<fmt::OcTriggerSpec>& specs, std::vector<TriggerRuntime>& rts,
                     const V3& v, float dt, TriggerContext& ctx, const Timing* timing) {
    if (specs.empty()) return actuated(v, kDefaultActuation) ? Rank::Triggered : Rank::None;
    if (rts.size() != specs.size()) rts.resize(specs.size());
    bool anyExplicit = false;
    Rank explicitMax = Rank::None, implicitMin = Rank::Triggered;
    for (usize i = 0; i < specs.size(); ++i) {
        const Rank r = stepTrigger(specs[i], rts[i], v, dt, ctx, timing);   // every trigger advances, no short-circuit
        if (specs[i].kind == fmt::OcTriggerKind::Chord) {
            implicitMin = minRank(implicitMin, r);
        } else {
            anyExplicit = true;
            explicitMax = maxRank(explicitMax, r);
        }
    }
    return minRank(anyExplicit ? explicitMax : Rank::Triggered, implicitMin);
}

int eventsFor(Rank prev, Rank now) {
    constexpr int S = AVER_FW_INPUT_EVENT_STARTED, O = AVER_FW_INPUT_EVENT_ONGOING,
                  T = AVER_FW_INPUT_EVENT_TRIGGERED, C = AVER_FW_INPUT_EVENT_COMPLETED,
                  X = AVER_FW_INPUT_EVENT_CANCELED;
    // [prev][now], states None/Ongoing/Triggered.
    static const int kTable[3][3] = {
        {0,     S | O, S | T},
        {X,     O,     T},
        {C,     O,     T},
    };
    return kTable[static_cast<int>(prev)][static_cast<int>(now)];
}

}  // namespace aver::fwinput
