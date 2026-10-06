#pragma once
// Mirrors a listener's hearing memory into its blackboard, so behaviour-tree decorators can watch
// "heard something" like any other key (and abort a branch when it changes).
//
//   Heard.Valid       Bool     a live memory exists
//   Heard.Position    Vec3     where it was heard
//   Heard.Level       Float    how loud, 0..1
//   Heard.Tag         Int      the noise's tag
//   Heard.Source      Entity   who made it (0 if unknown)
//   Heard.Confidence  Float    1 fresh, falling to 0 as it is forgotten
//
// Define the keys on a board with defineKeys (or put them in the agent's schema by hand). Keys the
// board does not define are skipped silently. Install with HearingSystem::setMemorySink; the resolver
// maps a listener entity to its board (null: not bound, nothing written).
#include "aver/synapse/Blackboard.hpp"
#include "aver/synapse/Hearing.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace aver::synapse {

struct HearingBlackboardKeys {
    std::string valid = "Heard.Valid";
    std::string position = "Heard.Position";
    std::string level = "Heard.Level";
    std::string tag = "Heard.Tag";
    std::string source = "Heard.Source";
    std::string confidence = "Heard.Confidence";
};

class HearingBlackboardSink final : public IHearingMemorySink {
public:
    using Resolver = Blackboard* (*)(u32 listener, void* user);

    HearingBlackboardSink(Resolver resolver, void* user, HearingBlackboardKeys keys = {})
        : resolver_(resolver), user_(user), keys_(std::move(keys)) {}

    static void defineKeys(Blackboard& board, const HearingBlackboardKeys& k = {}) {
        const auto def = [&](const std::string& name, BbType t) {
            BbKeyDef d;
            d.name = name;
            d.type = t;
            d.defaultValue = bbDefault(t);
            board.defineKey(d);
        };
        def(k.valid, BbType::Bool);
        def(k.position, BbType::Vec3);
        def(k.level, BbType::Float);
        def(k.tag, BbType::Int);
        def(k.source, BbType::Entity);
        def(k.confidence, BbType::Float);
    }

    void onHeard(u32 listener, const HeardMemory& m) override {
        Blackboard* b = board(listener);
        if (!b) return;
        b->setVec3(keys_.position, m.pos);
        b->setFloat(keys_.level, m.level);
        b->setInt(keys_.tag, static_cast<i64>(m.tag));
        b->setEntity(keys_.source, m.source);
        b->setFloat(keys_.confidence, m.confidence);
        b->setBool(keys_.valid, true);
    }

    // Only the entry currently mirrored clears the keys: forgetting an older, weaker one leaves them.
    void onForgotten(u32 listener, const HeardMemory& m) override {
        Blackboard* b = board(listener);
        if (!b) return;
        const Vec3 shown = b->getVec3(keys_.position);
        const bool same = std::fabs(shown.x - m.pos.x) < 0.5f && std::fabs(shown.y - m.pos.y) < 0.5f &&
                          std::fabs(shown.z - m.pos.z) < 0.5f && b->getInt(keys_.tag) == static_cast<i64>(m.tag);
        if (!same) return;
        b->setBool(keys_.valid, false);
        b->setFloat(keys_.confidence, 0.0f);
        b->setFloat(keys_.level, 0.0f);
    }

private:
    Blackboard* board(u32 listener) const { return resolver_ ? resolver_(listener, user_) : nullptr; }

    Resolver resolver_;
    void* user_;
    HearingBlackboardKeys keys_;
};

} // namespace aver::synapse
