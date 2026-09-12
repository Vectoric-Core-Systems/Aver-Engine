// The .ocparticle editor tab. See the header for the three-line SandboxApp hook, for why every edit
// is a member function rather than a free structural edit, and for its undo through the shared
// SnapshotUndo<State> template.
#include "ParticleEditor.hpp"

// GUARDED ON AVER_MODULE_PARTICLES, whole-file -- see ParticleEditor.hpp's own comment. With the
// header's content compiled out, everything below would otherwise reference an undeclared
// ParticleEditor class and undeclared particles::/fmt:: types; this .cpp is listed unconditionally
// in sandbox/CMakeLists.txt's SOURCES, so it has to reduce to an empty translation unit on its own
// rather than relying on the caller not to compile it.
#if AVER_MODULE_PARTICLES
#include "EditorKeybinds.hpp"
#include "EditorIcons.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>

// THE RENDERING/ENGINE INCLUDES BELOW ARE INSIDE THIS GUARD, DELIBERATELY, unlike Bt/SoundEditor.cpp
// (whose includes are all unconditional because neither has a live preview at all). ParticleEditor
// is the first of the header-declared, headlessly-testable tabs to ALSO carry one, and
// tests/editor/CMakeLists.txt's ParticleEditorLoadSaveTest compiles this .cpp with AVER_WITH_IMGUI
// undefined and links no RHI/render/runtime module -- exactly the configuration BtEditorTest/
// SoundEditorTest already prove works for their own headless halves. Pulling in ActorPreview.hpp /
// PreviewMeshCache.hpp / Engine.hpp unconditionally would make that test target need Aver.RHI,
// Aver.Render.ActorPreview and Aver.Runtime just to satisfy #include paths it never actually calls
// into (drawPreviewPane() is itself `#if AVER_WITH_IMGUI`-only) -- a needless, and needlessly
// fragile, extra link surface for a target whose whole point is not needing a device or a window.
#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "ActorEditor.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  include "aver/runtime/Engine.hpp"
#endif

