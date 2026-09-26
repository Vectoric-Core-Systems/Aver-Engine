#pragma once
// The particle effect editor tab: a .ocparticle opened as an asset. There is no list to select
// among -- an effect IS the whole record, unlike .ocsnd's graph of many nodes or .ocbt's tree -- so
// this tab is one parameter panel covering every record OcParticle.hpp's grammar defines, beside a
// live CPU preview of the effect playing.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching BtEditor.hpp/SoundEditor.hpp before it:
//   #include "ParticleEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeParticleEditor);   // APPENDED -- order is precedence
// plus one more: shutdownParticleEditors() at the single shutdown site next to
// shutdownActorEditors()/shutdownAnimEditors() -- see its own comment in the .cpp for why this tab
// needs one even though it registers no new render feature.
//
// UNDO THROUGH THE SHARED SnapshotUndo<State> TEMPLATE (SnapshotUndo.hpp), the fourth user, now that
// GraphEditor/SoundEditor/BtEditor's own hand-rolled triads have all been migrated onto it in the
// same change. This tab shipped with no undo at all the night it landed -- see the plan note this
// paragraph replaces -- specifically so that stage would not have to consolidate a fourth
// hand-written copy; with the template already extracted, adding this tab costs SnapshotUndo.hpp
// nothing and this tab a small pushUndo() before each field write. ActorEditor and AnimEditor use it
// too since 2026-09-16; AnimEditor snapshots only its editable parts, since its clip_ carries full
// sample arrays a whole-state copy per edit would duplicate.
//
// GUARDED ON AVER_MODULE_PARTICLES, whole-file. This header unconditionally #includes aver/formats/
// OcParticle.hpp (Aver.Formats.Particles), which only exists when Aver.Particles does -- sandbox/
// CMakeLists.txt links both `if(TARGET ...)`. Every include of this header (SandboxApp.cpp) is
// itself guarded the same way, but ParticleEditor.cpp includes it unconditionally as its own first
// line, so the guard has to live here too or a module-off tree fails at this file's own
// `#include "aver/formats/OcParticle.hpp"` with C1083 before either guard is ever consulted.
#if AVER_MODULE_PARTICLES
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcParticle.hpp"

#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace aver::editor {

// A starter effect for the Content Browser's "New Particle Effect": a small warm ember burst --
// one of the format test's own four expressiveness shapes (OcParticleTest.cpp: smoke, snow, embers,
// mist), not a weapon effect. Declared here so a test can check it, matching btStarterTree()/
// snStarterGraph()'s own precedent of exposing the starter for exactly that reason.
particles::ParticleEffect pxStarterEffect(fmt::OcParticleExtras* outExtras = nullptr);

// ---- the tab ------------------------------------------------------------------------------------
//
// Declared in the header rather than hidden behind the factory, for BtEditor.hpp's own reason:
// tests/editor constructs this tab directly to exercise load/save/dirty/the field setters/the
// preview tick with no ImGui and no window. draw()'s ImGui half is `#if AVER_WITH_IMGUI`-only, but
// tickPreview() is NOT gated on it at all -- it touches no ImGui symbol, so a headless test can drive
// the preview simulation itself, not merely the load/save bookkeeping around it.
class ParticleEditor final : public AssetEditor {
public:
    explicit ParticleEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Restores the preview/params split to its default proportion and persists that -- see
    // EditorWidgets.hpp's own comment for why this tab's split is a FRACTION (SplitPane) rather than
    // ActorEditor's pixel-width convention. Declared unconditionally (matching draw()/save() above)
    // but only does anything `#if AVER_WITH_IMGUI` -- see the .cpp: a headless build never lays the
    // panels out at all, so there is nothing for a reset to restore.
    void resetLayout() override;

    // Reachable for a headless test, for BtEditor/SoundEditor's own reason.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const particles::ParticleEffect& effect() const { return effect_; }
    const fmt::OcParticleExtras& extras() const { return extras_; }
    void markDirty() { dirty_ = true; }

