#pragma once
// The particle effect editor tab: a .ocparticle opened as an asset. A file holds a SYSTEM of emitters (OCPARTICLE 2) or one
// (OCPARTICLE 1): the tab keeps a particles::ParticleSystemDef, an emitter list, and for the selected emitter a module stack,
// render / lighting / sorting / events / culling sections and the system's user parameters, beside a live preview that
// draws through a private CPU-tier ParticleRenderer into the shared ActorPreview surface.
//
// THE FILES, to keep the Debug object files small: this one (state, load / save, undo, the preview simulation) compiles
// headless -- tests/editor builds it with AVER_WITH_IMGUI undefined -- and ParticleEditorUi.cpp holds every ImGui call.
// The edit logic itself is ImGui-free in ParticleStackModel.hpp.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching BtEditor.hpp/SoundEditor.hpp before it:
//   #include "ParticleEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeParticleEditor);   // APPENDED -- order is precedence
// plus shutdownParticleEditors() at the single shutdown site next to shutdownActorEditors()/shutdownAnimEditors(): this tab now
// owns a ParticleRenderer (the preview's), which must release its GPU objects before the device goes.
//
// UNDO THROUGH THE SHARED SnapshotUndo<State> TEMPLATE (SnapshotUndo.hpp): the state is the whole system plus the selection.
//
// GUARDED ON AVER_MODULE_PARTICLES, whole-file. This header unconditionally #includes aver/formats/OcParticle.hpp
// (Aver.Formats.Particles), which only exists when Aver.Particles does -- sandbox/CMakeLists.txt links both `if(TARGET ...)`.
#if AVER_MODULE_PARTICLES
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcParticle.hpp"
#include "aver/particles/EmitterSim.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// A starter effect for the Content Browser's "New Particle Effect": a small warm ember burst -- one of the format test's own
// four expressiveness shapes (OcParticleTest.cpp: smoke, snow, embers, mist), not a weapon effect. Declared here so a test can
// check it, matching btStarterTree()/snStarterGraph()'s own precedent of exposing the starter for exactly that reason.
particles::ParticleEffect pxStarterEffect(fmt::OcParticleExtras* outExtras = nullptr);

// What the tab owns on the GPU (the preview's ParticleRenderer), type-erased so this header names no rendering type.
// ParticleEditorUi.cpp derives it; shutdownParticleEditors() calls shutdown() on every live one before the device goes.
struct ParticleEditorGpu {
    virtual ~ParticleEditorGpu() = default;
    virtual void shutdown() = 0;
};