namespace aver::editor {

// A starter effect for the Content Browser's "New Particle Effect" -- a small warm ember burst, one
// of the format test's own four expressiveness shapes (smoke, snow, embers, mist; see
// OcParticleTest.cpp), not a weapon effect. A point-shaped, omni (spreadDeg 180) burst with no
// continuous rate reads immediately as "one puff", the shape an author reaches for most often
// starting from nothing.
particles::ParticleEffect pxStarterEffect(fmt::OcParticleExtras* outExtras) {
    particles::ParticleEffect e;
    e.shape = particles::EmitterShape::Point;
    e.emissionRate = 0.0f;
    e.burstCount = 40;
    e.maxParticles = 128;
    e.lifetimeMin = 0.6f; e.lifetimeMax = 1.1f;
    e.direction = Vec3{0.0f, 0.0f, 1.0f};
    e.spreadDeg = 180.0f;
    e.speedMin = 80.0f; e.speedMax = 220.0f;
    e.gravity = Vec3{0.0f, 0.0f, -150.0f};
    e.damping = 0.3f;
    e.sizeStart = 6.0f; e.sizeEnd = 1.0f;
    e.colorStart[0] = 1.0f;  e.colorStart[1] = 0.55f; e.colorStart[2] = 0.12f; e.colorStart[3] = 1.0f;
    e.colorEnd[0]   = 0.35f; e.colorEnd[1]   = 0.04f; e.colorEnd[2]   = 0.0f;  e.colorEnd[3]   = 0.0f;
    e.blend = rhi::BlendMode::Additive;
    e.receivesGI = false;   // an ember IS its own light source -- see ParticleTypes.hpp's own comment
    if (outExtras) outExtras->name = "New Ember Burst";
    return e;
}

namespace {

// ---- the editor's OWN preview simulator ---------------------------------------------------------
//
// DUPLICATED FROM aver::particles::ParticleSystem.cpp'S OWN (private) shapeOffset()/coneSample()
// ON PURPOSE. See ParticleEditor.hpp's tickPreview() comment for why this tab cannot tick the real,
// process-global ParticleSystem: that system reads scene::World::instance(), the ONE world that is
// whichever level the author actually has open, and it resolves effects through
// particles::particleEffects(), a shared id space keyed by content-relative path hashes. Ticking the
// real system for a preview would mean either spawning a phantom entity into the live level, or
// stomping the id space with a possibly-unsaved, mid-edit effect -- both wrong for a tab whose whole
// point is to preview edits BEFORE they are anywhere but this struct. Reimplementing the same small,
// documented DECIDED-2 physics (spawn shape, cone spread, gravity, damping, lifetime) locally costs
// about 40 lines and buys total isolation from the level being edited.

f32 pxUniform(std::mt19937& rng, f32 lo, f32 hi) {
    if (hi <= lo) return lo;
    std::uniform_real_distribution<f32> d(lo, hi);
    return d(rng);
}

Vec3 pxShapeOffset(const particles::ParticleEffect& fx, std::mt19937& rng) {
    switch (fx.shape) {
    case particles::EmitterShape::Sphere: {
        std::uniform_real_distribution<f32> d(-1.0f, 1.0f);
        Vec3 p;
        do { p = Vec3{d(rng), d(rng), d(rng)}; } while (p.sizeSquared() > 1.0f);
        return p * fx.shapeSize.x;
    }
    case particles::EmitterShape::Box: {
        const f32 x = fx.shapeSize.x > 0.0f ? pxUniform(rng, -fx.shapeSize.x, fx.shapeSize.x) : 0.0f;
        const f32 y = fx.shapeSize.y > 0.0f ? pxUniform(rng, -fx.shapeSize.y, fx.shapeSize.y) : 0.0f;
        const f32 z = fx.shapeSize.z > 0.0f ? pxUniform(rng, -fx.shapeSize.z, fx.shapeSize.z) : 0.0f;
        return Vec3{x, y, z};
    }
    case particles::EmitterShape::Point:
    default:
        return Vec3{0.0f, 0.0f, 0.0f};
    }
}

Vec3 pxConeSample(const Vec3& axis, f32 spreadDeg, std::mt19937& rng) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 spreadRad = spreadDeg * (kPi / 180.0f);
    const f32 cosSpread = std::cos(spreadRad < 0.0f ? 0.0f : (spreadRad > kPi ? kPi : spreadRad));
    const f32 cosTheta = pxUniform(rng, cosSpread, 1.0f);
    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const f32 phi = pxUniform(rng, 0.0f, 2.0f * kPi);

    const Vec3 hint = std::fabs(axis.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 right = cross(hint, axis).getSafeNormal();
    const Vec3 up = cross(axis, right);
    return axis * cosTheta + right * (sinTheta * std::cos(phi)) + up * (sinTheta * std::sin(phi));
}

} // namespace

// ================================================================================== the tab =======

ParticleEditor::ParticleEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void ParticleEditor::loadFromDisk() {
    std::string why;
    particles::ParticleEffect loadedFx;
    fmt::OcParticleExtras loadedEx;
    if (!fmt::loadOcparticle(path_, loadedFx, &loadedEx, &why)) {
        loaded_ = false;
        loadError_ = why;
        restartPreview();
        return;
    }
    effect_ = loadedFx;
    extras_ = loadedEx;
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();

    syncEditBuffers();

    previewFramed_ = false;   // a freshly (re)loaded effect gets its camera distance picked again
    restartPreview();
}

void ParticleEditor::syncEditBuffers() {
    std::snprintf(nameBuf_, sizeof nameBuf_, "%s", extras_.name.c_str());
    if (effect_.textureId != 0)
        std::snprintf(texBuf_, sizeof texBuf_, "%016llX", static_cast<unsigned long long>(effect_.textureId));
    else
        texBuf_[0] = '\0';
}

std::string ParticleEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor
    // whose dirty() is true (AssetEditor.cpp) -- a '*' here would double it, exactly the bug
    // Sound/BtEditor's own title() comments already document for this codebase.
    return std::filesystem::path(path_).filename().string() + "###particle:" + path_;
}

