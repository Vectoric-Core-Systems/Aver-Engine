#pragma once
// SequenceDirector: every level sequence of a session, one SequencePlayer each (docs/SEQUENCER.md).
// A single-sequence level behaves as it always did: the sequence starts with the session when it
// autoplays. Several sequences play side by side; script control (Play / Pause / Stop / SetTime /
// Bind) arrives through the framework's sequence ABI, whose provider is game::sequenceProvider
// (SequenceHost.hpp).
//
// Frame order, one director per host (the editor's frame and the game's are the same):
//   advance(dt); evaluate(Pre); animation graphs and AnimSystem::tick; evaluate(Post).
#include "aver/game/LevelSequence.hpp"

#if AVER_MODULE_SCENE

namespace aver::game {

class SequenceDirector {
public:
    SequenceDirector();
    ~SequenceDirector();
    SequenceDirector(const SequenceDirector&) = delete;
    SequenceDirector& operator=(const SequenceDirector&) = delete;

    // Copies the sequences. A sequence whose name survives keeps its clock, play state and start order.
    void setSequences(std::vector<fmt::OcSequence> seqs);
    void setBindings(SeqBindings b);
    void setHooks(SequenceHooks h);
    void setMode(SeqMode m);
    void setAudition(bool on);
    const std::vector<fmt::OcSequence>& sequences() const { return seqs_; }

    // The sequence whose name hashes (fnv1a64) to nameHash, or -1.
    i32 find(u64 nameHash) const;
    i32 find(const std::string& name) const;

    // Control: what the ABI provider and the host call. False for an unknown sequence.
    // startTime < 0 continues (a stopped or finished one starts at 0); rate 0 keeps the rate.
    bool play(i32 seq, f64 startTime, f64 rate);
    bool pause(i32 seq);
    bool stop(i32 seq);   // -1 = all: pause, time 0, put back what only it drove, stop its voices
    bool setTime(i32 seq, f64 t);
    bool setRate(i32 seq, f64 r);
    bool state(i32 seq, f64& time, f64& length, f64& rate, bool& playing) const;
    // Replaces the actor a `bind` name resolves to for that sequence (and its sub-sequences); an invalid
    // entity clears the override. False when the sequence has no track bound by that name.
    bool bindOverride(i32 seq, u64 nameHash, scene::Entity e);
    // Starts every sequence that autoplays, from 0.
    void startAutoplay();

    void advance(f64 dt);
    void evaluate(scene::World& world, SeqPhase phase);

    // The live camera: of the sequences that were started and have a camera, the most recently started.
    bool camera(SeqCameraPose& out) const;
    bool emissiveScale(scene::Entity e, f32 out[3]) const;
    // The actors the sequences move (for PlayMobility::seedMovable).
    std::vector<scene::Entity> boundEntities() const;
    // True when no sequence has any track.
    bool empty() const;
    bool playingAny() const;
    u32  count() const { return static_cast<u32>(entries_.size()); }
    SequencePlayer* player(i32 seq);
    const SequencePlayer* player(i32 seq) const;

    // Releases voices and driven animation, puts every sequenced write back.
    void restore(scene::World& world);
    // Forgets the sequences (call restore first).
    void clear();
    // Floating-origin shift (LARGE_WORLD 2.2.5): the shared base store follows the scene. Call after World::shiftRoots.
    void rebase(const scene::World& world, const Vec3& deltaCm);

private:
    struct Entry {
        SequencePlayer player;
        u32  serial = 0;          // order of the last start; 0 = never started / stopped
        bool engaged = false;     // evaluated every frame (started, scrubbed, or finished and holding)
        bool started = false;     // _Started raised and not yet stopped
        std::unordered_map<std::string, scene::Entity> overrides;
    };
    Entry* at(i32 seq) { return seq >= 0 && static_cast<usize>(seq) < entries_.size() ? entries_[static_cast<usize>(seq)].get() : nullptr; }
    const Entry* at(i32 seq) const { return seq >= 0 && static_cast<usize>(seq) < entries_.size() ? entries_[static_cast<usize>(seq)].get() : nullptr; }
    void configure(Entry& e);
    void raise(const Entry& e, const char* suffix) const;
    void stopOne(Entry& e, scene::World& w);

    std::vector<fmt::OcSequence> seqs_;
    std::vector<std::unique_ptr<Entry>> entries_;
    SeqBindings bindings_;
    SequenceHooks hooks_;
    SeqLibrary library_;
    std::unique_ptr<SeqBaseStore> store_;
    SeqMode mode_ = SeqMode::Live;
    bool audition_ = false;
    u32 serial_ = 0;
};

} // namespace aver::game

#endif
