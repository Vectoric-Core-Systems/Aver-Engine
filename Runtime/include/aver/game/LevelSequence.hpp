#pragma once
// LEVEL SEQUENCE PLAYBACK, shared by the editor's Animate mode, editor Play and the packaged game.
// The data is fmt::OcSequence, stored in the .ocworld itself (docs/formats/FORMAT_SPECS.md section 11);
// the tracks, modes and phases are docs/SEQUENCER.md.
//
// Sampling is pure (no scene) so it is testable headless. The player binds tracks to entities (by
// placement index or by name) and, each evaluate(), writes what the tracks drive and caches the camera
// pose and per-entity emissive multipliers for the host to apply to the view and the draw walk. Every
// write the player makes is undoable through a base store (SeqBaseStore).
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcSeq.hpp"
#include "aver/formats/OcWorld.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"
#include "aver/scene/Fields.hpp"
namespace aver::scene { class World; }
#endif

namespace aver::game {

// A camera key's pose in the editor camera's convention: yaw/pitch in radians, as
// game::cameraForward(yaw, pitch) takes them.
struct SeqCameraPose {
    Vec3 position{0, 0, 0};   // cm, world
    f32  yaw = 0, pitch = 0;
};

// Time t mapped into [0, length]: wrapped when the sequence loops, clamped when it does not.
f64 seqWrapTime(const fmt::OcSequence& seq, f64 t);

// The fixed step of fixed-step playback and capture runs: 1 / fps seconds (fps clamped to 1..1000).
inline f64 seqStepSeconds(const fmt::OcSequence& seq) {
    return 1.0 / static_cast<f64>(seq.fps < 1 ? 1 : (seq.fps > 1000 ? 1000 : seq.fps));
}

// The track's value at t. False when the track has no keys or is the wrong kind.
// Smooth = Catmull-Rom through the neighbouring keys (positions, scales, material values; rotations slerp
// with the same eased parameter), Linear = lerp / slerp, Step = hold the key. A camera track differs in
// Smooth: position follows a centripetal Catmull-Rom spline and yaw / pitch go through squad on
// quaternions (no flip across +-180 degrees, shortest arc); before the first key and after the last the
// pose is held. Cubic = cubic Hermite per channel on the authored numbers, Euler degrees included (no
// quaternion path, no short arc), with the keys' own tangents or the automatic ones.
bool sampleSeqTransform(const fmt::OcSeqTrack& track, f64 t, Transform& out);
bool sampleSeqCamera(const fmt::OcSeqTrack& track, f64 t, SeqCameraPose& out);
bool sampleSeqMaterial(const fmt::OcSeqTrack& track, f64 t, f32 outScale[3]);   // rgb * intensity

// Key values from a live transform / camera, in the format's units (the inverse of the samplers).
void seqKeyFromTransform(const Transform& xf, fmt::OcSeqKey& key);
void seqKeyFromCamera(const SeqCameraPose& pose, fmt::OcSeqKey& key);

// ---- curves, tangents, sections, crossings (pure; the editor's curve view draws with these) ----------
// Channels a track edits as curves: transform 9, camera 5, material 4, property `propertyArity`, others 0.
u32  seqChannelCount(const fmt::OcSeqTrack& track, u32 propertyArity = 1);
// One channel as the player evaluates it. Rotation channels (transform 3..5, camera 3..4) are the authored
// Euler numbers: under Smooth / Linear they follow the same eased parameter the rotation slerp uses, under
// Cubic they are exactly what the Hermite gives. An out-of-range channel is 0.
f64  sampleSeqChannel(const fmt::OcSeqTrack& track, u32 channel, f64 t);
// The automatic Hermite slopes (value per second) at `key`: the non-uniform finite difference across its
// neighbours, one-sided at the ends; arrive == leave.
void seqAutoTangent(const std::vector<fmt::OcSeqKey>& keys, usize key, u32 channel, f64& arrive, f64& leave);
// The slopes a key really uses: its own tan entries when it has them, else the automatic ones.
void seqEffectiveTangent(const std::vector<fmt::OcSeqKey>& keys, usize key, u32 channel, f64& arrive, f64& leave);
// A property track at t into out[0..3] (channels past the field's arity are whatever the keys hold).
// `quat`: channels are a quaternion (slerped, normalised); `stepOnly`: integer fields hold the key.
bool sampleSeqProperty(const fmt::OcSeqTrack& track, f64 t, bool quat, bool stepOnly, f64 out[4]);
// The section playing at t: the latest-starting key at or before t whose [t, t + dur) holds t, or, with
// holdAfter, the latest-starting key whatever its end. local = t - start. Null when none.
const fmt::OcSeqKey* seqActiveSection(const fmt::OcSeqTrack& track, f64 t, bool holdAfter, f64& local);
// 0..1 smoothstep ramps in (fadeIn s) and out (fadeOut s, ending at dur), 1 between, 0 before the start.
f32  seqSectionWeight(const fmt::OcSeqKey& key, f64 local, f64 fadeIn, f64 fadeOut);
// Key indices whose time the clock passed going from t0 to t1, in order: (t0, t1], or [t0, t1] with
// inclusiveStart. t1 < t0 with `loop` is a wrap through `length`: (t0, length] then [0, t1]. Returns the count.
usize seqCrossedKeys(const fmt::OcSeqTrack& track, f64 t0, f64 t1, bool inclusiveStart, f64 length, bool loop,
                     std::vector<u32>& out);
// The camera a cut track names at t: `camera` (empty before the first cut: use the first camera track),
// `prevCamera` while the blend into `camera` runs (empty otherwise), blendAlpha 0..1 (smoothstep). False
// when the track has no keys.
bool sampleSeqCut(const fmt::OcSeqTrack& cuts, f64 t, std::string& camera, std::string& prevCamera, f32& blendAlpha);
// A camera track's display / lookup name: its own, or "Camera".
inline const std::string& seqCameraName(const fmt::OcSeqTrack& tr) {
    static const std::string kDefault = "Camera";
    return tr.name.empty() ? kDefault : tr.name;
}

#if AVER_MODULE_SCENE

enum class SeqMode : u8 { Preview, Live, Offline };
enum class SeqPhase : u8 { Pre, Post };

// What the host knows about the level's actors. A track resolves: overrides[bind], byName[bind], find(bind),
// then byPlacement[target] as the fallback.
struct SeqBindings {
    std::vector<scene::Entity> byPlacement;                    // placement index -> entity
    std::unordered_map<std::string, scene::Entity> byName;     // placement `name` (first wins), light names, class actors
    std::function<scene::Entity(const std::string&)> find;     // fallback; hosts pass World::find
    std::unordered_map<std::string, scene::Entity> overrides;  // runtime Bind: wins over everything
};

struct SeqVoice {
    f32 volume = 1, pitch = 1;
    bool loop = false, spatial = false;
    Vec3 pos{0, 0, 0};
    f32 innerCm = 300, outerCm = 3000, fadeIn = 0;
};
struct SeqEventCall {
    std::string name, arg;
    scene::Entity sender = scene::kInvalidEntity, target = scene::kInvalidEntity;
    u8 route = 0, argc = 0;                                    // route: 0 bus, 1 graph, 2 both
    f32 args[4] = {};
};
// Installed by the host; a null function makes that track kind silent. The player ramps a voice's volume
// itself (updateVoice every tick); SeqVoice::fadeIn is informational.
struct SequenceHooks {
    std::function<i32(const std::string& contentPath)> loadSound;            // handle or 0
    std::function<i32(i32 sound, const SeqVoice&)> playVoice;                // voice or 0
    std::function<void(i32 voice, const Vec3* pos, f32 volume)> updateVoice;
    std::function<void(i32 voice, f32 fadeSeconds)> stopVoice;
    std::function<void(const SeqEventCall&)> fireEvent;
    std::function<std::string(const std::string& contentPath)> readFile;     // for .ocseq; null = read from disk
};

// name -> sequence for sub-sequences: the level's sequences plus loaded .ocseq files (cached by path).
class SeqLibrary {
public:
    void setLevel(const std::vector<fmt::OcSequence>* seqs) { level_ = seqs; }   // borrowed
    // A level sequence by name, or a `path.ocseq[#Name]`. Null on failure (a failed path is cached).
    const fmt::OcSequence* find(const std::string& nameOrPath, const SequenceHooks& hooks);
    void clear() { files_.clear(); }
private:
    const std::vector<fmt::OcSequence>* level_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<fmt::OcSeqFile>> files_;     // null = failed once
};

// What the sequencer wrote over, so everything is undoable: the first write of a (entity, component,
// offset) records the authored bytes; restore puts them back. Entities that no longer exist are dropped.
class SeqBaseStore {
public:
    // Records `size` (<= 16) bytes at `offset` of the entity's component, on the first write only.
    void capture(scene::World& w, scene::Entity e, u32 component, u16 offset, u16 size);
    void captureField(scene::World& w, scene::Entity e, const scene::FieldDesc& f);
    void captureTransform(scene::World& w, scene::Entity e);          // the entity's CLocal transform
    void captureComponentAdded(scene::Entity e, u32 component);       // the sequencer attached it: restore removes it
    void restore(scene::World& w);                                    // everything, then forget
    void release(scene::World& w, scene::Entity e);                   // restore one entity's entries and forget them
    // Restores and forgets every entry whose entity is not in `driven`.
    void releaseUndriven(scene::World& w, const std::unordered_set<scene::Entity>& driven);
    bool empty() const { return props_.empty() && xforms_.empty() && added_.empty(); }
    usize size() const { return props_.size() + xforms_.size() + added_.size(); }
    bool hasTransform(scene::Entity e) const;
    // Floating-origin shift (LARGE_WORLD 2.2.5): subtracts deltaCm from the saved position of every ROOT
    // entity (a child's is parent-relative) - the captured transform and a captured CLocal position field -
    // so a later restore() puts it back in the shifted frame. Call after World::shiftRoots.
    void rebase(const scene::World& w, const Vec3& deltaCm);
private:
    struct Entry { scene::Entity e = scene::kInvalidEntity; u32 comp = 0; u16 off = 0, size = 0; u8 bytes[16] = {}; };
    struct XfEntry { scene::Entity e = scene::kInvalidEntity; Transform xf; };
    struct AddedEntry { scene::Entity e = scene::kInvalidEntity; u32 comp = 0; };
    void restoreEntry(scene::World& w, const Entry& en) const;
    std::vector<Entry> props_;
    std::vector<XfEntry> xforms_;
    std::vector<AddedEntry> added_;
};

class SequencePlayer {
public:
    SequencePlayer();
    ~SequencePlayer();
    SequencePlayer(SequencePlayer&&);
    SequencePlayer& operator=(SequencePlayer&&);
    SequencePlayer(const SequencePlayer&) = delete;
    SequencePlayer& operator=(const SequencePlayer&) = delete;