    // Snapshot undo, through the shared SnapshotUndo<State> template -- see the header comment above.
    // State bundles effect_ AND extras_, matching GraphEditor's own {graph, displayPos} pair: extras_
    // carries the name, which setName() edits and which an undo must therefore restore too.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- field edits, as MEMBERS, not free functions ------------------------------------------
    //
    // Unlike Bt/SoundEditor's free-function structural edits (which exist to keep an add/remove/
    // reparent operation testable independent of a tab's own bookkeeping): this format has no
    // structure to edit, one record, no nodes, no links, so every edit here already IS the whole
    // operation. Each bundles exactly the fields OcParticle.hpp's grammar writes on one record line
    // (EMISSION's rate+burstCount+maxParticles together, SPEED's min+max together, and so on), and
    // each clamps to whatever range the format's own parser enforces at load time (see OcParticle.cpp)
    // so a value typed in the UI can never be one the save path would go on to reject.
    void setName(std::string name);
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
    // Deliberately NOT aver::particles::ParticleSystem, and NOT the process-global
    // particles::particleEffects()/scene::World::instance() -- see the .cpp for why reaching into
    // either from an editor tab would be wrong: World is a process-wide SINGLETON (there is exactly
    // one, and it is whichever level is actually open), so spawning a preview entity into it would
    // put a phantom emitter into the level the author is editing; and the effect library is a shared
    // id space keyed by content-relative path hashes that an unsaved, mid-edit effect must not
    // collide with. This reimplements the same DECIDED-2 spawn/integrate rules (ParticleSystem.cpp)
    // directly against whatever effect() currently holds, including unsaved edits, entirely local to
    // this tab.
    void tickPreview(f32 dt);
    void restartPreview();
    const std::vector<particles::Particle>& previewParticles() const { return preview_; }

private:
    void loadFromDisk();
    // Resyncs nameBuf_/texBuf_ from effect_/extras_ -- loadFromDisk()'s own two snprintf calls,
    // factored out so undo()/redo() can share them: an undo that changes extras_.name or
    // effect_.textureId without this would leave the visible text field showing the PRE-undo value
    // until the author happened to touch it.
    void syncEditBuffers();

    std::string path_;
    particles::ParticleEffect effect_;
    fmt::OcParticleExtras extras_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;

    // Through the shared SnapshotUndo<State> template (SnapshotUndo.hpp).
    struct UndoState { particles::ParticleEffect effect; fmt::OcParticleExtras extras; };
    SnapshotUndo<UndoState> history_;

    // Preview simulation state. A FIXED seed (re-applied by restartPreview(), including from
    // loadFromDisk()) rather than a wall-clock one: an author toggling Restart, or reloading the
    // tab, gets the same spread and lifetimes back, which is what makes "does this look right" a
    // question worth answering twice in a row.
    std::vector<particles::Particle> preview_;
    std::mt19937 previewRng_{12345u};
    f32 previewAccum_ = 0.0f;          // fractional particles owed by emissionRate*dt
    bool previewBurstFired_ = false;   // the once-only burst, exactly like CParticleEmitter's own
    bool previewPlaying_ = true;

    // ImGui-only bookkeeping (a debounced preview-panel resize, and "camera framed once already"),
    // plain PODs so they need no #if -- only the two functions that actually call ImGui:: below do.
    f64  previewLastClock_ = 0.0;
    bool previewFramed_ = false;
    u32  previewPendingW_ = 0, previewPendingH_ = 0;
    f64  previewResizeDue_ = 0.0;

    // Editable-text scratch buffers, synced from effect_/extras_ on every load/reload -- plain
    // char arrays so they need no #if either, even though only drawParams() (ImGui) reads them.
    char nameBuf_[128] = {};
    char texBuf_[32] = {};

    // The preview/params divider. A plain SplitPane (EditorWidgets.hpp), not gated on
    // AVER_WITH_IMGUI, for the same reason the ImGui-only bookkeeping just above IS: an editor tab's
    // fields must keep compiling headless even though only draw() and resetLayout() actually touch it.
    SplitPane split_;

#if AVER_WITH_IMGUI
    void drawParams();
    void drawPreviewPane(Engine& e);
#endif
};

// Creates a particle editor for a .ocparticle, else nullptr.
std::unique_ptr<AssetEditor> makeParticleEditor(const std::string& path);

// Releases whatever this editor registered with the render device, before the device goes. Called
// from the single shutdown site in SandboxApp::onShutdown, beside shutdownActorEditors()/
// shutdownAnimEditors() -- see the .cpp for why this is a deliberate no-op today, kept for the same
// reason those two exist at all: AnimEditor.cpp:1263-1276 documents what skipping this looks like
// (a clean exit that access-violates AFTER the last frame rendered correctly) for the shape this tab
// would take on the day it stops being one.
void shutdownParticleEditors();

} // namespace aver::editor
#endif  // AVER_MODULE_PARTICLES
