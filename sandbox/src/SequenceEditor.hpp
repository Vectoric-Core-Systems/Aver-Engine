#pragma once
// The level sequence editor behind the editor's Animate mode: the level's sequences stored in the
// .ocworld, edited on a timeline, previewed in the viewport and played in Play.
//
// SandboxApp owns one and calls it at a few hook points; the model, the preview state and all the UI
// live here. A track bound to an actor holds a scene::Entity while editing; load() and save() convert
// to and from placement indices and names (SequenceTrackOps.hpp). The logic that needs no UI lives in
// SequenceTrackOps and SequenceCurveOps so it can be tested headless.
//
// Camera paths: K keys the editor camera on a camera track; playback and scrubbing look through the
// sequence camera; run mode (beginRun) is the command-line capture driver and the render queue's
// pose-at-time. docs/SEQUENCER.md, docs/EDITOR.md Phase 15.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelSequence.hpp"
#include "aver/game/SequenceDirector.hpp"

#include "SnapshotUndo.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"

namespace aver::game { class PlayMobility; }
namespace aver::rhi { struct LineVertex; }

namespace aver::editor {

// What the host (SandboxApp) lends the sequencer for one UI call.
struct SequenceHost {
    std::vector<scene::Entity> selection;                      // the editor's selected entities
    std::function<std::string(scene::Entity)> label;           // an entity's outliner label
    std::function<bool(scene::Entity)> canKey;                 // a saved placement a transform track may drive
    game::SeqCameraPose editorCamera;                          // the free-fly camera right now
    std::function<void(const game::SeqCameraPose&)> setEditorCamera;
    std::function<void()> markLevelDirty;                      // the level differs from its file
    f32 vpX = 0, vpY = 0, vpW = 0, vpH = 0;                    // viewport rect, for first placement
    bool levelFocused = false;                                 // the Level tab has keyboard focus
    // Bindings by name (lights, class actors and placements with a name).
    std::function<scene::Entity(const std::string&)> findByName;
    // The name a track binds an actor by: a placement's name, else a light / class actor's name when it is
    // unique in the level, else empty ("rename it first").
    std::function<std::string(scene::Entity)> bindName;
    std::function<int(const std::string&)> nameCount;         // actors that share a name
    std::function<bool(scene::Entity)> canBind;               // any actor a non-transform track may drive
    // Content paths: full path <-> content-relative, forward slashes.
    std::function<std::string(const std::string&)> contentRelative;
    std::function<std::string(const std::string&)> contentFull;
};

// What the frame loop tells tickPre().
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
    // Takes every sequence of the level; placementEntities[i] is the entity placement i became, findByName
    // resolves `bind` names (null = names stay unresolved).
    void load(const std::vector<fmt::OcSequence>& seqs, const std::vector<scene::Entity>& placementEntities,
              const std::function<scene::Entity(const std::string&)>& findByName = {});
    // Writes w.sequences from the model; slotOf maps entity -> the placement slot it is saved in, bindName
    // names an actor that is not a placement. A sequence with no tracks writes nothing.
    void save(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf,
              const std::function<std::string(scene::Entity)>& bindName = {}) const;

    // ---- mode ----
    void enter();
    void leave();
    bool active() const { return active_; }

    // ---- frame ----
    // Before the animation graphs and AnimSystem::tick: advances the preview or Play's director and evaluates
    // the Pre phase (property, visibility, animation, events, audio, sub-sequences).
    void tickPre(const SequenceTickCtx& ctx);
    // After the animation tick, before World::flush: the Post phase (transform, emissive, camera).
    void tickPost();

    // ---- Play ----
    // After PlayMobility::begin: starts the autoplay sequences from 0 and seeds their actors.
    void beginPlay(game::PlayMobility& mobility, const game::SequenceHooks& hooks);
    void endPlay();
    bool playRunning() const { return playRunning_; }
    bool drivesTransform(scene::Entity e) const;
    // Every entity a track names (a streamed level keeps them loaded).
    std::vector<scene::Entity> trackEntities() const;
    // The ABI relay's user pointer: the director Play runs (empty outside Play).
    game::SequenceDirector* director() { return &director_; }
    // Hooks for the preview's audio audition (the host builds them: audio ABI, event bus).
    void setHooks(const game::SequenceHooks& h);
    bool hasHooks() const { return hooksSet_; }

    // ---- bases ----
    // Puts back everything the preview wrote (transforms, properties, visibility, animation) and forgets it
    // (the next evaluate re-applies).
    void restoreBases();
    bool basesApplied() const { return !player_.baseStore().empty(); }
    void markDirty() { evalDirty_ = true; }

    // ---- view ----
    // The camera the viewport should show instead of the editor's: the pilot view in Animate (locked,
    // playing, scrubbing or running), the sequence camera in Play (never while ejected). False when
    // the editor's own applies.
    bool viewPose(bool playEjected, game::SeqCameraPose& out) const;
    // The emissive multiplier for e while the sequence is previewing or playing.
    bool emissiveScale(scene::Entity e, f32 out[3]) const;

