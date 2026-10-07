#pragma once
// The level sequence editor behind the editor's Animate mode: one fmt::OcSequence stored in the
// .ocworld, edited on a timeline, previewed in the viewport and played in Play.
//
// SandboxApp owns one and calls it at a few hook points; the model, the preview state and all the UI
// live here. The model's transform and material tracks hold a scene::Entity while editing; load()
// and save() convert them to and from placement indices.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelSequence.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"

namespace aver::game { class PlayMobility; }

namespace aver::editor {

// What the host (SandboxApp) lends the sequencer for one UI call.
struct SequenceHost {
    std::vector<scene::Entity> selection;                      // the editor's selected entities
    std::function<std::string(scene::Entity)> label;           // an entity's outliner label
    game::SeqCameraPose editorCamera;                          // the free-fly camera right now
    std::function<void(const game::SeqCameraPose&)> setEditorCamera;
    std::function<void()> markLevelDirty;                      // the level differs from its file
    f32 vpX = 0, vpY = 0, vpW = 0, vpH = 0;                    // viewport rect, for first placement
    bool levelFocused = false;                                 // the Level tab has keyboard focus
};

// What the frame loop tells tick().
struct SequenceTickCtx {
    f32  dt = 0;
    bool playActive = false;   // any kind of Play is running
    bool playPaused = false;
};

class SequenceEditor {
public:
    // ---- level lifecycle ----
    // Forgets everything without touching the world (the level is gone or being replaced).
    void reset();
    // Takes the level's first sequence; placementEntities[i] is the entity placement i became.
    void load(const std::vector<fmt::OcSequence>& seqs, const std::vector<scene::Entity>& placementEntities);
    // Writes w.sequences from the model; slotOf maps entity -> the placement slot it is saved in.
    // A sequence with no tracks writes nothing.
    void save(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf) const;

    // ---- mode ----
    void enter();
    void leave();
    bool active() const { return active_; }

    // ---- frame ----
    // Between animSystem().tick and World::flush: advances and applies the sequence.
    void tick(const SequenceTickCtx& ctx);

    // ---- Play ----
    // After PlayMobility::begin: starts the sequence from 0 when it autoplays and seeds its actors.
    void beginPlay(game::PlayMobility& mobility);
    void endPlay();
    bool playRunning() const { return playRunning_; }
    bool drivesTransform(scene::Entity e) const;

    // ---- bases ----
    // Writes the pre-sequence transforms back and forgets them (the next evaluate re-applies).
    void restoreBases();
    bool basesApplied() const { return !bases_.empty(); }
    void markDirty() { evalDirty_ = true; }

    // ---- view ----
    // The camera the viewport should show instead of the editor's: the pilot view in Animate,
    // the sequence camera in Play (never while ejected). False when the editor's own applies.
    bool viewPose(bool playEjected, game::SeqCameraPose& out) const;
    // The emissive multiplier for e while the sequence is previewing or playing.
    bool emissiveScale(scene::Entity e, f32 out[3]) const;

    // ---- UI ----
    void drawModePanel(SequenceHost& host);
    void drawTimeline(SequenceHost& host);
    bool pilotCamera() const { return pilot_; }
    bool emissiveActive() const { return previewing(); }

private:
    bool previewing() const { return playRunning_ || (active_ && !playActiveLast_); }
    void syncPlayer();
    void edited();                                   // after any model change
    void captureBase(scene::Entity e);
    void captureMissingBases();
    void evaluateNow();

    int  findTrack(fmt::OcSeqTrackKind kind, scene::Entity e) const;
    void addTrack(fmt::OcSeqTrackKind kind, scene::Entity e);
    void removeTrack(int index);
    void addKeyAtPlayhead();
    void removeSelectedKey();
    int  moveKey(fmt::OcSeqTrack& tr, int index, f64 t);
    void fillKey(const fmt::OcSeqTrack& tr, fmt::OcSeqKey& key) const;
    std::string trackLabel(const fmt::OcSeqTrack& tr) const;
    fmt::OcSeqKey* selectedKey();
    void clampSelection();

    fmt::OcSequence model_;
    game::SequencePlayer player_;
    SequenceHost* host_ = nullptr;                   // valid only inside a draw call
    std::unordered_map<u32, Transform> bases_;       // entity -> CLocal before the sequence wrote it
    bool active_ = false;
    bool playActiveLast_ = false;
    bool playRunning_ = false;
    bool evalDirty_ = true;
    bool pilot_ = false;
    f64  editTime_ = 0;                              // the editor playhead, kept across Play
    int  selTrack_ = -1;
    int  selKey_ = -1;
    bool dragKey_ = false;
    bool dragRuler_ = false;
    std::string status_;
};

} // namespace aver::editor

#endif
