// The particle editor tab's ImGui half: the emitter list, the module stack, the render / lighting / sorting / events /
// culling sections, the user parameters, the preview pane and the tab chrome. Split from ParticleEditor.cpp so the headless
// test builds without ImGui and no Debug object file grows past the COFF section cap. Every edit goes through the tab's
// editEffect / editSystem and the ImGui-free functions in ParticleStackModel.hpp.
#include "ParticleEditor.hpp"

#if AVER_MODULE_PARTICLES && AVER_WITH_IMGUI
#include "ActorEditor.hpp"
#include "EditorIcons.hpp"
#include "EditorKeybinds.hpp"
#include "ParticleStackModel.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleRenderer.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/runtime/Engine.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aver::editor {
namespace detail {
void registerParticleEditorGpu(const std::shared_ptr<ParticleEditorGpu>& g);
} // namespace detail

namespace {

// The preview/params split -- a FRACTION (SplitPane), see EditorWidgets.hpp's top comment. 0.42 is this tab's long-standing default.
constexpr f32 kDefaultPreviewFraction = 0.42f;
constexpr const char* kPrefPreviewSplit = "particleEditor.previewSplit";

// The preview's private renderer and the frame's draw list. The hook ActorPreview calls captures a shared_ptr to this, so it
// outlives the tab safely; the stamp keeps a tab that is no longer on screen from drawing into another tab's preview.
struct PxGpu final : ParticleEditorGpu {
    particles::ParticleRenderer renderer;
    bool tried = false, ready = false;
    int stampFrame = -1;
    std::vector<particles::InstanceView> views;
    void shutdown() override {
        renderer.shutdown();
        ready = false;
        views.clear();
    }
};

u32 idOf(const char* label) { return static_cast<u32>(ImGui::GetID(label)); }
u64 frameNow() { return static_cast<u64>(ImGui::GetFrameCount()); }

bool isToggleParam(particles::ModuleKind kind, int pi) {
    using K = particles::ModuleKind;
    return (kind == K::UpdateAttractor && pi == 6) || (kind == K::UpdateCollidePlane && pi == 6) ||
           (kind == K::UpdateCollideSphere && pi == 6) || (kind == K::UpdateKillVolume && pi == 7);
}

const char* kindWord(u8 kind) { return kind == 0 ? "float" : (kind == 1 ? "vec3" : "color"); }

} // namespace

// ================================================================================== the sections ==