    // The sequence is copied; call again after an edit. Releases what the previous tracks were driving
    // (animation players, voices) but leaves the base store alone, so an edit never flickers the scene.
    void setSequence(const fmt::OcSequence& seq);
    // A borrowed sequence (sub-sequences, the library's). It must outlive the player or the next setSequence.
    void setSequenceRef(const fmt::OcSequence* seq);
    const fmt::OcSequence& sequence() const { return ext_ ? *ext_ : own_; }
    bool empty() const { return sequence().tracks.empty(); }

    // placementEntities[i] is the entity placement i instantiated as (kInvalidEntity when none).
    void bind(const std::vector<scene::Entity>& placementEntities) { bindings_.byPlacement = placementEntities; }
    void setBindings(SeqBindings b) { bindings_ = std::move(b); }
    const SeqBindings& bindings() const { return bindingsRef_ ? *bindingsRef_ : bindings_; }
    void setLibrary(SeqLibrary* lib) { library_ = lib; }
    void setHooks(const SequenceHooks* hooks) { hooks_ = hooks; }
    void setMode(SeqMode m) { mode_ = m; }
    SeqMode mode() const { return mode_; }
    // The editor's Audition toggle: Preview lets audio play while the clock moves forward.
    void setAudition(bool on) { audition_ = on; }
    void setRate(f64 r) { rate_ = r < 0 ? 0 : r; }
    f64  rate() const { return rate_; }
    // The scene's floating origin O in cm (LARGE_WORLD 2.2.5), shared by every player because the scene has one
    // frame. Transform and camera keys are authored level-absolute f64; a ROOT entity's key is written as
    // key - O in f64, then narrowed. Zero (the default) writes exactly what the sampler returned.
    static void setSceneOrigin(i64 xCm, i64 yCm);
    static void sceneOrigin(i64& xCm, i64& yCm);

