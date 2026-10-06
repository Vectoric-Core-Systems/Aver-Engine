#pragma once
// The Details panel's Decal section: size, tint, opacity, channels, normal/roughness controls, angle
// and distance fades, sort order and the three image pickers. Header-only so the pure half (new-decal
// defaults) is testable without a window; the ImGui half follows the guard. Reuses the light
// editor's image picker (LightDetails.hpp). docs/rendering/DECALS.md.
#include "LightDetails.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/DecalGather.hpp"

#include <algorithm>
#include <cmath>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

// A new decal with values that read well on a wall or floor, not the zero-filled struct. Every field
// is written, so what the author sees in the panel is what is stored.
inline scene::CDecal makeNewDecal() {
    scene::CDecal c{};
    c.sizeCm[0] = 60.0f;    // depth along the projection
    c.sizeCm[1] = 120.0f;   // width
    c.sizeCm[2] = 120.0f;   // height
    c.tint[0] = c.tint[1] = c.tint[2] = 1.0f;
    c.transparency = 0.0f;
    c.normalStrength = 1.0f;
    c.roughness = 0.0f;     // unset: an image-less decal leaves roughness alone
    c.edgeFade = scene::kDecalDefaultEdgeFade;
    c.angleFadeStartDeg = scene::kDecalDefaultAngleStartDeg;
    c.angleFadeEndDeg = scene::kDecalDefaultAngleEndDeg;
    c.uvScale[0] = c.uvScale[1] = 1.0f;
    return c;
}

#if AVER_WITH_IMGUI
// Draws the Decal section body. Returns true when any field changed (the caller marks the level
// unsaved). `contentDir` is the project's absolute content folder.
inline bool drawDecalDetails(scene::CDecal& c, const std::string& contentDir) {
    static LightAssetPicker basePicker, normalPicker, ormPicker;
    bool changed = false;

    if (c.flags & scene::kDecalPooled)
        ImGui::TextDisabled("Spawned by the decal pool at run time; not saved with the level.");

    bool enabled = (c.flags & scene::kDecalDisabled) == 0;
    if (ImGui::Checkbox("Enabled", &enabled)) {
        c.flags = enabled ? (c.flags & ~scene::kDecalDisabled) : (c.flags | scene::kDecalDisabled);
        changed = true;
    }

    ImGui::SeparatorText("Box");
    f32 size[3] = {scene::decalSizeCm(c, 0), scene::decalSizeCm(c, 1), scene::decalSizeCm(c, 2)};
    if (ImGui::DragFloat3("Size (cm)", size, 1.0f, 1.0f, 100000.0f, "%.0f")) {
        for (int i = 0; i < 3; ++i) c.sizeCm[i] = std::max(size[i], 1.0f);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth along the projection (X), width (Y), height (Z). The decal paints\n"
                          "every surface inside this box and projects along the entity's +X.\n"
                          "Drag the handles in the viewport, or scale the entity.");
    changed |= ImGui::DragInt("Sort order", &c.sortOrder, 0.2f, -1000, 1000);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher paints over lower. Equal orders paint the nearer decal on top.");

    ImGui::SeparatorText("Appearance");
    f32 opacity = 1.0f - std::min(std::max(c.transparency, 0.0f), 1.0f);
    if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f)) { c.transparency = 1.0f - opacity; changed = true; }
    f32 tint[3];
    scene::decalTint(c, tint);
    if (ImGui::ColorEdit3("Tint", tint)) { for (int i = 0; i < 3; ++i) c.tint[i] = std::max(tint[i], 0.001f); changed = true; }
    const bool colour = (c.flags & scene::kDecalNoColour) == 0;
    bool colourOn = colour;
    if (ImGui::Checkbox("Paint base colour", &colourOn)) {
        c.flags = colourOn ? (c.flags & ~scene::kDecalNoColour) : (c.flags | scene::kDecalNoColour);
        changed = true;
    }
    changed |= lightAssetField("Base colour", basePicker, contentDir, false, c.baseTexture);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("sRGB image; its alpha is the decal's coverage (which also gates normal and roughness).");

    ImGui::SeparatorText("Normal");
    bool normalOn = (c.flags & scene::kDecalNoNormal) == 0;
    if (ImGui::Checkbox("Paint normal", &normalOn)) {
        c.flags = normalOn ? (c.flags & ~scene::kDecalNoNormal) : (c.flags | scene::kDecalNoNormal);
        changed = true;
    }
    changed |= lightAssetField("Normal map", normalPicker, contentDir, false, c.normalTexture);
    f32 ns = scene::decalNormalStrength(c);
    if (ImGui::SliderFloat("Normal strength", &ns, 0.01f, 2.0f)) { c.normalStrength = ns; changed = true; }

    ImGui::SeparatorText("Roughness and metallic");
    bool roughOn = (c.flags & scene::kDecalNoRoughness) == 0;
    if (ImGui::Checkbox("Paint roughness / metallic", &roughOn)) {
        c.flags = roughOn ? (c.flags & ~scene::kDecalNoRoughness) : (c.flags | scene::kDecalNoRoughness);
        changed = true;
    }
    changed |= lightAssetField("Roughness / metallic map", ormPicker, contentDir, false, c.ormTexture);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Linear image: green = roughness, blue = metallic (the glTF packing).");
    changed |= ImGui::SliderFloat("Roughness", &c.roughness, 0.0f, 1.0f, c.roughness > 0.0f ? "%.2f" : "unset");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplies the map's green; with no map, the roughness itself. Unset leaves the\n"
                          "surface alone when there is no map.");
    changed |= ImGui::SliderFloat("Metallic", &c.metallic, 0.0f, 1.0f, c.metallic > 0.0f ? "%.2f" : "none");

    ImGui::SeparatorText("Fading");
    f32 edge = scene::decalEdgeFade(c);
    if (ImGui::SliderFloat("Edge fade", &edge, 0.01f, 1.0f)) { c.edgeFade = edge; changed = true; }
    f32 a0 = scene::decalAngleStartDeg(c), a1 = scene::decalAngleEndDeg(c);
    if (ImGui::DragFloatRange2("Angle fade (deg)", &a0, &a1, 0.5f, 1.0f, 90.0f, "full to %.0f", "gone at %.0f")) {
        c.angleFadeStartDeg = a0;
        c.angleFadeEndDeg = std::max(a1, a0 + 0.5f);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Angle between the surface normal and the way the decal looks back at you.\n"
                          "Surfaces tilted past the second value get nothing: stops stretching on walls\n"
                          "that run along the projection.");
    changed |= ImGui::DragFloat("Fade distance (cm)", &c.fadeDistanceCm, 10.0f, 0.0f, 1.0e6f,
                                c.fadeDistanceCm > 0.0f ? "%.0f" : "never");

    ImGui::SeparatorText("Image placement");
    f32 uvs[2] = {scene::decalUvScale(c, 0), scene::decalUvScale(c, 1)};
    if (ImGui::DragFloat2("UV scale", uvs, 0.01f, -64.0f, 64.0f)) { c.uvScale[0] = uvs[0]; c.uvScale[1] = uvs[1]; changed = true; }
    changed |= ImGui::DragFloat2("UV offset", c.uvOffset, 0.01f, -64.0f, 64.0f);
    return changed;
}
#endif

} // namespace aver::editor