void ParticleEditor::drawParams() {
    using namespace particles;
    ParticleSystemDef& d = def_;
    const u64 frame = frameNow();

    // ---- identity ----
    ImGui::SeparatorText("Identity");
    if (ImGui::InputText("Name", nameBuf_, sizeof nameBuf_)) setName(nameBuf_);

    // ---- emitters ----
    ImGui::SeparatorText("Emitters");
    {
        for (usize i = 0; i < d.emitters.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool isSel = static_cast<int>(i) == sel_;
            char label[96];
            std::snprintf(label, sizeof label, "%zu  %s", i, d.emitters[i].name.empty() ? "(unnamed)" : d.emitters[i].name.c_str());
            if (ImGui::Selectable(label, isSel, 0, ImVec2(ImGui::GetContentRegionAvail().x - 90.0f * (ImGui::GetFontSize() / 16.0f), 0))) select(static_cast<int>(i));
            ImGui::SameLine();
            bool m = muted(static_cast<int>(i));
            if (ImGui::Checkbox("##mute", &m)) setMuted(static_cast<int>(i), m);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute this emitter in the preview (not saved)");
            ImGui::SameLine();
            const bool soloed = solo_ == static_cast<int>(i);
            if (ImGui::SmallButton(soloed ? "solo*" : "solo")) setSolo(soloed ? -1 : static_cast<int>(i));
            ImGui::PopID();
        }
        if (ImGui::Button(ICON_ADD " Add")) {
            int created = -1;
            editSystem([&](ParticleSystemDef& s) { created = pxmodel::addEmitter(s); return created >= 0; });
            if (created >= 0) select(created);
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            int created = -1;
            editSystem([&](ParticleSystemDef& s) { created = pxmodel::duplicateEmitter(s, sel_); return created >= 0; });
            if (created >= 0) { select(created); restartPreview(); }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(d.emitters.size() <= 1);
        if (ImGui::Button(ICON_DELETE " Delete")) {
            editSystem([&](ParticleSystemDef& s) { return pxmodel::removeEmitter(s, sel_); });
            select(sel_);
            restartPreview();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Up")) { if (editSystem([&](ParticleSystemDef& s) { return pxmodel::moveEmitter(s, sel_, sel_ - 1); })) select(sel_ - 1); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down")) { if (editSystem([&](ParticleSystemDef& s) { return pxmodel::moveEmitter(s, sel_, sel_ + 1); })) select(sel_ + 1); }

        if (ImGui::InputText("Emitter name", emitterNameBuf_, sizeof emitterNameBuf_, ImGuiInputTextFlags_EnterReturnsTrue))
            editSystem([&](ParticleSystemDef& s) { return pxmodel::renameEmitter(s, sel_, emitterNameBuf_); });
    }

    ImGui::TextDisabled("Editing emitter %d of %zu", sel_, d.emitters.size());
    ImGui::Separator();

    // ---- simulation ----
    ImGui::SeparatorText("Simulation");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        static const char* kTier[] = {"Auto", "CPU", "GPU"};
        int tier = static_cast<int>(e.simTier);
        if (ImGui::Combo("Simulate on", &tier, kTier, 3))
            editEffect([&](ParticleEffect& x) { x.simTier = static_cast<SimTier>(tier); return true; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto puts an emitter on the GPU when its max particles is 2048 or more and the device supports it.\n"
                              "The preview always runs on the CPU; the result is the same.");

        static const char* kShapeNames[] = {"Point", "Sphere", "Box", "Cylinder", "Disc"};
        int idx = static_cast<int>(e.shape);
        if (ImGui::Combo("Shape", &idx, kShapeNames, 5)) setShape(static_cast<EmitterShape>(idx));
        Vec3 size = e.shapeSize;
        bool changed = false;
        switch (e.shape) {
        case EmitterShape::Sphere:
            changed |= ImGui::DragFloat("Radius (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            break;
        case EmitterShape::Box:
            changed |= ImGui::DragFloat3("Half-extent XYZ (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            break;
        case EmitterShape::Cylinder:
            changed |= ImGui::DragFloat("Radius (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            changed |= ImGui::DragFloat("Height (cm)", &size.y, 1.0f, 0.0f, 100000.0f, "%.1f");
            break;
        case EmitterShape::Disc:
            changed |= ImGui::DragFloat("Radius (cm)", &size.x, 1.0f, 0.0f, 100000.0f, "%.1f");
            break;
        case EmitterShape::Point:
        default:
            ImGui::TextDisabled("A point emitter has no spawn offset.");
            break;
        }
        if (changed) setShapeSize(size);
        if (e.shape == EmitterShape::Sphere || e.shape == EmitterShape::Cylinder || e.shape == EmitterShape::Disc) {
            bool shell = e.shapeShell;
            if (ImGui::Checkbox("Surface only (shell)", &shell))
                editEffect([&](ParticleEffect& x) { x.shapeShell = shell; return true; });
        }

        f32 rate = e.emissionRate;
        int burst = static_cast<int>(e.burstCount);
        int maxP = static_cast<int>(e.maxParticles);
        bool ec = false;
        ec |= ImGui::DragFloat("Rate (particles/sec)", &rate, 0.5f, 0.0f, 1000000.0f, "%.2f");
        ec |= ImGui::DragInt("Burst (on play)", &burst, 1.0f, 0, 2000000);
        ec |= ImGui::DragInt("Max particles", &maxP, 10.0f, 1, 4000000);
        if (ec) setEmission(rate, static_cast<u32>(std::max(0, burst)), static_cast<u32>(std::max(1, maxP)));

        f32 lo = e.lifetimeMin, hi = e.lifetimeMax;
        bool lc = false;
        lc |= ImGui::DragFloat("Lifetime min (s)", &lo, 0.02f, 0.0f, 3600.0f, "%.3f");
        lc |= ImGui::DragFloat("Lifetime max (s)", &hi, 0.02f, 0.0f, 3600.0f, "%.3f");
        if (lc) setLifetime(lo, hi);

        Vec3 dir = e.direction;
        f32 spread = e.spreadDeg;
        bool dc = false;
        dc |= ImGui::DragFloat3("Direction XYZ", &dir.x, 0.02f, -1.0f, 1.0f, "%.3f");
        dc |= ImGui::DragFloat("Spread (deg; 0 exact, 180 omni)", &spread, 0.5f, 0.0f, 180.0f, "%.1f");
        if (dc) setDirection(dir, spread);

        f32 sLo = e.speedMin, sHi = e.speedMax;
        bool sc = false;
        sc |= ImGui::DragFloat("Speed min (cm/s)", &sLo, 1.0f, 0.0f, 1000000.0f, "%.1f");
        sc |= ImGui::DragFloat("Speed max (cm/s)", &sHi, 1.0f, 0.0f, 1000000.0f, "%.1f");
        if (sc) setSpeed(sLo, sHi);

        Vec3 g = e.gravity;
        if (ImGui::DragFloat3("Gravity XYZ (cm/s^2)", &g.x, 1.0f, -1000000.0f, 1000000.0f, "%.1f")) setGravity(g);
        f32 damping = e.damping;
        if (ImGui::DragFloat("Damping (0..1)", &damping, 0.005f, 0.0f, 0.999f, "%.3f")) setDamping(damping);
    }

    // ---- the module stack ----
    ImGui::SeparatorText("Modules");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        ImGui::TextDisabled("%zu of %u modules. Spawn modules run once per particle; update modules every tick, in this order.",
                            e.modules.size(), kMaxModulesPerEmitter);
        for (usize mi = 0; mi < e.modules.size(); ++mi) {
            const Module& m = e.modules[mi];
            const ModuleInfo& info = moduleInfo(m.kind);
            ImGui::PushID(static_cast<int>(mi));
            bool enabled = m.enabled;
            if (ImGui::Checkbox("##en", &enabled))
                editEffect([&](ParticleEffect& x) { return pxmodel::setModuleEnabled(x, static_cast<int>(mi), enabled); });
            ImGui::SameLine();
            const bool open = ImGui::TreeNodeEx(info.key, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::SameLine();
            if (ImGui::SmallButton("^")) editEffect([&](ParticleEffect& x) { return pxmodel::moveModule(x, static_cast<int>(mi), static_cast<int>(mi) - 1); });
            ImGui::SameLine();
            if (ImGui::SmallButton("v")) editEffect([&](ParticleEffect& x) { return pxmodel::moveModule(x, static_cast<int>(mi), static_cast<int>(mi) + 1); });
            ImGui::SameLine();
            const bool removeIt = ImGui::SmallButton(ICON_CLOSE);
            if (open) {
                for (u32 pi = 0; pi < info.paramCount; ++pi) {
                    const ModuleParam& p = m.p[pi];
                    ImGui::PushID(static_cast<int>(pi));
                    if (p.userSlot >= 0 && static_cast<usize>(p.userSlot) < d.params.size()) {
                        ImGui::Text("%s = @%s", info.names[pi], d.params[static_cast<usize>(p.userSlot)].name.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("unbind")) editEffect([&](ParticleEffect& x) { return pxmodel::unbindModuleParam(x, static_cast<int>(mi), static_cast<int>(pi)); });
                    } else {
                        f32 v = p.value;
                        bool changed = false;
                        const std::string label = info.names[pi];
                        if (isToggleParam(m.kind, static_cast<int>(pi))) {
                            bool on = v != 0.0f;
                            if (ImGui::Checkbox(label.c_str(), &on)) { v = on ? 1.0f : 0.0f; changed = true; }
                        } else if (m.kind == ModuleKind::UpdateKillVolume && pi == 0) {
                            int shape = v < 0.5f ? 0 : 1;
                            static const char* kKill[] = {"Box (half-extents hx hy hz)", "Sphere (radius hx)"};
                            if (ImGui::Combo(label.c_str(), &shape, kKill, 2)) { v = static_cast<f32>(shape); changed = true; }
                        } else if (m.kind == ModuleKind::UpdateCurlNoise && pi == 3) {
                            int oct = static_cast<int>(v);
                            if (ImGui::DragInt(label.c_str(), &oct, 0.1f, 1, 4)) { v = static_cast<f32>(oct); changed = true; }
                        } else {
                            changed = ImGui::DragFloat(label.c_str(), &v, std::max(0.01f, std::fabs(v) * 0.01f), -1000000.0f, 1000000.0f, "%.4g");
                        }
                        if (changed)
                            editEffect([&](ParticleEffect& x) { return pxmodel::setModuleParam(x, static_cast<int>(mi), static_cast<int>(pi), v); },
                                       idOf(label.c_str()), frame);
                        if (!d.params.empty()) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("@")) ImGui::OpenPopup("bind");
                            if (ImGui::BeginPopup("bind")) {
                                ImGui::TextDisabled("Bind to a user parameter");
                                for (usize s = 0; s < d.params.size(); ++s) {
                                    const UserParam& up = d.params[s];
                                    const u32 comps = pxmodel::userParamComponents(up);
                                    for (u32 c = 0; c < comps; ++c) {
                                        static const char* kC[4] = {"x", "y", "z", "w"};
                                        char item[96];
                                        if (up.kind == 0) std::snprintf(item, sizeof item, "@%s", up.name.c_str());
                                        else std::snprintf(item, sizeof item, "@%s.%s", up.name.c_str(), kC[c]);
                                        if (ImGui::Selectable(item))
                                            editSystem([&](ParticleSystemDef& sd) {
                                                return pxmodel::bindModuleParam(sd, sel_, static_cast<int>(mi), static_cast<int>(pi), static_cast<int>(s), static_cast<int>(c));
                                            });
                                    }
                                }
                                ImGui::EndPopup();
                            }
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (removeIt) { editEffect([&](ParticleEffect& x) { return pxmodel::removeModule(x, static_cast<int>(mi)); }); break; }
        }

        // The searchable Add module popup: all 13, filtered by what is typed.
        if (ImGui::Button(ICON_ADD " Add module")) { moduleFilter_[0] = '\0'; ImGui::OpenPopup("addmodule"); }
        if (ImGui::BeginPopup("addmodule")) {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##filter", "search modules", moduleFilter_, sizeof moduleFilter_);
            std::string needle = moduleFilter_;
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (u32 k = 0; k < static_cast<u32>(ModuleKind::Count); ++k) {
                const ModuleInfo& info = moduleInfo(static_cast<ModuleKind>(k));
                if (!needle.empty() && std::string(info.key).find(needle) == std::string::npos) continue;
                if (ImGui::Selectable(info.key)) {
                    editEffect([&](ParticleEffect& x) { return pxmodel::addModule(x, static_cast<ModuleKind>(k)) >= 0; });
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // ---- render ----
    ImGui::SeparatorText("Render");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        static const char* kRender[] = {"Sprite", "Ribbon (trail)", "Ribbon (linked)", "Mesh"};
        int rk = e.render == RenderKind::Sprite ? 0 : (e.render == RenderKind::Mesh ? 3 : (e.linked ? 2 : 1));
        if (ImGui::Combo("Kind", &rk, kRender, 4))
            editEffect([&](ParticleEffect& x) {
                x.render = rk == 0 ? RenderKind::Sprite : (rk == 3 ? RenderKind::Mesh : RenderKind::Ribbon);
                x.linked = rk == 2;
                return true;
            });

        if (e.render == RenderKind::Sprite) {
            static const char* kAlign[] = {"Camera facing", "Velocity (stretched)", "Axis"};
            int al = static_cast<int>(e.align);
            if (ImGui::Combo("Alignment", &al, kAlign, 3)) editEffect([&](ParticleEffect& x) { x.align = static_cast<SpriteAlign>(al); return true; });
            if (e.align == SpriteAlign::Velocity) {
                f32 st = e.stretch;
                if (ImGui::DragFloat("Stretch", &st, 0.01f, 0.0f, 100.0f, "%.2f"))
                    editEffect([&](ParticleEffect& x) { x.stretch = std::max(0.0f, st); return true; }, idOf("Stretch"), frame);
            } else if (e.align == SpriteAlign::Axis) {
                Vec3 ax = e.alignAxis;
                if (ImGui::DragFloat3("Axis XYZ", &ax.x, 0.02f, -1.0f, 1.0f, "%.3f"))
                    editEffect([&](ParticleEffect& x) { x.alignAxis = ax; return true; }, idOf("Axis XYZ"), frame);
            }
        }
        if (e.render == RenderKind::Ribbon && !e.linked) {
            int pts = e.trailPoints;
            f32 seg = e.trailSegmentCm, fade = e.trailFade;
            bool c = false;
            c |= ImGui::DragInt("Trail points", &pts, 0.2f, 2, static_cast<int>(kMaxTrailPoints));
            c |= ImGui::DragFloat("Segment length (cm)", &seg, 0.1f, 0.1f, 1000.0f, "%.2f");
            c |= ImGui::DragFloat("Fade along length", &fade, 0.01f, 0.0f, 1.0f, "%.2f");
            if (c) editEffect([&](ParticleEffect& x) {
                x.trailPoints = static_cast<u8>(std::clamp(pts, 2, static_cast<int>(kMaxTrailPoints)));
                x.trailSegmentCm = std::max(0.1f, seg);
                x.trailFade = std::clamp(fade, 0.0f, 1.0f);
                return true;
            }, idOf("Trail"), frame);
        }
        if (e.render == RenderKind::Ribbon && e.linked) {
            int cap = e.linkedCapacity;
            if (ImGui::DragInt("Ring capacity", &cap, 1.0f, 2, static_cast<int>(kMaxLinkedCapacity)))
                editEffect([&](ParticleEffect& x) { x.linkedCapacity = static_cast<u16>(std::clamp(cap, 2, static_cast<int>(kMaxLinkedCapacity))); return true; }, idOf("Ring"), frame);
            ImGui::TextDisabled("Consecutive spawns are joined; the oldest segment is overwritten when the ring is full.");
        }
        if (e.render == RenderKind::Mesh) {
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::InputText("Mesh (.ocmesh path)", meshBuf_, sizeof meshBuf_, ImGuiInputTextFlags_EnterReturnsTrue))
                editEffect([&](ParticleEffect& x) {
                    x.meshPath = meshBuf_;
                    for (char& ch : x.meshPath) if (ch == '\\') ch = '/';
                    x.meshId = x.meshPath.empty() ? 0 : fnv1a64(std::string_view(x.meshPath));
                    return true;
                });
            static const char* kOrient[] = {"Along velocity", "Velocity + spin", "Fixed (spin about Z)"};
            int mo = e.meshOrient;
            if (ImGui::Combo("Orientation", &mo, kOrient, 3)) editEffect([&](ParticleEffect& x) { x.meshOrient = static_cast<u8>(mo); return true; });
            ImGui::TextDisabled("Up to 2,048 triangles; mesh particles use the particle lighting model, not material graphs.");
        }

        // blend and texture
        static const char* kBlendNames[] = {"Opaque", "Alpha blend", "Additive", "Premultiplied alpha"};
        static const rhi::BlendMode kBlendValues[] = {rhi::BlendMode::Opaque, rhi::BlendMode::AlphaBlend, rhi::BlendMode::Additive,
                                                      rhi::BlendMode::PremultipliedAlpha};
        int bi = 3;
        for (int i = 0; i < 4; ++i) if (kBlendValues[i] == e.blend) { bi = i; break; }
        if (ImGui::Combo("Blend", &bi, kBlendNames, 4)) setBlend(kBlendValues[bi]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The particle shader always premultiplies rgb by alpha, so only Additive and\nPremultiplied alpha compose correctly against it.");

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
        {
            static char texPath[260] = {};
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::InputText("Texture path (content-relative)", texPath, sizeof texPath, ImGuiInputTextFlags_EnterReturnsTrue) && texPath[0]) {
                setTextureId(particles::effectId(texPath));
                syncEditBuffers();
            }
            ImGui::TextDisabled("An effect without a texture draws a soft dot. The id is fnv1a64 of the content path.");
        }

        // SubUV
        {
            int cols = e.subuvCols, rows = e.subuvRows, frames = e.subuvFrames;
            f32 fps = e.subuvFps;
            bool blend = e.subuvBlend, c = false;
            c |= ImGui::DragInt("SubUV columns", &cols, 0.2f, 1, 64);
            c |= ImGui::DragInt("SubUV rows", &rows, 0.2f, 1, 64);
            c |= ImGui::DragInt("SubUV frames", &frames, 0.2f, 1, 4096);
            c |= ImGui::DragFloat("SubUV fps (0 = once over life)", &fps, 0.5f, 0.0f, 240.0f, "%.1f");
            c |= ImGui::Checkbox("SubUV blend frames", &blend);
            if (c) editEffect([&](ParticleEffect& x) {
                x.subuvCols = static_cast<u16>(std::clamp(cols, 1, 64));
                x.subuvRows = static_cast<u16>(std::clamp(rows, 1, 64));
                x.subuvFrames = static_cast<u16>(std::clamp(frames, 1, std::min(4096, static_cast<int>(x.subuvCols) * static_cast<int>(x.subuvRows))));
                x.subuvFps = std::max(0.0f, fps);
                x.subuvBlend = blend;
                return true;
            }, idOf("SubUV"), frame);
        }

        // Size and colour over life
        f32 s0 = e.sizeStart, s1 = e.sizeEnd;
        bool sc = false;
        sc |= ImGui::DragFloat("Size at birth (cm)", &s0, 0.2f, 0.0f, 100000.0f, "%.2f");
        sc |= ImGui::DragFloat("Size at death (cm)", &s1, 0.2f, 0.0f, 100000.0f, "%.2f");
        if (sc) setSize(s0, s1);
        f32 cs[4], ce[4];
        for (int i = 0; i < 4; ++i) { cs[i] = e.colorStart[i]; ce[i] = e.colorEnd[i]; }
        if (ImGui::ColorEdit4("Colour at birth", cs)) setColorStart(cs);
        if (ImGui::ColorEdit4("Colour at death", ce)) setColorEnd(ce);

        // Curves
        const auto curveUi = [&](const char* title, const Curve4& curve, bool isColor) {
            ImGui::PushID(title);
            bool use = curve.n >= 2;
            if (ImGui::Checkbox(title, &use)) {
                editEffect([&](ParticleEffect& x) {
                    Curve4& c = isColor ? x.colorCurve : x.sizeCurve;
                    if (!use) { pxmodel::clearCurve(c); return true; }
                    const f32 a[4] = {isColor ? x.colorStart[0] : x.sizeStart, isColor ? x.colorStart[1] : 0.0f, isColor ? x.colorStart[2] : 0.0f, isColor ? x.colorStart[3] : 0.0f};
                    const f32 b[4] = {isColor ? x.colorEnd[0] : x.sizeEnd, isColor ? x.colorEnd[1] : 0.0f, isColor ? x.colorEnd[2] : 0.0f, isColor ? x.colorEnd[3] : 0.0f};
                    c = Curve4{};
                    c.n = 2; c.t[0] = 0.0f; c.t[1] = 1.0f;
                    for (int k = 0; k < 4; ++k) { c.v[0][k] = a[k]; c.v[1][k] = b[k]; }
                    return true;
                });
            }
            if (curve.n >= 2) {
                for (u32 i = 0; i < curve.n; ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    f32 t = curve.t[i], v[4] = {curve.v[i][0], curve.v[i][1], curve.v[i][2], curve.v[i][3]};
                    bool ch = false;
                    ImGui::SetNextItemWidth(70.0f);
                    ch |= ImGui::DragFloat("t", &t, 0.005f, 0.0f, 1.0f, "%.3f");
                    ImGui::SameLine();
                    if (isColor) ch |= ImGui::ColorEdit4("##c", v, ImGuiColorEditFlags_NoInputs);
                    else { ImGui::SetNextItemWidth(90.0f); ch |= ImGui::DragFloat("##v", &v[0], 0.1f, 0.0f, 100000.0f, "%.2f"); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_CLOSE))
                        editEffect([&](ParticleEffect& x) { return pxmodel::removeCurvePoint(isColor ? x.colorCurve : x.sizeCurve, static_cast<int>(i)); });
                    else if (ch)
                        editEffect([&](ParticleEffect& x) { return pxmodel::setCurvePoint(isColor ? x.colorCurve : x.sizeCurve, static_cast<int>(i), t, v); },
                                   idOf("pt") + static_cast<u32>(i), frame);
                    ImGui::PopID();
                }
                if (curve.n < 4 && ImGui::SmallButton(ICON_ADD " stop")) {
                    editEffect([&](ParticleEffect& x) {
                        Curve4& c = isColor ? x.colorCurve : x.sizeCurve;
                        // Insert in the widest gap, with the interpolated value there.
                        u32 at = 0;
                        f32 widest = -1.0f;
                        for (u32 k = 0; k + 1 < c.n; ++k) if (c.t[k + 1] - c.t[k] > widest) { widest = c.t[k + 1] - c.t[k]; at = k; }
                        const f32 t = 0.5f * (c.t[at] + c.t[at + 1]);
                        f32 v[4];
                        evalCurve(c, t, v);
                        return pxmodel::addCurvePoint(c, t, v);
                    });
                }
            }
            ImGui::PopID();
        };
        curveUi("Size curve (overrides start / end)", e.sizeCurve, false);
        curveUi("Colour curve (overrides start / end)", e.colorCurve, true);
    }

    // ---- lighting and fog ----
    ImGui::SeparatorText("Lighting and fog");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        static const char* kLight[] = {"Unlit", "Ambient (GI term)", "Lit (sun, sky, nearby lights)"};
        const Lighting eff = effectiveLighting(e);
        int li = static_cast<int>(eff);
        if (ImGui::Combo("Lighting", &li, kLight, 3))
            editEffect([&](ParticleEffect& x) {
                x.lighting = static_cast<Lighting>(li);
                x.receivesGI = li != 0;
                return true;
            });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unlit: the authored colour as is (an ember, a spark).\n"
                              "Ambient: modulated by the Voxi GI volume where one is installed.\n"
                              "Lit: sun (shadowed when the GI seam is present), sky, and up to 4 nearby scene lights, unshadowed.\n"
                              "--no-particle-gi leaves Lit with an unshadowed sun.");
        bool fog = e.fog;
        if (ImGui::Checkbox("Fog and aerial haze", &fog)) editEffect([&](ParticleEffect& x) { x.fog = fog; return true; });
    }

    // ---- sorting ----
    ImGui::SeparatorText("Sorting");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        static const char* kSort[] = {"Auto (depth for alpha blends)", "None", "Depth"};
        int so = static_cast<int>(e.sort);
        if (ImGui::Combo("Sort particles", &so, kSort, 3)) editEffect([&](ParticleEffect& x) { x.sort = static_cast<SortMode>(so); return true; });
        ImGui::TextDisabled("Above 65,536 particles a GPU emitter is not sorted; use Additive for dense clouds.");
    }

    // ---- events ----
    ImGui::SeparatorText("Events");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        for (usize k = 0; k < e.events.size(); ++k) {
            const EventSpawn& ev = e.events[k];
            const std::string child = ev.emitter < d.emitters.size() ? d.emitters[ev.emitter].name : std::string("?");
            ImGui::PushID(static_cast<int>(k));
            ImGui::Text("on %s: spawn %u into %s%s", ev.on == 0 ? "death" : "collide", ev.count, child.c_str(),
                        ev.inheritVelocity != 0.0f ? (" (inherit " + std::to_string(ev.inheritVelocity).substr(0, 4) + ")").c_str() : "");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_CLOSE)) { editSystem([&](ParticleSystemDef& s) { return pxmodel::removeEvent(s, sel_, static_cast<int>(k)); }); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        static int evOn = 0, evChild = 0, evCount = 3;
        static f32 evInherit = 0.0f;
        static const char* kOn[] = {"death", "collide"};
        ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("##evon", &evOn, kOn, 2);
        ImGui::SameLine();
        std::vector<const char*> names;
        for (const ParticleEffect& x : d.emitters) names.push_back(x.name.c_str());
        evChild = std::clamp(evChild, 0, static_cast<int>(names.size()) - 1);
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("##evchild", &evChild, names.data(), static_cast<int>(names.size()));
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragInt("count##ev", &evCount, 0.2f, 1, 1000);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("inherit##ev", &evInherit, 0.01f, 0.0f, 4.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button(ICON_ADD " Event"))
            editSystem([&](ParticleSystemDef& s) { return pxmodel::addEvent(s, sel_, static_cast<u8>(evOn), evChild, static_cast<u32>(evCount), evInherit); });
        ImGui::TextDisabled("A child needs a name without spaces; a rule that would form a loop is refused. Children spawn one tick later.");
    }

    // ---- culling and bounds ----
    ImGui::SeparatorText("Culling and bounds");
    {
        const ParticleEffect& e = d.emitters[static_cast<usize>(sel_)];
        f32 br = e.boundsRadiusCm, cull = e.cullDistanceCm;
        bool c = false;
        c |= ImGui::DragFloat("Bounds radius (cm, 0 = never culled)", &br, 1.0f, 0.0f, 1000000.0f, "%.1f");
        if (c) editEffect([&](ParticleEffect& x) { x.boundsRadiusCm = std::max(0.0f, br); return true; }, idOf("Bounds"), frame);
        ImGui::SameLine();
        if (ImGui::Button("Compute bounds")) computeBounds();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs this emitter for 3 s of simulated time and fits a sphere around it (x1.2).");
        if (ImGui::DragFloat("Cull distance (cm, 0 = never)", &cull, 10.0f, 0.0f, 10000000.0f, "%.0f"))
            editEffect([&](ParticleEffect& x) { x.cullDistanceCm = std::max(0.0f, cull); return true; }, idOf("Cull"), frame);
        bool sat = e.spawnAtTarget;
        if (ImGui::Checkbox("Takes part in spawn-at requests", &sat)) editEffect([&](ParticleEffect& x) { x.spawnAtTarget = sat; return true; });
    }

    // ---- user parameters ----
    ImGui::SeparatorText("User parameters");
    {
        for (usize i = 0; i < d.params.size(); ++i) {
            const UserParam& p = d.params[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s  (%s)", p.name.c_str(), kindWord(p.kind));
            ImGui::SameLine();
            f32 v[4] = {p.v[0], p.v[1], p.v[2], p.v[3]};
            bool ch = false;
            ImGui::SetNextItemWidth(220.0f);
            if (p.kind == 0) ch = ImGui::DragFloat("##v", &v[0], 0.05f);
            else if (p.kind == 1) ch = ImGui::DragFloat3("##v", v, 0.05f);
            else ch = ImGui::ColorEdit4("##v", v);
            if (ch) editSystem([&](ParticleSystemDef& s) { return pxmodel::setUserParamValue(s, static_cast<int>(i), v); }, idOf("pv") + static_cast<u32>(i), frame);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_CLOSE)) { editSystem([&](ParticleSystemDef& s) { return pxmodel::removeUserParam(s, static_cast<int>(i)); }); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        static const char* kKinds[] = {"float", "vec3", "color"};
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("##newparam", paramNameBuf_, sizeof paramNameBuf_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Combo("##newkind", &newParamKind_, kKinds, 3);
        ImGui::SameLine();
        if (ImGui::Button(ICON_ADD " Parameter")) {
            editSystem([&](ParticleSystemDef& s) { return pxmodel::addUserParam(s, paramNameBuf_, static_cast<u8>(newParamKind_)) >= 0; });
            paramNameBuf_[0] = '\0';
        }
        ImGui::TextDisabled("Bind a module parameter to one with the @ button; script it with Entity.SetParticleFloat / AN_SetParticleFloat.");
    }

    if (!loadError_.empty()) {
        ImGui::SeparatorText("Load error");
        ImGui::TextWrapped(ICON_WARNING " %s", loadError_.c_str());
    }
}

// ================================================================================== the preview ===

// Draws the live preview into the SAME shared ActorPreview surface Mesh/Actor/Anim/Graph tabs use (sharedPreview(),
// ActorEditor.hpp). The particles are drawn by a private CPU-tier ParticleRenderer through ActorPreview's transparent hook, fed
// from this tab's own EmitterSims -- so blending, textures, SubUV, ribbons and mesh particles look as they do in the game. When
// the renderer cannot be built the old cube preview is drawn instead.
//
// VISUAL-ONLY: there is no automated way to check that a rendered image "looks right", and this comment says so instead of
// implying otherwise. The preview target is display-referred (the preview's own shader gamma-encodes), so particle colours read
// a little darker here than in the scene.
void ParticleEditor::drawPreviewPane(Engine& e) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview) {
        ImGui::TextDisabled("No preview on this backend.");
        return;
    }
    if (!gpu_) {
        gpu_ = std::make_shared<PxGpu>();
        detail::registerParticleEditorGpu(gpu_);
    }
    PxGpu& g = *static_cast<PxGpu*>(gpu_.get());
    if (!g.tried) {
        g.tried = true;
        if (rhi::IDevice* dev = e.device()) {
            g.renderer.setGpuEnabled(false);   // the preview is a CPU-tier renderer: same simulation, no pool
            g.ready = g.renderer.init(*dev);
            if (g.ready) g.renderer.onRenderTargetsChanged(1, preview->colorFormat(), preview->depthFormat(), preview->width(), preview->height());
        }
    }

    if (rhi::IDevice* dev = e.device()) {
        const usize n = def_.emitters.size();
        if (g.ready) {
            g.views.clear();
            for (usize i = 0; i < n && i < sims_.size(); ++i) {
                if ((solo_ >= 0 && static_cast<int>(i) != solo_) || muted(static_cast<int>(i))) continue;
                particles::InstanceView v;
                v.emitterIndex = static_cast<u32>(i);
                v.effect = &def_.emitters[i];
                v.center = Vec3{0, 0, 0};
                v.particles = &sims_[i].particles();
                v.trail = &sims_[i].trail();
                v.trailRing = sims_[i].trailRing();
                v.linked = sims_[i].linked();
                v.aliveHint = sims_[i].aliveCount();
                g.views.push_back(v);
            }
            g.stampFrame = ImGui::GetFrameCount();
            std::shared_ptr<PxGpu> st = std::static_pointer_cast<PxGpu>(gpu_);
            preview->setDrawList({});
            preview->setTransparentHook([st, preview](rhi::IRenderContext& ctx) {
                if (!st->ready || st->stampFrame != ImGui::GetFrameCount() || st->views.empty()) return;
                particles::ParticleRenderer::Camera cam;
                preview->cameraMatrices(cam.viewProj, cam.invViewProjRel, cam.pos);
                st->renderer.drawViews(ctx, st->views, cam, nullptr);
            });
        } else {
            // The fallback: every live particle as a small cube through ActorPreview's opaque pipeline.
            f32 cubeRadius = 1.0f;
            const rhi::MeshHandle cube = sharedPreviewMeshes().resolve(*dev, "Meshes/cube.ocmesh", &cubeRadius);
            std::vector<render::preview::PreviewDraw> draws;
            if (cube) {
                for (usize i = 0; i < n && i < sims_.size(); ++i) {
                    const particles::ParticleEffect& fx = def_.emitters[i];
                    for (const particles::Particle& p : sims_[i].particles()) {
                        if (!particles::particleAlive(p)) continue;
                        const f32 t = p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 1.0f;
                        const f32 s = std::max(0.0f, particles::evalSize(fx, t)) * 0.5f * p.sizeScale;
                        render::preview::PreviewDraw d;
                        d.mesh = cube;
                        d.world[0] = s;  d.world[5] = s;  d.world[10] = s;
                        d.world[12] = p.position.x; d.world[13] = p.position.y; d.world[14] = p.position.z;
                        f32 col[4];
                        particles::evalColor(fx, t, col);
                        for (int c = 0; c < 4; ++c) d.baseColor[c] = col[c];
                        d.boundsRadius = cubeRadius;
                        d.roughness = 0.85f;
                        draws.push_back(d);
                    }
                }
            }
            preview->setDrawList(std::move(draws));
            preview->setTransparentHook(nullptr);
        }

        if (!previewFramed_) {
            // A FIXED estimate, set ONCE: the draw list is a moving target (particles spawn and die every tick), and re-framing
            // off it every frame would fight the author's own orbit / zoom instead of framing once like every other tab does.
            f32 reach = 50.0f;
            for (const particles::ParticleEffect& fx : def_.emitters)
                reach = std::max({reach, fx.shapeSize.size(), fx.speedMax * fx.lifetimeMax * 0.6f, fx.boundsRadiusCm});
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
    // DEBOUNCED: resize() waits for the GPU to go idle, so resizing on every frame of a drag would stall the editor.
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

// ================================================================================== the tab =======

void ParticleEditor::draw(Engine& e) {
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    // THE CLOCK RUNS OFF ImGui's, not the engine's frame delta -- Sound/AnimEditor's own reason: this tab only draws while it is
    // the active dock tab, so an engine-dt clock would keep accumulating while it was hidden and the preview would jump.
    const f64 now = ImGui::GetTime();
    const f32 dt = previewLastClock_ > 0.0 ? static_cast<f32>(now - previewLastClock_) : 0.0f;
    previewLastClock_ = now;
    if (previewPlaying_) tickPreview(std::min(dt, 0.1f));

    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[ParticleEditor] save failed for '{}': {}", path_, why);
    }
    // Ctrl+Z / Ctrl+Y reach the same undo()/redo() the buttons below call, skipped while an InputText has focus (it has its own).
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
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
    if (ImGui::Button(previewPlaying_ ? ICON_PAUSE " Pause preview" : ICON_PLAY " Play preview")) previewPlaying_ = !previewPlaying_;
    ImGui::SameLine();
    if (ImGui::Button(ICON_REFRESH " Restart preview")) restartPreview();
    ImGui::SameLine();
    if (ImGui::Button("Compute bounds")) computeBounds();
    ImGui::SameLine();
    ImGui::TextDisabled("%u live particle(s) in this preview", previewTotal());

    ImGui::Separator();

    // Draggable, persisted, through the shared SplitPane helper (EditorWidgets.hpp).
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 minPreview = 180.0f * dpi, minParams = 220.0f * dpi;
    const f32 leftW = splitPaneWidth(split_, kPrefPreviewSplit, kDefaultPreviewFraction, avail, minPreview, minParams);
    const f32 h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##pxpreview", ImVec2(leftW, h), true)) drawPreviewPane(e);
    ImGui::EndChild();
    drawSplitHandle(split_, "##pxsplit", kPrefPreviewSplit, avail, minPreview, minParams, 6.0f * dpi);
    if (ImGui::BeginChild("##pxparams", ImVec2(0, h), true)) drawParams();
    ImGui::EndChild();
}

// Restores the preview/params split to its default proportion and persists that immediately -- see AssetEditor.hpp's
// resetLayout() comment for why "Reset Tab Layout" needs every tab to implement this.
void ParticleEditor::resetLayout() { resetSplitPane(split_, kPrefPreviewSplit, kDefaultPreviewFraction); }

} // namespace aver::editor
#endif  // AVER_MODULE_PARTICLES && AVER_WITH_IMGUI