bool ParticleEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // saveOcparticle re-reads whatever is already at path_ and MERGES into it (see OcParticle.hpp's
    // own round-trip contract) -- comments, blank lines and any record this format does not
    // recognise all survive, at their original position. A no-edit load/save round trip is therefore
    // byte-identical BEFORE this tab ever existed: OcParticleTest.cpp's testFileRoundTrip already
    // proves it at the format layer, and this call is the only thing standing between that guarantee
    // and the file on disk -- it does not re-derive or weaken it.
    if (!fmt::saveOcparticle(path_, effect_, &extras_, why)) return false;
    dirty_ = false;
    return true;
}

void ParticleEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS -- Bt/SoundEditor's own rule: reloading here would discard what the
    // author typed because something else touched the file, which an editor must never do on its own.
    if (dirty_) {
        AVER_WARN("[ParticleEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them",
                  path_);
        return;
    }
    loadFromDisk();
}

// ---- undo -----------------------------------------------------------------------------------------
//
// Through the shared SnapshotUndo<State> template (SnapshotUndo.hpp) -- see the header comment for
// why this tab did not have one before tonight's consolidation stage. Every setter below pushes
// BEFORE mutating, exactly like Bt/Sound/GraphEditor's own field-edit call sites, and after the
// existing no-op guard where one exists -- so retyping the same value again costs no undo entry,
// consistent with how those three editors' own structural edits already behave.

void ParticleEditor::pushUndo() {
    history_.push(UndoState{effect_, extras_});
}

void ParticleEditor::undo() {
    UndoState s{effect_, extras_};
    if (!history_.undo(s)) return;
    effect_ = std::move(s.effect);
    extras_ = std::move(s.extras);
    dirty_ = true;
    syncEditBuffers();   // extras_.name / effect_.textureId may have just changed under the text fields
}

void ParticleEditor::redo() {
    UndoState s{effect_, extras_};
    if (!history_.redo(s)) return;
    effect_ = std::move(s.effect);
    extras_ = std::move(s.extras);
    dirty_ = true;
    syncEditBuffers();
}

// ---- field edits ----------------------------------------------------------------------------------

void ParticleEditor::setName(std::string name) {
    if (!loaded_ || extras_.name == name) return;
    pushUndo();
    extras_.name = std::move(name);
    dirty_ = true;
}

void ParticleEditor::setShape(particles::EmitterShape shape) {
    if (!loaded_ || effect_.shape == shape) return;
    pushUndo();
    effect_.shape = shape;
    dirty_ = true;
}

void ParticleEditor::setShapeSize(Vec3 size) {
    if (!loaded_) return;
    pushUndo();
    effect_.shapeSize = size;
    dirty_ = true;
}

void ParticleEditor::setBlend(rhi::BlendMode blend) {
    if (!loaded_ || effect_.blend == blend) return;
    pushUndo();
    effect_.blend = blend;
    dirty_ = true;
}

void ParticleEditor::setEmission(f32 rate, u32 burstCount, u32 maxParticles) {
    if (!loaded_) return;
    pushUndo();
    effect_.emissionRate = std::max(0.0f, rate);
    effect_.burstCount = burstCount;
    // >=1: a maxParticles of 0 would mean nothing can ever spawn (ParticleSystem.cpp's own `room`
    // computation floors at 0 room), a degenerate effect the format's parser would still accept but
    // that reads as broken rather than authored.
    effect_.maxParticles = std::max(1u, maxParticles);
    dirty_ = true;
}

void ParticleEditor::setLifetime(f32 lifetimeMin, f32 lifetimeMax) {
    if (!loaded_) return;
    pushUndo();
    lifetimeMin = std::max(0.0f, lifetimeMin);
    lifetimeMax = std::max(lifetimeMin, lifetimeMax);   // the parser requires min <= max
    effect_.lifetimeMin = lifetimeMin;
    effect_.lifetimeMax = lifetimeMax;
    dirty_ = true;
}

void ParticleEditor::setDirection(Vec3 direction, f32 spreadDeg) {
    if (!loaded_) return;
    pushUndo();
    effect_.direction = direction;
    effect_.spreadDeg = std::clamp(spreadDeg, 0.0f, 180.0f);   // the parser's own range
    dirty_ = true;
}