    // ---- camera path overlay (Animate mode) ----
    // Every free camera track as lines (the selected one bright), cut markers and the live camera at the
    // playhead. False when there is nothing to draw.
    bool pathLines(std::vector<rhi::LineVertex>& out) const;
    // Changes whenever pathLines() would give different lines.
    u64 pathStamp() const;

    // ---- run mode: the command-line capture driver (--sequence-play) and the render queue ----
    // True when the CURRENT sequence has a camera (a free camera track with keys, or an actor camera).
    bool hasCamera() const;
    // True when any sequence of the level does.
    bool anyCamera() const;
    // Makes the sequence called `name` the current one; an empty name picks the first with a camera.
    // False, with `why` listing the level's sequence names, when there is none.
    bool selectRunSequence(const std::string& name, std::string* why);
    void beginRun();
    void endRun();
    bool running() const { return running_; }
    // Frames in one pass: ceil(length * fps); the end time itself is the loop point.
    i64  runFrameCount() const;
    // Poses the sequence for frame n (time = n / fps). False once n is past the last frame.
    bool runFrame(i64 n);
    f64  runFps() const { return 1.0 / game::seqStepSeconds(model_); }
    // The render queue's accessors: poses the sequence at an arbitrary time (clamped to [0, length]),
    // for sub-frame samples and for output rates other than the sequence's own. Offline: sampled tracks
    // only, nothing triggered, nothing audible.
    f64  runLength() const { return model_.length; }
    const std::string& sequenceName() const { return model_.name; }
    std::vector<std::string> sequenceNames() const;
    void runAt(f64 seconds);

    // ---- blend shapes ----
    // Keys the weight of a blend shape (a face pose when `isPose`) on the actor at the playhead: the Property track
    // CMorphTargets.morph.<name> (or .pose.<name>) is created when the actor has none, and holds one key with `value`
    // (an existing key at the playhead is overwritten). The track plays through MorphSystem's sequencer layer. Returns the
    // track index, -1 (status set) when the actor is not valid or already lacks Morph Targets.
    int keyMorph(scene::Entity e, const std::string& name, bool isPose, f32 value);

    // ---- sequences ----
    usize sequenceCount() const { return others_.size() + 1; }
    // Creates / duplicates / renames / deletes; each returns false (status set) when refused. The new
    // sequence becomes the current one.
    bool newSequence(const std::string& name);
    bool duplicateSequence();
    bool renameSequence(const std::string& name);
    bool deleteSequence();
    bool switchSequence(usize slot);
    // .ocseq: import copies the file's sequences into the level, export writes the current one (refused
    // while a bound actor has no unique name). Paths are full paths.
    bool importOcseq(const std::string& fullPath, const std::function<scene::Entity(const std::string&)>& findByName = {});
    bool exportOcseq(const std::string& fullPath);
    const std::string& status() const { return status_; }

    // ---- UI ----
    void drawModePanel(SequenceHost& host);
    void drawTimeline(SequenceHost& host);
    bool pilotCamera() const { return pilot_; }
    bool emissiveActive() const { return previewing(); }

    // ---- UI state shared with the curve view (SequenceCurveView.cpp) ----
    enum class View : u8 { DopeSheet, Curves };

private:
    struct DragItem {
        fmt::OcSeqKey key;
        bool selected = false;
        bool primary = false;
    };
    // What a press on a lane landed on.
    enum class LaneDrag : u8 { None, Keys, SectionMove, TrimStart, TrimEnd };

    bool previewing() const { return playRunning_ || (active_ && !playActiveLast_); }
    bool viewIsPilot() const { return active_ && !playActiveLast_ && (pilot_ || player_.playing() || dragRuler_ || running_); }
    void syncPlayer();
    void edited();                                   // after any model change
    void pushUndo() { undo_.push(model_); }
    void undoEdit();
    void redoEdit();
    void releaseUndrivenBases();
    std::vector<fmt::OcSequence> takeAll();
    void adopt(std::vector<fmt::OcSequence>&& all, usize slot);

