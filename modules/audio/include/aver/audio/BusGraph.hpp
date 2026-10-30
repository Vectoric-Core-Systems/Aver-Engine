#pragma once
// The bus graph on the mixer. Named buses in a list (index 0 is "Master"), each with a volume, mute, solo,
// bypass, an effect chain and a send to another bus; voices that name a graph bus are summed into it, the buses are processed from the
// last index down to Master, and the Master block is the graph's output. Opt-in: nothing here runs until the graph is activated, and
// the four legacy buses (audio::Bus) are untouched.
//
// Threading. Structure edits (add / remove / move a bus, add / remove an effect, a send or sidechain change) are game-thread calls that
// take a spin lock the audio thread only ever try-locks: a block that finds the graph locked renders without its graph voices (a rare,
// brief gap while a script is rebuilding the layout). Volume, mute, solo, bypass, effect enable and effect parameters are atomics and
// never lock.
#include "aver/audio/BusLayout.hpp"
#include "aver/audio/Effects.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aver::audio {

class BusGraph {
public:
    static constexpr u32 kMaxBuses = 255;
    static constexpr u32 kMaxEffects = 32;
    static constexpr u32 kBlock = 4096;                     // frames per processing chunk

    BusGraph();
    ~BusGraph();

    // Sizes the graph for a device rate and creates "Master". Allocates.
    void init(u32 sampleRate);
    void shutdown();
    u32 sampleRate() const { return rate_; }
    // A device rate change: every effect is rebuilt for the new rate with its parameters, enabled flag, sidechain and id kept (its delay
    // lines and filter memory start empty). Allocates; the audio thread must not be mixing (the device layer calls it between devices).
    void setSampleRate(u32 sampleRate);

    // The graph is inert until activated; voices on graph buses are silent while it is not.
    bool active() const { return active_.load(std::memory_order_acquire); }
    void setActive(bool a) { active_.store(a, std::memory_order_release); }

    // ---- structure (game thread) ----
    i32  busCount() const { return static_cast<i32>(buses_.size()); }
    // Adds a bus: at < 0 or past the end appends; 0 is not an insertion point (it becomes 1 when there is more than Master). Returns the index.
    i32  addBus(i32 at = -1);
    bool removeBus(i32 idx);                                 // never Master
    // Moves a bus: the bus goes to `to` (-1 or bus_count appends), numbered before the move. Never Master, never to 0.
    bool moveBus(i32 from, i32 to);
    // Adds or removes buses at the end until there are `count` (at least 1).
    void setBusCount(i32 count);
    i32  findBus(const std::string& name) const;             // -1 when none
    const std::string& busName(i32 idx) const;
    void setBusName(i32 idx, const std::string& name);       // Master keeps its name; a clash gets a numeric suffix
    // Sets a bus's send: by name, unvalidated; resolved when mixing (an unknown name or a bus at or after this one sends to Master).
    void setSend(i32 idx, const std::string& sendName);
    const std::string& sendName(i32 idx) const;
    i32  sendIndex(i32 idx) const;                           // the bus the send resolves to now
    void setVolumeDb(i32 idx, f32 db);
    f32  volumeDb(i32 idx) const;
    void setMute(i32 idx, bool m);
    bool mute(i32 idx) const;
    void setSolo(i32 idx, bool s);
    bool solo(i32 idx) const;
    void setBypassEffects(i32 idx, bool b);
    bool bypassEffects(i32 idx) const;

    // ---- effects ----
    // Adds an effect at `at` (-1 appends). Returns its id (never 0), or 0 when the bus or kind is invalid / the chain is full.
    u32  addEffect(i32 bus, EffectKind kind, i32 at = -1);
    bool removeEffect(i32 bus, i32 effectIdx);
    bool moveEffect(i32 bus, i32 from, i32 to);               // swaps into place: the effect ends at index `to`
    i32  effectCount(i32 bus) const;
    u32  effectId(i32 bus, i32 effectIdx) const;
    Effect* effect(u32 id) const;                            // null for an unknown id
    Effect* effectAt(i32 bus, i32 effectIdx) const;
    bool findEffect(u32 id, i32& bus, i32& effectIdx) const;
    void setEffectEnabled(i32 bus, i32 effectIdx, bool e);
    bool effectEnabled(i32 bus, i32 effectIdx) const;
    // A compressor's sidechain: the bus it listens to (empty clears). Resolved with the sends.
    void setSidechain(i32 bus, i32 effectIdx, const std::string& busName);
    const std::string& sidechain(i32 bus, i32 effectIdx) const;

    // Game thread: lets every effect free what the audio thread has finished with (a convolution reverb's replaced impulse).
    void collect();

    // ---- meters ----
    // The left / right peak in dB: the peak of the bus's last block after effects and volume; -200 when silent.
    f32  peakDb(i32 idx, i32 channel) const;

    // ---- layouts ----
    // Replaces every bus with the layout's (index 0 becomes Master whatever it is called). Returns the number of effects it could not
    // make (an unknown class name).
    u32  applyLayout(const BusLayout& layout);
    BusLayout exportLayout() const;

    // ---- audio thread ----
    // Takes the structure lock for one chunk; false when a game-thread edit holds it. Call end() when done.
    bool begin();
    void end();
    // The stereo (interleaved) accumulation buffer of a bus for this chunk; voices add into it. Index clamps to Master when out of range.
    f32* busBuffer(i32 idx);
    // Zeroes the first `frames` frames of every bus buffer.
    void clear(u32 frames);
    // Runs effects, gains and sends from the last bus to Master over `frames` frames (<= kBlock). Master's block is busBuffer(0).
    void process(u32 frames);

private:
    struct Slot {
        std::unique_ptr<Effect> fx;
        std::atomic<bool> enabled{true};
        std::string sidechain;
        u32 id = 0;
    };
    struct Bus {
        std::string name, send = "Master";
        std::atomic<f32> volumeDb{0.0f};
        std::atomic<bool> mute{false}, solo{false}, bypass{false};
        std::vector<std::unique_ptr<Slot>> effects;
        i32 sendIdx = 0;
        std::vector<f32> buf;
        std::atomic<f32> peak[2];
        f32 lastGain = 1.0f;
        bool soloed = false;
        Bus() { peak[0].store(0.0f); peak[1].store(0.0f); buf.assign(static_cast<usize>(kBlock) * 2, 0.0f); }
    };

    struct Lock {
        std::atomic_flag f = ATOMIC_FLAG_INIT;
        bool tryLock() { return !f.test_and_set(std::memory_order_acquire); }
        void lock() { while (f.test_and_set(std::memory_order_acquire)) {} }
        void unlock() { f.clear(std::memory_order_release); }
    };
    struct Guard { Lock& l; explicit Guard(Lock& x) : l(x) { l.lock(); } ~Guard() { l.unlock(); } };

    bool valid(i32 idx) const { return idx >= 0 && idx < busCount(); }
    std::string uniqueName(const std::string& want, const Bus* except) const;
    void resolveLinks();                                     // sends and sidechains to indices; under the lock

    u32 rate_ = 0;
    std::atomic<bool> active_{false};
    std::vector<std::unique_ptr<Bus>> buses_;
    u32 nextEffectId_ = 1;
    mutable Lock lock_;
};

} // namespace aver::audio