void ParticleEditor::setSpeed(f32 speedMin, f32 speedMax) {
    if (!loaded_) return;
    pushUndo();
    speedMin = std::max(0.0f, speedMin);
    speedMax = std::max(speedMin, speedMax);
    effect_.speedMin = speedMin;
    effect_.speedMax = speedMax;
    dirty_ = true;
}

void ParticleEditor::setGravity(Vec3 gravity) {
    if (!loaded_) return;
    pushUndo();
    effect_.gravity = gravity;
    dirty_ = true;
}

void ParticleEditor::setDamping(f32 damping) {
    if (!loaded_) return;
    pushUndo();
    // [0, 1): OcParticle.cpp's own range check -- a negative or >=1 damping is a velocity MULTIPLIER
    // above one, so a mistyped sign would accelerate every particle without bound rather than damp
    // it. Clamped here so this field can never be edited into a value save() would go on to refuse.
    effect_.damping = std::clamp(damping, 0.0f, 0.999f);
    dirty_ = true;
}

void ParticleEditor::setSize(f32 sizeStart, f32 sizeEnd) {
    if (!loaded_) return;
    pushUndo();
    effect_.sizeStart = std::max(0.0f, sizeStart);
    effect_.sizeEnd = std::max(0.0f, sizeEnd);
    dirty_ = true;
}

void ParticleEditor::setColorStart(const f32 rgba[4]) {
    if (!loaded_) return;
    pushUndo();
    for (int i = 0; i < 4; ++i) effect_.colorStart[i] = std::clamp(rgba[i], 0.0f, 1.0f);
    dirty_ = true;
}

void ParticleEditor::setColorEnd(const f32 rgba[4]) {
    if (!loaded_) return;
    pushUndo();
    for (int i = 0; i < 4; ++i) effect_.colorEnd[i] = std::clamp(rgba[i], 0.0f, 1.0f);
    dirty_ = true;
}

void ParticleEditor::setTextureId(u64 id) {
    if (!loaded_ || effect_.textureId == id) return;
    pushUndo();
    effect_.textureId = id;
    dirty_ = true;
}

void ParticleEditor::setReceivesGI(bool on) {
    if (!loaded_ || effect_.receivesGI == on) return;
    pushUndo();
    effect_.receivesGI = on;
    dirty_ = true;
}

// ---- the preview simulation -------------------------------------------------------------------

void ParticleEditor::restartPreview() {
    preview_.clear();
    previewAccum_ = 0.0f;
    previewBurstFired_ = false;
    previewRng_.seed(12345u);   // a FIXED seed: Restart reproduces the same spread/lifetimes every time
}

void ParticleEditor::tickPreview(f32 dt) {
    if (!loaded_ || dt <= 0.0f) return;
    const particles::ParticleEffect& fx = effect_;

    u32 toSpawn = 0;
    if (!previewBurstFired_) {
        // The stopped -> playing edge, exactly once, matching scene::CParticleEmitter's own
        // semantics (ParticleSystem.cpp): a tab that has just loaded (or just been restarted) is
        // "freshly playing".
        toSpawn += fx.burstCount;
        previewBurstFired_ = true;
    }
    if (fx.emissionRate > 0.0f) {
        previewAccum_ += fx.emissionRate * dt;
        const u32 whole = static_cast<u32>(previewAccum_);
        toSpawn += whole;
        previewAccum_ -= static_cast<f32>(whole);
    }

    if (toSpawn > 0) {
        const u32 room = fx.maxParticles > preview_.size()
                             ? fx.maxParticles - static_cast<u32>(preview_.size()) : 0;
        const u32 spawnNow = std::min(toSpawn, room);
        Vec3 axis = fx.direction.getSafeNormal();
        if (axis.sizeSquared() < 0.5f) axis = Vec3{0.0f, 0.0f, 1.0f};
        preview_.reserve(std::min<usize>(fx.maxParticles, preview_.size() + spawnNow));
        for (u32 n = 0; n < spawnNow; ++n) {
            particles::Particle p{};
            p.position = pxShapeOffset(fx, previewRng_);
            const Vec3 dir = pxConeSample(axis, fx.spreadDeg, previewRng_);
            p.velocity = dir * pxUniform(previewRng_, fx.speedMin, fx.speedMax);
            p.age = 0.0f;
            p.lifetime = std::max(0.001f, pxUniform(previewRng_, fx.lifetimeMin, fx.lifetimeMax));
            p.seed = previewRng_();
            preview_.push_back(p);
        }
    }

    // Semi-implicit Euler, matching ParticleSystem.cpp's own updateParticles() order (velocity
    // before position), and the same in-place compaction of anything that aged past its lifetime.
    usize w = 0;
    for (usize r = 0; r < preview_.size(); ++r) {
        particles::Particle p = preview_[r];
        p.age += dt;
        if (p.age >= p.lifetime) continue;
        p.velocity += fx.gravity * dt;
        if (fx.damping > 0.0f) p.velocity *= std::max(0.0f, 1.0f - fx.damping * dt);
        p.position += p.velocity * dt;
        preview_[w++] = p;
    }
    preview_.resize(w);
}

