// Physics viewport overlay (Show > Physics). See the header for the wiring.
#include "PhysicsDebugOverlay.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_debug_abi.h"
#endif

#if AVER_WITH_IMGUI
#  include "AiDebugOverlay.hpp"   // aiDebugProject
#  include "imgui.h"
#endif

#include <algorithm>

namespace aver::editor {

#if AVER_MODULE_PHYSICS

static_assert(sizeof(rhi::LineVertex) == 6 * sizeof(f32), "aver_phys_debug_lines writes two rhi::LineVertex per line");

u32 physicsDebugFlags(const PhysicsDebugOptions& o) {
    u32 f = 0;
    if (o.bodies)          f |= AVER_PHYS_DBG_BODIES;
    if (o.bounds)          f |= AVER_PHYS_DBG_BOUNDS;
    if (o.centreOfMass)    f |= AVER_PHYS_DBG_COM;
    if (o.velocity)        f |= AVER_PHYS_DBG_VELOCITY;
    if (o.sleep)           f |= AVER_PHYS_DBG_SLEEP;
    if (o.mass)            f |= AVER_PHYS_DBG_MASS;
    if (o.constraints)     f |= AVER_PHYS_DBG_CONSTRAINTS;
    if (o.limits)          f |= AVER_PHYS_DBG_LIMITS;
    if (o.contacts)        f |= AVER_PHYS_DBG_CONTACTS;
    if (o.supportFaces)    f |= AVER_PHYS_DBG_SUPPORT_FACES;
    if (o.softVertices)    f |= AVER_PHYS_DBG_SOFT_VERTICES;
    if (o.softEdges)       f |= AVER_PHYS_DBG_SOFT_EDGES;
    if (o.characters)      f |= AVER_PHYS_DBG_CHARACTERS;
    if (o.staticTriangles) f |= AVER_PHYS_DBG_STATIC_TRIANGLES;
    if (o.linearCast)      f |= AVER_PHYS_DBG_LINEAR_CAST;
    if (o.labels)          f |= AVER_PHYS_DBG_LABELS;
    return f;
}

void physicsDebugCollect(const PhysicsDebugOptions& opts, const Vec3& eye, PhysicsDebugFrame& out, const PhysicsDebugSource& src) {
    out.clear();
    const u32 flags = physicsDebugFlags(opts);
    // Every frame the overlay is on: contacts, supporting faces and continuous-collision markers are drawn inside the step.
    // arm(0) with nothing armed is a no-op in the module, so the overlay being off costs one call.
    src.arm(flags);
    if (flags == 0) return;
    src.setScheme(opts.colourScheme);

    const f32 camera[3] = {eye.x, eye.y, eye.z};
    const i32 budget = std::max(opts.maxLines, 1);
    const i32 n = src.collect(flags, camera, opts.radiusCm, budget);
    out.truncated = src.truncated() != 0;
    if (n > 0) {
        out.lines.resize(static_cast<size_t>(n) * 2);
        const i32 got = src.lines(reinterpret_cast<f32*>(out.lines.data()), n);
        out.lines.resize(static_cast<size_t>(std::max(got, 0)) * 2);
    }
    const i32 labels = src.labelCount();
    for (i32 i = 0; i < labels; ++i) {
        f32 xyz[3] = {}, rgb[3] = {};
        u8 text[128] = {};
        if (!src.labelGet(i, xyz, rgb, text, static_cast<i32>(sizeof text))) break;
        PhysicsDebugLabel l;
        l.world = Vec3{xyz[0], xyz[1], xyz[2]};
        l.text = reinterpret_cast<const char*>(text);
        const auto b = [](f32 c) { return static_cast<u32>(std::min(std::max(c, 0.0f), 1.0f) * 255.0f + 0.5f); };
        l.rgba = 0xFF000000u | (b(rgb[0]) << 16) | (b(rgb[1]) << 8) | b(rgb[2]);
        out.labels.push_back(std::move(l));
    }
}

const PhysicsDebugSource& physicsDebugModuleSource() {
    static const PhysicsDebugSource s = {
        &aver_phys_debug_set_color_scheme, &aver_phys_debug_arm,         &aver_phys_debug_collect,   &aver_phys_debug_truncated,
        &aver_phys_debug_lines,            &aver_phys_debug_label_count, &aver_phys_debug_label_get,
    };
    return s;
}

void physicsDebugDraw(rhi::IDevice* device, const PhysicsDebugOptions& opts, const Vec3& eye, PhysicsDebugFrame& frame,
                      rhi::LineHandle& mesh, bool chromeHidden) {
    if (mesh && device) { device->destroyLineMesh(mesh); mesh = 0; }
    physicsDebugCollect(opts, eye, frame, physicsDebugModuleSource());
    if (!device || chromeHidden || frame.lines.empty()) return;
    mesh = device->createLineMesh(frame.lines.data(), static_cast<u32>(frame.lines.size()));
    const Mat4 id = Mat4::identity();
    if (mesh) device->drawLines(mesh, &id.m[0][0]);
}

#endif // AVER_MODULE_PHYSICS

#if AVER_WITH_IMGUI

void physicsDebugDrawLabels(const PhysicsDebugFrame& frame, const Mat4& viewProj, f32 vpX, f32 vpY, f32 vpW, f32 vpH, const Vec3& eye, f32 maxDistCm) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const PhysicsDebugLabel& l : frame.labels) {
        if (dist(l.world, eye) > maxDistCm) continue;
        f32 x, y;
        if (!aiDebugProject(viewProj, l.world, vpX, vpY, vpW, vpH, x, y)) continue;
        const ImU32 col = IM_COL32((l.rgba >> 16) & 0xFF, (l.rgba >> 8) & 0xFF, l.rgba & 0xFF, (l.rgba >> 24) & 0xFF);
        dl->AddText(ImVec2(x, y), col, l.text.c_str());
    }
}

