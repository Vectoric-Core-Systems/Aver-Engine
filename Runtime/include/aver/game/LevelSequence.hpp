#pragma once
// LEVEL SEQUENCE PLAYBACK, shared by the editor's Animate mode, editor Play and the packaged game.
// The data is fmt::OcSequence, stored in the .ocworld itself (docs/formats/FORMAT_SPECS.md section 11).
//
// Sampling is pure (no scene) so it is testable headless. The player binds tracks to entities by
// placement index and, each evaluate(), writes the transform targets' CLocal and caches the camera
// pose and per-entity emissive multipliers for the host to apply to the view and the draw walk.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"

#include <array>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"
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

// The track's value at t. False when the track has no keys or is the wrong kind.
// Smooth = Catmull-Rom through the neighbouring keys (positions, scales, angles, material values;
// rotations slerp with the same eased parameter), Linear = lerp / slerp, Step = hold the key.
bool sampleSeqTransform(const fmt::OcSeqTrack& track, f64 t, Transform& out);
bool sampleSeqCamera(const fmt::OcSeqTrack& track, f64 t, SeqCameraPose& out);
bool sampleSeqMaterial(const fmt::OcSeqTrack& track, f64 t, f32 outScale[3]);   // rgb * intensity

// Key values from a live transform / camera, in the format's units (the inverse of the samplers).
void seqKeyFromTransform(const Transform& xf, fmt::OcSeqKey& key);
void seqKeyFromCamera(const SeqCameraPose& pose, fmt::OcSeqKey& key);

#if AVER_MODULE_SCENE
class SequencePlayer {
public:
    // The sequence is copied; call again after an edit.
    void setSequence(const fmt::OcSequence& seq) { seq_ = seq; }
    const fmt::OcSequence& sequence() const { return seq_; }
    bool empty() const { return seq_.tracks.empty(); }

    // placementEntities[i] is the entity placement i instantiated as (kInvalidEntity when none).
    void bind(const std::vector<scene::Entity>& placementEntities) { placementEntities_ = placementEntities; }
    scene::Entity entityOf(i32 placement) const;
    // Every entity a transform or material track drives, once each.
    std::vector<scene::Entity> boundEntities() const;
    std::vector<scene::Entity> transformEntities() const;

    // Clock. advance() moves the time only while playing, wrapping or stopping at the end.
    void play() { playing_ = true; }
    void pause() { playing_ = false; }
    bool playing() const { return playing_; }
    void setTime(f64 t) { time_ = seqWrapTime(seq_, t); }
    f64  time() const { return time_; }
    void advance(f64 dt);

    // Samples every track at time(): writes CLocal of transform targets, caches camera and material.
    void evaluate(scene::World& world);

    // The camera track's pose at the last evaluate(); false when there is no camera track.
    bool camera(SeqCameraPose& out) const;
    // The emissive multiplier for e at the last evaluate(); false when no material track drives it.
    bool emissiveScale(scene::Entity e, f32 out[3]) const;

private:
    fmt::OcSequence seq_;
    std::vector<scene::Entity> placementEntities_;
    f64  time_ = 0;
    bool playing_ = false;
    bool hasCamera_ = false;
    SeqCameraPose camera_{};
    std::unordered_map<u32, std::array<f32, 3>> emissive_;
};
#endif

} // namespace aver::game