// ---- drawing ------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

void ParticleEditor::drawParams() {
    ImGui::SeparatorText("Identity");
    if (ImGui::InputText("Name", nameBuf_, sizeof nameBuf_)) setName(nameBuf_);

    ImGui::SeparatorText("Emitter shape");
    {
        static const char* kShapeNames[] = {"Point", "Sphere", "Box"};
        int idx = static_cast<int>(effect_.shape);
        if (ImGui::Combo("Shape", &idx, kShapeNames, 3))
            setShape(static_cast<particles::EmitterShape>(idx));

        Vec3 size = effect_.shapeSize;
        bool changed = false;
        switch (effect_.shape) {
        case particles::EmitterShape::Sphere:
            changed |= ImGui::DragFloat("Radius (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            ImGui::TextDisabled("(a sphere ignores the other two SHAPE components)");
            break;
        case particles::EmitterShape::Box:
            changed |= ImGui::DragFloat3("Half-extent XYZ (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            break;
        case particles::EmitterShape::Point:
        default:
            ImGui::TextDisabled("A point emitter has no spawn offset.");
            break;
        }
        if (changed) setShapeSize(size);
    }

    ImGui::SeparatorText("Blend & texture");
    {
        // rhi::BlendMode's own declaration order does not match this list, so the mapping is explicit
        // both ways rather than cast through the enum's numeric value.
        static const char* kBlendNames[] = {"Opaque", "Alpha blend", "Additive", "Premultiplied alpha"};
        static const rhi::BlendMode kBlendValues[] = {
            rhi::BlendMode::Opaque, rhi::BlendMode::AlphaBlend,
            rhi::BlendMode::Additive, rhi::BlendMode::PremultipliedAlpha};
        int idx = 3;
        for (int i = 0; i < 4; ++i) if (kBlendValues[i] == effect_.blend) { idx = i; break; }
        if (ImGui::Combo("Blend", &idx, kBlendNames, 4)) setBlend(kBlendValues[idx]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The particle shader always premultiplies rgb by alpha, so only\n"
                              "Additive and Premultiplied alpha compose correctly against it --\n"
                              "see ParticleTypes.hpp's own comment on this field.");

        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("Texture GUID (hex)", texBuf_, sizeof texBuf_, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(ICON_SAVE " Apply##tex")) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(texBuf_, &end, 16);
            if (end && end != texBuf_) setTextureId(static_cast<u64>(v));
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_CLOSE " Clear##tex")) { texBuf_[0] = '\0'; setTextureId(0); }
        ImGui::TextDisabled("Unused this slice: the renderer always draws its own procedural dot\n"
                            "regardless of this value (see ParticleTypes.hpp's own comment).");
    }

    ImGui::SeparatorText("Emission");
    {
        f32 rate = effect_.emissionRate;
        int burst = static_cast<int>(effect_.burstCount);
        int maxP = static_cast<int>(effect_.maxParticles);
        bool changed = false;
        changed |= ImGui::DragFloat("Rate (particles/sec)", &rate, 0.5f, 0.0f, 100000.0f, "%.2f");
        changed |= ImGui::DragInt("Burst (on play)", &burst, 1.0f, 0, 100000);
        changed |= ImGui::DragInt("Max particles", &maxP, 1.0f, 1, 1000000);
        if (changed)
            setEmission(rate, static_cast<u32>(std::max(0, burst)), static_cast<u32>(std::max(1, maxP)));
    }

    ImGui::SeparatorText("Lifetime, direction & speed");
    {
        f32 lo = effect_.lifetimeMin, hi = effect_.lifetimeMax;
        bool lchanged = false;
        lchanged |= ImGui::DragFloat("Lifetime min (s)", &lo, 0.02f, 0.0f, 3600.0f, "%.3f");
        lchanged |= ImGui::DragFloat("Lifetime max (s)", &hi, 0.02f, 0.0f, 3600.0f, "%.3f");
        if (lchanged) setLifetime(lo, hi);

        Vec3 dir = effect_.direction;
        f32 spread = effect_.spreadDeg;
        bool dchanged = false;
        dchanged |= ImGui::DragFloat3("Direction XYZ", &dir.x, 0.02f, -1.0f, 1.0f, "%.3f");
        dchanged |= ImGui::DragFloat("Spread (deg; 0=exact, 180=omni)", &spread, 0.5f, 0.0f, 180.0f, "%.1f");
        if (dchanged) setDirection(dir, spread);

        f32 sLo = effect_.speedMin, sHi = effect_.speedMax;
        bool schanged = false;
        schanged |= ImGui::DragFloat("Speed min (cm/s)", &sLo, 1.0f, 0.0f, 1000000.0f, "%.1f");
        schanged |= ImGui::DragFloat("Speed max (cm/s)", &sHi, 1.0f, 0.0f, 1000000.0f, "%.1f");
        if (schanged) setSpeed(sLo, sHi);

        Vec3 g = effect_.gravity;
        if (ImGui::DragFloat3("Gravity XYZ (cm/s^2)", &g.x, 1.0f, -1000000.0f, 1000000.0f, "%.1f"))
            setGravity(g);

        f32 damping = effect_.damping;
        if (ImGui::DragFloat("Damping (0..1)", &damping, 0.005f, 0.0f, 0.999f, "%.3f")) setDamping(damping);
    }

    ImGui::SeparatorText("Size & colour over life");
    {
        f32 s0 = effect_.sizeStart, s1 = effect_.sizeEnd;
        bool changed = false;
        changed |= ImGui::DragFloat("Size at birth (cm)", &s0, 0.2f, 0.0f, 100000.0f, "%.2f");
        changed |= ImGui::DragFloat("Size at death (cm)", &s1, 0.2f, 0.0f, 100000.0f, "%.2f");
        if (changed) setSize(s0, s1);

        f32 cs[4]; for (int i = 0; i < 4; ++i) cs[i] = effect_.colorStart[i];
        if (ImGui::ColorEdit4("Colour at birth", cs)) setColorStart(cs);
        f32 ce[4]; for (int i = 0; i < 4; ++i) ce[i] = effect_.colorEnd[i];
        if (ImGui::ColorEdit4("Colour at death", ce)) setColorEnd(ce);
    }

    ImGui::SeparatorText("Lighting");
    {
        bool gi = effect_.receivesGI;
        if (ImGui::Checkbox("Receives GI (ambient bounce light)", &gi)) setReceivesGI(gi);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off for an effect that IS its own light source (an ember, a spark) --\n"
                              "see ParticleTypes.hpp's own comment on receivesGI.");
    }

    if (!loadError_.empty()) {
        ImGui::SeparatorText("Load error");
        ImGui::TextWrapped(ICON_WARNING " %s", loadError_.c_str());
    }
}