    int  findTrack(fmt::OcSeqTrackKind kind, scene::Entity e) const;
    int  addTrack(fmt::OcSeqTrackKind kind, scene::Entity e);
    int  addPropertyTrack(scene::Entity e, const std::string& qualified);
    int  addAssetTrack(fmt::OcSeqTrackKind kind, scene::Entity e, const std::string& assetPath);
    int  addCameraTrack(scene::Entity actor);
    void removeTrack(int index);
    void clearTrack(int index);
    void moveTrack(int from, int to);
    void setMute(int index, bool mute);
    void keyCamera();                                // K: the editor camera on a camera track at the playhead
    void keySelectedTrack();                         // Shift+K: the selected track at the playhead
    void keyObjects(const std::vector<scene::Entity>& ents);   // a transform key at the playhead, tracks created as needed
    void drawKeyObjectButton(const char* idLabel, f32 width);  // "Key <name>" for the keyable part of the selection
    void addKeyAtPlayhead();
    void addEventKey(const std::string& name);
    void addSectionAtPlayhead(const std::string& assetPath);
    void dropAsset(int track, f64 t, const std::string& assetPath, scene::Entity target);
    void removeSelectedKeys();
    void goToKey(int track, int key);
    void setSelectedInterp(fmt::OcSeqInterp interp);
    int  moveKey(fmt::OcSeqTrack& tr, int index, f64 t);
    void applyDrag(fmt::OcSeqTrack& tr, f64 delta);
    void applySectionDrag(fmt::OcSeqTrack& tr, f64 delta);
    void fillKey(const fmt::OcSeqTrack& tr, fmt::OcSeqKey& key) const;
    std::string trackLabel(const fmt::OcSeqTrack& tr) const;
    const char* bindBadge(const fmt::OcSeqTrack& tr, bool& red) const;
    fmt::OcSeqKey* selectedKey();
    void selectTrack(int index);
    void selectOnly(int key);
    void clampSelection();
    int  selectedCount() const;
    void stepFrames(int frames);
    void setAudition(bool on);
    bool animationTracks() const;
    void drawKeyDetails(SequenceHost& host);
    void drawAddTrackMenu(SequenceHost& host);
    void drawSequenceBar(SequenceHost& host);
    void drawPropertyPicker(SequenceHost& host);
    void drawLanes(SequenceHost& host, f32 laneX0, f32 nameW, f32 laneW, f32 rowH, f32 ui);
    // Defined in SequenceCurveView.cpp.
    void drawCurveView(SequenceHost& host, f32 laneX0, f32 nameW, f32 laneW, f32 ui);

    fmt::OcSequence model_;                          // the current sequence, in the editing form
    std::vector<fmt::OcSequence> others_;            // the level's other sequences, in the editing form
    usize curSlot_ = 0;                              // where model_ sits among the level's sequences
    SnapshotUndo<fmt::OcSequence> undo_;
    game::SequencePlayer player_;                    // Animate preview (SeqMode::Preview) and run mode (Offline)
    game::SequenceDirector director_;                // Play (SeqMode::Live), every sequence of the level
    game::SeqLibrary library_;
    game::SequenceHooks hooks_;
    bool hooksSet_ = false;
    std::vector<fmt::OcSequence> view_;              // the player's copy of every sequence (dense targets), in slot order
    std::vector<scene::Entity> dense_;               // view_'s placement table
    SequenceHost* host_ = nullptr;                   // valid only inside a draw call
    bool active_ = false;
    bool playActiveLast_ = false;
    bool playRunning_ = false;
    bool running_ = false;
    bool evalDirty_ = true;
    bool pilot_ = false;                             // lock the viewport to the sequence camera
    bool fixedStep_ = false;                         // preview advances 1 / fps per frame, not wall time
    bool audition_ = false;                          // preview plays audio sections while the clock moves
    f64  editTime_ = 0;                              // the editor playhead, kept across Play
    u64  editRev_ = 0;                               // bumps on every model or selection change
    int  selTrack_ = -1;
    int  selKey_ = -1;                               // the primary key of selTrack_
    std::vector<char> keySel_;                       // per key of selTrack_: selected
    LaneDrag drag_ = LaneDrag::None;
    bool dragRuler_ = false;
    bool dragStarted_ = false;                       // the drag has changed something (undo pushed)
    f32  dragX0_ = 0;
    std::vector<DragItem> dragOrig_;                 // the track's keys as they were at the press
    std::string status_;
    View view_mode_ = View::DopeSheet;               // Dope sheet | Curves
    std::string eventName_ = "Event";                // the Event name field
    std::string assetField_;                         // the section asset path field
    std::string propFilter_;                         // the property picker's filter
    std::string renameBuf_;                          // sequence rename / new name field
    std::string ocseqPath_;                          // import / export path field (content-relative)
    int  pendingPopup_ = 0;                          // 1 property picker, 2 asset track, opened at window level next frame
    fmt::OcSeqTrackKind assetKind_ = fmt::OcSeqTrackKind::Audio;
    scene::Entity propPickEntity_ = scene::kInvalidEntity;
    std::string linkedFile_;                         // the .ocseq the current sequence was opened from (full path)
    bool linkedDirty_ = false;                       // it differs from that file

    // Timeline view: pxPerSec_ 0 = the whole sequence fits the lane; viewStart_ is the time at the lane's left edge.
    f32  pxPerSec_ = 0;
    f64  viewStart_ = 0;
    f64  followTime_ = -1;                           // the playhead last frame, to scroll it into view on a change
    f32  scrollGrab_ = 0;                            // scrollbar: where in the thumb it was grabbed, px
    f64  dragView0_ = 0;                             // viewStart_ at the key-drag press

    // Curve view state (SequenceCurveView.cpp).
    struct CurveState {
        f64 v0 = 0, v1 = 1;                          // visible value range
        bool fit = true;                             // re-fit the value range to the selected track's keys
        u32 hidden = 0;                              // channels switched off (bit per channel)
        int dragKind = 0;                            // 0 none, 1 key, 2 arrive handle, 3 leave handle, 4 box select
        int dragKey = -1;
        u32 dragChannel = 0;
        bool dragMoved = false;
        f32 boxX0 = 0, boxY0 = 0;
        f64 dragT0 = 0, dragV0 = 0;
    } curve_;
};

} // namespace aver::editor

#endif