void drawPhysicsDebugMenu(PhysicsDebugOptions& o, const PhysicsDebugFrame& frame) {
    if (!ImGui::BeginMenu("Show Physics")) return;
#if AVER_MODULE_PHYSICS
    if (!aver_phys_debug_available()) {
        ImGui::TextDisabled("This build has no Jolt debug renderer (AVER_PHYSICS_DEBUG_RENDERER is off).");
        ImGui::EndMenu();
        return;
    }
#endif
    ImGui::MenuItem("Bodies", nullptr, &o.bodies);
    ImGui::MenuItem("Bounds", nullptr, &o.bounds);
    ImGui::MenuItem("Centre of mass", nullptr, &o.centreOfMass);
    ImGui::MenuItem("Velocity", nullptr, &o.velocity);
    ImGui::MenuItem("Sleep", nullptr, &o.sleep);
    ImGui::MenuItem("Mass", nullptr, &o.mass);
    ImGui::MenuItem("Constraints", nullptr, &o.constraints);
    ImGui::MenuItem("Limits", nullptr, &o.limits);
    ImGui::MenuItem("Contacts", nullptr, &o.contacts);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contact points and normals of the LAST fixed step only,\nhowever many steps the frame ran.");
    ImGui::MenuItem("Support faces", nullptr, &o.supportFaces);
    ImGui::MenuItem("Soft body vertices", nullptr, &o.softVertices);
    ImGui::MenuItem("Soft body constraints", nullptr, &o.softEdges);
    ImGui::MenuItem("Characters", nullptr, &o.characters);
    ImGui::MenuItem("Static triangles", nullptr, &o.staticTriangles);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mesh and heightfield triangles inside the radius only:\na city is never drawn whole.");
    ImGui::MenuItem("Continuous collision", nullptr, &o.linearCast);
    ImGui::MenuItem("Labels", nullptr, &o.labels);
    ImGui::Separator();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Colour by", &o.colourScheme, "Motion type\0Sleep\0Layer\0Shape type\0Island\0Instance\0");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Radius (cm)", &o.radiusCm, 500.0f, 50000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Line budget", &o.maxLines, 5000, 400000);
    if (frame.truncated)
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "Truncated: raise the line budget or lower the radius.");
    ImGui::EndMenu();
}

#endif // AVER_WITH_IMGUI

} // namespace aver::editor