// Draws the live preview into the SAME shared ActorPreview surface Mesh/Actor/Anim/Graph tabs use
// (sharedPreview(), ActorEditor.hpp) -- one texture, one orbit camera, already torn down by
// shutdownActorEditors(). See ParticleEditor.hpp's own comment on tickPreview() for the simulation
// half; this is the drawing half.
//
// VISUAL-ONLY. This was NOT verified against the runtime's actual look -- there is no automated way
// to check that a rendered image "looks right", and this comment says so instead of implying
// otherwise. Two things make it an APPROXIMATION even of what it does draw, both deliberate
// simplifications, not oversights:
//   1. Every live particle draws as a small CUBE through ActorPreview's ordinary opaque, lit
//      pipeline, never as the runtime's true camera-facing, additively/premultiplied-blended
//      billboard (aver::particles::ParticleRenderer, ParticleShaders.hpp). A correct billboard needs
//      the SAME eye-direction basis ActorPreview::buildViewProj derives from the camera's yaw/pitch,
//      and a subtly wrong derivation produces a plausible-looking but silently sideways sprite --
//      exactly the kind of confidently-wrong result this tree's own notes warn about repeatedly. A
//      cube needs no orientation at all, so it reads correctly from any angle; it costs looking like
//      a small box rather than a soft round dot, and it never shows additive glow or GI dimming.
//   2. BLEND mode, TEX and "Receives GI" are all authored and saved correctly, but do not change
//      what the preview looks like: ActorPreview has one fixed key-light-plus-fill shader, not the
//      runtime's blend-mode-selecting pipeline pair.
// What the preview DOES faithfully show: spawn shape and rate, the burst, lifetime, direction and
// spread, gravity, damping, and size/colour interpolated over each particle's age -- the same
// DECIDED-2 physics the runtime simulates, run locally (see tickPreview()'s own comment for why).
void ParticleEditor::drawPreviewPane(Engine& e) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview) {
        ImGui::TextDisabled("No preview on this backend.");
        return;
    }

    if (rhi::IDevice* dev = e.device()) {
        f32 cubeRadius = 1.0f;
        const rhi::MeshHandle cube =
            sharedPreviewMeshes().resolve(*dev, "Meshes/cube.ocmesh", &cubeRadius);
        std::vector<render::preview::PreviewDraw> draws;
        if (cube) {
            draws.reserve(preview_.size());
            for (const particles::Particle& p : preview_) {
                const f32 t = p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 1.0f;
                const f32 size = std::max(0.0f, effect_.sizeStart + (effect_.sizeEnd - effect_.sizeStart) * t);
                const f32 s = size * 0.5f;   // the built-in cube spans -1..1 -- edge length 2 locally

                render::preview::PreviewDraw d;
                d.mesh = cube;
                d.world[0] = s;    d.world[1] = 0.0f; d.world[2] = 0.0f;  d.world[3] = 0.0f;
                d.world[4] = 0.0f; d.world[5] = s;    d.world[6] = 0.0f;  d.world[7] = 0.0f;
                d.world[8] = 0.0f; d.world[9] = 0.0f; d.world[10] = s;    d.world[11] = 0.0f;
                d.world[12] = p.position.x; d.world[13] = p.position.y; d.world[14] = p.position.z;
                d.world[15] = 1.0f;
                for (int c = 0; c < 4; ++c)
                    d.baseColor[c] = effect_.colorStart[c] + (effect_.colorEnd[c] - effect_.colorStart[c]) * t;
                d.boundsRadius = cubeRadius;   // mesh-LOCAL units -- ActorPreview::frameAll multiplies
                d.metallic = 0.0f;
                d.roughness = 0.85f;
                draws.push_back(d);
            }
        }
        preview->setDrawList(std::move(draws));

        if (!previewFramed_) {
            // A FIXED estimate, set ONCE, rather than ActorPreview::frameAll() over draws_: that list
            // is a moving target here (particles spawn and die every tick), and re-framing off it
            // every frame would fight the author's own orbit/zoom instead of framing once like every
            // other tab does.
            const f32 reach = std::max({effect_.shapeSize.size(),
                                        effect_.speedMax * effect_.lifetimeMax, 50.0f});
            preview->camera().pivot[0] = 0.0f;
            preview->camera().pivot[1] = 0.0f;
            preview->camera().pivot[2] = 0.0f;
            preview->camera().distance = std::clamp(reach * 1.6f, 100.0f, 5000.0f);
            previewFramed_ = true;
        }
    }

    if (!preview->uiTextureId()) {
        ImGui::TextDisabled("No preview on this backend.");
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const u32 w = static_cast<u32>(std::max(64.0f, avail.x));
    const u32 h = static_cast<u32>(std::max(64.0f, avail.y));
    // DEBOUNCED: resize() waits for the GPU to go idle, so resizing on every frame of a drag would
    // stall the editor -- the same 0.25s debounce AnimEditor's own preview panel uses.
    if (w != previewPendingW_ || h != previewPendingH_) {
        previewPendingW_ = w; previewPendingH_ = h;
        previewResizeDue_ = ImGui::GetTime() + 0.25;
    } else if (previewResizeDue_ > 0.0 && ImGui::GetTime() >= previewResizeDue_) {
        previewResizeDue_ = 0.0;
        preview->resize(previewPendingW_, previewPendingH_);
    }
    ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), avail);
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            preview->camera().addOrbit(io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
        if (io.MouseWheel != 0.0f)
            preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);
    }
}