// ---- the tab ------------------------------------------------------------------------------------
//
// Declared in the header rather than hidden behind the factory, for BtEditor.hpp's own reason: tests/editor constructs this
// tab directly to exercise load/save/dirty/the edits/the preview tick with no ImGui and no window. tickPreview() touches no
// ImGui symbol, so a headless test drives the preview simulation itself.
class ParticleEditor final : public AssetEditor {
public:
    explicit ParticleEditor(std::string path);
    ~ParticleEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    bool usesSharedPreview() const override { return true; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Restores the preview/params split to its default proportion and persists that.
    void resetLayout() override;

    // Reachable for a headless test, for BtEditor/SoundEditor's own reason.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    // The whole system, and the selected emitter (what the single-effect accessors below mean).
    const particles::ParticleSystemDef& system() const { return def_; }
    int selected() const { return sel_; }
    void select(int emitter);
    const particles::ParticleEffect& effect() const { return def_.emitters[static_cast<usize>(sel_)]; }
    const fmt::OcParticleExtras& extras() const { return extras_; }
    void markDirty() { dirty_ = true; }

    // Snapshot undo, through the shared SnapshotUndo<State> template. State bundles the system and the selection.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- edits ---------------------------------------------------------------------------------------
    //
    // The generic entry points: run `fn` on the system (or the selected emitter); when it returns true the tab gets one undo
    // entry and goes dirty. The ParticleStackModel.hpp functions are meant to be called through these, so every edit is
    // clamped to what the parser accepts and none can leave a half-applied change.
    // `coalesceKey` (non-zero) merges consecutive edits by the same widget on neighbouring UI frames into ONE undo entry, so a
    // drag is one step back rather than one per frame; `frame` is the UI frame counter. Headless callers pass neither.
    bool editSystem(const std::function<bool(particles::ParticleSystemDef&)>& fn, u32 coalesceKey = 0, u64 frame = 0);
    bool editEffect(const std::function<bool(particles::ParticleEffect&)>& fn, u32 coalesceKey = 0, u64 frame = 0);
    // Compute bounds: runs the emitter for 3 s of simulated time and writes BOUNDS.
    bool computeBounds();

    // ---- field edits of the SELECTED emitter, each clamped to the parser's range -----------------------
    void setName(std::string name);   // the system's NAME
    void setShape(particles::EmitterShape shape);
    void setShapeSize(Vec3 size);
    void setBlend(rhi::BlendMode blend);
    void setEmission(f32 rate, u32 burstCount, u32 maxParticles);
    void setLifetime(f32 lifetimeMin, f32 lifetimeMax);
    void setDirection(Vec3 direction, f32 spreadDeg);
    void setSpeed(f32 speedMin, f32 speedMax);
    void setGravity(Vec3 gravity);
    void setDamping(f32 damping);
    void setSize(f32 sizeStart, f32 sizeEnd);
    void setColorStart(const f32 rgba[4]);
    void setColorEnd(const f32 rgba[4]);
    void setTextureId(u64 id);
    void setReceivesGI(bool on);

    // ---- the live preview's OWN simulation ------------------------------------------------------
    //
    // An EmitterSim per emitter (the same CPU executor the engine's own CPU tier runs), ticked locally: NOT
    // aver::particles::ParticleSystem and NOT the process-global effect library or scene::World -- World is a process-wide
    // SINGLETON (whichever level is open), so spawning a preview entity into it would put a phantom emitter into the level the
    // author is editing, and an unsaved, mid-edit effect must not collide with the shared id space. Events route between the
    // emitters here exactly as ParticleSystem routes them (one tick later).
    void tickPreview(f32 dt);
    void restartPreview();
    // The selected emitter's live particles (empty before the first tick).
    const std::vector<particles::Particle>& previewParticles() const;
    // Every emitter's live particles together.
    u32 previewTotal() const;
    // Per-emitter solo / mute for the preview only (not saved).
    void setMuted(int emitter, bool muted);
    bool muted(int emitter) const;
    void setSolo(int emitter);   // -1 clears
    int solo() const { return solo_; }
    // The preview's simulations, for the drawing half.
    const std::vector<particles::EmitterSim>& previewSims() const { return sims_; }
    bool previewPlaying() const { return previewPlaying_; }
    void setPreviewPlaying(bool on) { previewPlaying_ = on; }

private:
    void loadFromDisk();
    void syncEditBuffers();
    void ensureSims();
    particles::ParticleEffect& sel() { return def_.emitters[static_cast<usize>(sel_)]; }
    bool beginEdit();   // loaded_ guard

    std::string path_;
    particles::ParticleSystemDef def_;
    int sel_ = 0;
    fmt::OcParticleExtras extras_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;

    // Through the shared SnapshotUndo<State> template (SnapshotUndo.hpp).
    struct UndoState { particles::ParticleSystemDef def; int sel = 0; };
    SnapshotUndo<UndoState> history_;

    // Preview simulation state. A FIXED seed rather than a wall-clock one: an author toggling Restart, or reloading the tab,
    // gets the same spread and lifetimes back, which is what makes "does this look right" a question worth answering twice.
    std::vector<particles::EmitterSim> sims_;
    std::vector<u32> simSignature_;                        // render kind / trail / ring: a change re-inits that emitter
    std::vector<f32> accum_;
    std::vector<std::vector<particles::SpawnRequest>> inbox_, nextInbox_;
    std::vector<u8> muted_;
    int solo_ = -1;
    bool previewBurstFired_ = false;
    bool previewPlaying_ = true;
    u32 lastCoalesceKey_ = 0;
    u64 lastCoalesceFrame_ = 0;
    f32 previewTime_ = 0.0f;

    // ImGui-only bookkeeping, plain PODs so they need no #if -- only the UI translation unit reads them.
    f64  previewLastClock_ = 0.0;
    bool previewFramed_ = false;
    u32  previewPendingW_ = 0, previewPendingH_ = 0;
    f64  previewResizeDue_ = 0.0;

    // Editable-text scratch buffers, synced from the state on every load / reload / undo.
    char nameBuf_[128] = {};
    char texBuf_[32] = {};
    char emitterNameBuf_[64] = {};
    char paramNameBuf_[64] = {};
    char meshBuf_[260] = {};
    int moduleToAdd_ = 0;
    int selParam_ = 0;
    int newParamKind_ = 0;
    char moduleFilter_[64] = {};

    std::shared_ptr<ParticleEditorGpu> gpu_;   // ParticleEditorUi.cpp creates and owns it
    SplitPane split_;

    friend struct ParticleEditorUiAccess;
    void drawParams();
    void drawPreviewPane(Engine& e);
};

// Creates a particle editor for a .ocparticle, else nullptr.
std::unique_ptr<AssetEditor> makeParticleEditor(const std::string& path);

// Releases whatever this editor registered with the render device, before the device goes: the private ParticleRenderer every
// open tab built for its preview. Called from the single shutdown site in SandboxApp::onShutdown.
void shutdownParticleEditors();

} // namespace aver::editor
#endif  // AVER_MODULE_PARTICLES