    // Where writes are recorded; null = the player's own store (restore() then restores it).
    void setBaseStore(SeqBaseStore* s) { store_ = s; }
    SeqBaseStore& baseStore() { return store_ ? *store_ : ownStore_; }
    const SeqBaseStore& baseStore() const { return store_ ? *store_ : ownStore_; }

    scene::Entity entityOf(i32 placement) const;
    // The entity a track drives or rides, resolved through the bindings (kInvalidEntity when none).
    scene::Entity resolve(const fmt::OcSeqTrack& track) const;
    // Every entity any track binds, once each, sub-sequences included.
    std::vector<scene::Entity> boundEntities() const;
    // The ones that MOVE (transform and material tracks, CLocal property tracks; sub-sequences included).
    std::vector<scene::Entity> movedEntities() const;
    std::vector<scene::Entity> transformEntities() const;

    // Clock. advance() moves the time only while playing, wrapping or stopping at the end.
    void play();
    void pause() { playing_ = false; }
    bool playing() const { return playing_; }
    // A jump: nothing triggered fires for the span skipped, and voices stop.
    void setTime(f64 t);
    f64  time() const { return time_; }
    void advance(f64 dt);
    // True once after a non-looping sequence reached its end.
    bool takeFinished() { const bool f = finished_; finished_ = false; return f; }