void ParticleEditor::draw(Engine& e) {
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    // THE CLOCK RUNS OFF ImGui's, not the engine's frame delta -- Sound/AnimEditor's own reason: this
    // tab only draws while it is the active dock tab, so an engine-dt clock would keep accumulating
    // while it was hidden and the preview would visibly jump on the way back.
    const f64 now = ImGui::GetTime();
    const f32 dt = previewLastClock_ > 0.0 ? static_cast<f32>(now - previewLastClock_) : 0.0f;
    previewLastClock_ = now;
    if (previewPlaying_) tickPreview(dt);

    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[ParticleEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(previewPlaying_ ? ICON_PAUSE " Pause preview" : ICON_PLAY " Play preview"))
        previewPlaying_ = !previewPlaying_;
    ImGui::SameLine();
    if (ImGui::Button(ICON_REFRESH " Restart preview")) restartPreview();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu live particle(s) in this preview", preview_.size());

    ImGui::Separator();

    const f32 leftW = ImGui::GetContentRegionAvail().x * 0.42f;
    const f32 h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##pxpreview", ImVec2(leftW, h), true)) drawPreviewPane(e);
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##pxparams", ImVec2(0, h), true)) drawParams();
    ImGui::EndChild();
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's own target, which deliberately leaves AVER_WITH_IMGUI
// undefined -- see tests/editor/CMakeLists.txt) still gets load/save/dirty/every field setter/the
// preview TICK (tickPreview() is not gated on ImGui at all); only the window is absent. Bt/Sound/
// GraphEditor's own #else branches do exactly this.
void ParticleEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeParticleEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocparticle") return nullptr;
    return std::make_unique<ParticleEditor>(path);
}

// See ParticleEditor.hpp's own comment on why this exists at all despite doing nothing today.
//
// This tab registers NO new rhi::IRenderFeature of its own. Its live preview draws through the SAME
// shared ActorPreview feature Mesh/Actor/Anim/Graph tabs already use (sharedPreview(),
// ActorEditor.hpp) -- already released by shutdownActorEditors() -- and its cube geometry through
// the SAME shared PreviewMeshCache (sharedPreviewMeshes()), likewise already released there. There
// is therefore nothing device-owned left for this function to release, and no static/global state of
// this tab's own that outlives a single draw() call either (preview_ and friends are per-instance
// members, destroyed with their ParticleEditor when its tab closes, well before device teardown).
//
// WIRED INTO THE SHUTDOWN SITE ANYWAY, beside shutdownActorEditors()/shutdownAnimEditors(), for two
// reasons. First, the brief for this tab is explicit that a render feature must be unregistered
// before the device goes, and AnimEditor.cpp:1263-1276 is the documented cost of assuming "I share
// ActorPreview so I have nothing to do" without writing the hook that proves it -- AnimEditor made
// precisely that assumption about its OWN feature and was wrong once already. Second, this tab's
// design is not guaranteed to stay this simple: the day a future slice gives it a real GPU
// particle-rendering path (see drawPreviewPane()'s own comment on why today's preview draws cubes
// through ActorPreview's ordinary pipeline rather than the runtime's actual additive/premultiplied
// billboard shader), that path will need exactly this call site already wired in, not a fifth one
// added under pressure the night it lands.
void shutdownParticleEditors() {
    // Nothing to release -- see the comment above. No render feature, no owned GPU resource, no
    // static cache of this tab's own.
}

} // namespace aver::editor
#endif  // AVER_MODULE_PARTICLES