    // Samples the tracks at time(). Pre (before the animation tick): property, visibility, animation, events,
    // audio, sub-sequences. Post (after it): transform, material, cameras and cuts. The one-argument form is Post.
    void evaluate(scene::World& world, SeqPhase phase);
    void evaluate(scene::World& world) { evaluate(world, SeqPhase::Post); }
    // The animation tracks alone (and the sub-sequences', recursively): re-drives the montage players an
    // editor holds still between edits, since a driven player lapses after a few ticks without a call.
    void evaluateAnimation(scene::World& world);

    // The live camera at the last Post evaluate (cuts resolved); false when there is no camera.
    bool camera(SeqCameraPose& out) const;
    const std::string& cameraName() const { return cameraName_; }
    // The emissive multiplier for e at the last evaluate(); false when no material track drives it.
    bool emissiveScale(scene::Entity e, f32 out[3]) const;

    // Stops voices, releases driven animation, and puts back everything the base store holds when the
    // player owns its store (a director-owned store is restored by the director).
    void restore(scene::World& world);
    // Voices and driven animation only (a stop that keeps the pose).
    void releaseTriggered();

private:
    struct Span { f64 t0 = 0, t1 = 0; bool inclusive = false; };
    struct AudioState { i32 voice = 0; i32 key = -1; scene::Entity e = scene::kInvalidEntity; };
    struct AnimState { scene::Entity e = scene::kInvalidEntity; u64 asset = 0; bool warned = false; };
    struct PropState { bool warned = false; };

    SequencePlayer* subFor(u32 track, u32 key, const fmt::OcSeqKey& k, const fmt::OcSeqTrack& tr);
    void followTime(f64 newTime, bool contiguous, bool entered, bool forceLoop);
    void evalPre(scene::World& w);
    void evalPost(scene::World& w);
    void applyProperty(scene::World& w, u32 index, const fmt::OcSeqTrack& tr, f64 t);
    void applyVisibility(scene::World& w, const fmt::OcSeqTrack& tr, f64 t);
    void applyAnimation(scene::World& w, u32 index, const fmt::OcSeqTrack& tr, f64 t);
    void applyEvents(scene::World& w, u32 index, const fmt::OcSeqTrack& tr);
    void applyAudio(scene::World& w, u32 index, const fmt::OcSeqTrack& tr, f64 t);
    void applySub(scene::World& w, SeqPhase phase, u32 index, const fmt::OcSeqTrack& tr, f64 t);
    void releaseAnim(u32 index);
    void stopVoice(u32 index, f32 fade);
    void stopAllVoices(f32 fade);
    void resetTrackState();
    u64  ownerOf(u32 track) const { return uid_ * 0x9E3779B97F4A7C15ull + track + 1; }
    bool triggersActive() const;

    fmt::OcSequence own_;
    const fmt::OcSequence* ext_ = nullptr;
    SeqBindings bindings_;
    const SeqBindings* bindingsRef_ = nullptr;     // a sub-sequence reads its parent's
    SeqLibrary* library_ = nullptr;
    const SequenceHooks* hooks_ = nullptr;
    SeqMode mode_ = SeqMode::Live;
    SeqBaseStore ownStore_;
    SeqBaseStore* store_ = nullptr;
    u64 uid_ = 0;
    u32 depth_ = 0;
    std::vector<std::string> stack_;               // sequence names above this one (cycle check)
    f64 prevSub_ = 0;                              // sub-sequence: its time at the last follow
    bool subActive_ = false;                       // sub-sequence: inside its section last frame

    f64  time_ = 0;
    f64  rate_ = 1;
    bool playing_ = false;
    bool finished_ = false;
    bool inclusive_ = false;                       // the next advance includes its start instant
    bool jumped_ = false;                          // setTime since the last Pre evaluate
    bool audition_ = false;
    bool forceLoop_ = false;                       // sub-sequence section loops
    bool animOnly_ = false;                        // evaluateAnimation is running
    std::vector<Span> spans_;

    bool hasCamera_ = false;
    bool hasSubCam_ = false;                       // an active sub-sequence section claimed the camera this frame
    SeqCameraPose subCam_{};
    SeqCameraPose camera_{};
    std::string cameraName_;
    std::unordered_map<u32, std::array<f32, 3>> emissive_;

    std::vector<AudioState> audio_;                // per track index
    std::vector<AnimState> anim_;
    std::vector<PropState> prop_;
    std::map<u64, std::unique_ptr<SequencePlayer>> subs_;   // (track << 32 | key) -> its player
    std::unordered_set<u64> warnedSubs_;
};
#endif

} // namespace aver::game
