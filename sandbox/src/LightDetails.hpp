#pragma once
// The Details panel's Light section: type, colour, intensity, range, cone, rectangle size, emitter
// radius, shadows, and the IES profile / cookie texture pickers. Header-only so the pure half (asset
// listing, id derivation, cone conversions, defaults) is testable without a window; the ImGui half
// follows the guard. docs/rendering/LIGHTS.md.
#include "aver/core/Hash.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

// One pickable file: content-relative path (forward slashes) and the ObjectId a CLight stores for it.
struct LightAssetChoice {
    std::string label;
    u64 id = 0;
};

// The id a CLight stores for a content-relative path: fnv1a64 of the forward-slash path, the same id
// space CMeshRenderer::mesh and CParticleEmitter::effect use.
inline u64 lightAssetId(std::string_view contentRelativePath) {
    std::string p(contentRelativePath);
    std::replace(p.begin(), p.end(), '\\', '/');
    return fnv1a64(p);
}

inline bool lightHasExt(const std::filesystem::path& p, bool ies) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ies) return e == ".ies";
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

// Every .ies (or cookie-capable image) under `contentDir`, sorted by path. Capped so a huge texture
// library cannot stall the editor; `truncated` says when the cap bit.
inline std::vector<LightAssetChoice> listLightAssets(const std::string& contentDir, bool ies,
                                                     bool* truncated = nullptr) {
    constexpr usize kCap = 4000;
    std::vector<LightAssetChoice> out;
    if (truncated) *truncated = false;
    std::error_code ec;
    if (contentDir.empty() || !std::filesystem::is_directory(contentDir, ec)) return out;
    for (std::filesystem::recursive_directory_iterator it(contentDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !lightHasExt(it->path(), ies)) continue;
        if (out.size() >= kCap) { if (truncated) *truncated = true; break; }
        std::string rel = std::filesystem::relative(it->path(), contentDir, ec).generic_string();
        if (ec || rel.empty()) continue;
        out.push_back({rel, lightAssetId(rel)});
    }
    std::sort(out.begin(), out.end(), [](const LightAssetChoice& a, const LightAssetChoice& b) { return a.label < b.label; });
    return out;
}

// Cone half-angles are stored as cosines; the panel edits degrees.
inline f32 lightConeDegrees(f32 cosine) {
    return std::acos(std::min(1.0f, std::max(-1.0f, cosine))) * kRadToDeg;
}
inline f32 lightConeCos(f32 degrees) {
    return std::cos(std::min(89.9f, std::max(0.0f, degrees)) * kDegToRad);
}

// A new light of `kind` with values that are sane for a room, not the sun-sized struct defaults.
// (addComponent zero-fills, so every field a light needs is written.)
inline scene::CLight makeNewLight(i32 kind) {
    scene::CLight c{};
    c.kind = kind;
    c.colour[0] = c.colour[1] = c.colour[2] = 1.0f;
    c.rangeCm = 0.0f;
    c.sourceRadiusCm = 2.0f;
    switch (kind) {
        case scene::kLightSpot:
            c.intensityLux = 5000.0f;
            c.innerCos = lightConeCos(15.0f);
            c.outerCos = lightConeCos(35.0f);
            break;
        case scene::kLightRect:
            c.intensityLux = 3000.0f;
            c.widthCm = 120.0f;
            c.heightCm = 60.0f;
            c.outerCos = lightConeCos(60.0f);
            break;
        default:
            c.intensityLux = 1500.0f;
            c.innerCos = 1.0f;
            c.outerCos = lightConeCos(60.0f);
            break;
    }
    return c;
}

#if AVER_WITH_IMGUI
// Per-picker cache: scanned on first use and whenever the project changes, rescanned on demand.
struct LightAssetPicker {
    std::string scannedDir;
    std::vector<LightAssetChoice> list;
    bool truncated = false;
    char filter[64] = {};

    void ensure(const std::string& contentDir, bool ies, bool force = false) {
        if (force || scannedDir != contentDir) {
            list = listLightAssets(contentDir, ies, &truncated);
            scannedDir = contentDir;
        }
    }
    const char* labelOf(u64 id) const {
        for (const LightAssetChoice& c : list) if (c.id == id) return c.label.c_str();
        return nullptr;
    }
};

// A path field with Browse / Clear. Returns true when `id` changed.
inline bool lightAssetField(const char* label, LightAssetPicker& pk, const std::string& contentDir,
                            bool ies, i64& id) {
    bool changed = false;
    pk.ensure(contentDir, ies);
    ImGui::PushID(label);
    const char* name = id ? pk.labelOf(static_cast<u64>(id)) : nullptr;
    char buf[160];
    if (!id)        std::snprintf(buf, sizeof buf, "(none)");
    else if (name)  std::snprintf(buf, sizeof buf, "%s", name);
    else            std::snprintf(buf, sizeof buf, "id 0x%llx (not found in this project)", static_cast<unsigned long long>(id));
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", buf);
    if (ImGui::Button("Browse...")) {
        pk.ensure(contentDir, ies, true);
        pk.filter[0] = 0;
        ImGui::OpenPopup("##pick");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(id == 0);
    if (ImGui::Button("Clear")) { id = 0; changed = true; }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##pick")) {
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputTextWithHint("##filter", ies ? "Search .ies profiles..." : "Search textures...", pk.filter, sizeof pk.filter);
        ImGui::Separator();
        std::string needle = pk.filter;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        usize matched = 0, drawn = 0;
        for (const LightAssetChoice& c : pk.list) {
            if (!needle.empty()) {
                std::string l = c.label;
                std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (l.find(needle) == std::string::npos) continue;
            }
            ++matched;
            if (drawn >= 40) continue;
            ++drawn;
            if (ImGui::Selectable(c.label.c_str(), static_cast<u64>(id) == c.id)) {
                id = static_cast<i64>(c.id);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (matched == 0)
            ImGui::TextDisabled("%s", pk.list.empty() ? (ies ? "no .ies files in this project's content" : "no images in this project's content")
                                                      : "nothing matches");
        else if (matched > drawn)
            ImGui::TextDisabled("...and %zu more; type to narrow", matched - drawn);
        if (pk.truncated) ImGui::TextDisabled("(listing capped; narrow the content folder)");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Draws the Light section body. Returns true when any field changed (the caller marks the level
// unsaved). `contentDir` is the project's absolute content folder.
inline bool drawLightDetails(scene::CLight& c, const std::string& contentDir) {
    static LightAssetPicker iesPicker, cookiePicker;
    bool changed = false;

    if (c.kind == scene::kLightDirectional) {
        ImGui::TextDisabled("Directional: the Sun and Sky entries own this; the lamp renderer ignores it.");
        return false;
    }

    int type = c.kind == scene::kLightSpot ? 1 : (c.kind == scene::kLightRect ? 2 : 0);
    const char* types[] = {"Point", "Spot", "Rectangle"};
    if (ImGui::Combo("Type", &type, types, 3)) {
        const i32 was = c.kind;
        c.kind = type == 1 ? scene::kLightSpot : (type == 2 ? scene::kLightRect : scene::kLightPoint);
        // Switching to a shape whose size was never set: give it the new-light value rather than 0.
        if (c.kind == scene::kLightRect && c.widthCm <= 0.0f) { c.widthCm = 120.0f; c.heightCm = 60.0f; }
        if (c.kind == scene::kLightSpot && was != scene::kLightSpot && c.outerCos >= c.innerCos) {
            c.innerCos = lightConeCos(15.0f);
            c.outerCos = lightConeCos(35.0f);
        }
        changed = true;
    }
    changed |= ImGui::ColorEdit3("Colour", c.colour);

    changed |= ImGui::DragFloat("Intensity (cd)", &c.intensityLux, 10.0f, 0.0f, 1.0e7f, "%.0f",
                                ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Candela: lux at 1 m on axis. A point light's total output is 4*pi times this;\n"
                          "a rectangle's is its intensity along the panel normal (radiance = cd / area).\n"
                          "With an IES profile this scales the profile; see 'Intensity = IES peak'.");
    changed |= ImGui::DragFloat("Range (cm)", &c.rangeCm, 10.0f, 0.0f, 100000.0f, c.rangeCm > 0.0f ? "%.0f" : "auto");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the light fades to nothing. 0 derives it from the intensity (capped at 50 m).");

    const bool noShadows = (c.flags & scene::kLightNoShadows) != 0;
    bool shadows = !noShadows;
    if (ImGui::Checkbox("Cast shadows", &shadows)) {
        c.flags = shadows ? (c.flags & ~scene::kLightNoShadows) : (c.flags | scene::kLightNoShadows);
        changed = true;
    }

    if (c.kind == scene::kLightRect) {
        f32 w = c.widthCm > 0.0f ? c.widthCm : 100.0f;
        f32 h = c.heightCm > 0.0f ? c.heightCm : 100.0f;
        if (ImGui::DragFloat("Width (cm)", &w, 1.0f, 1.0f, 5000.0f, "%.0f")) { c.widthCm = w; changed = true; }
        if (ImGui::DragFloat("Height (cm)", &h, 1.0f, 1.0f, 5000.0f, "%.0f")) { c.heightCm = h; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The panel emits from its +X face, spanning local Y (width) and Z (height).\n"
                              "Shadows get a penumbra shaped like the panel.");
    } else {
        f32 r = c.sourceRadiusCm > 0.0f ? c.sourceRadiusCm : 1.0f;
        if (ImGui::DragFloat("Source radius (cm)", &r, 0.1f, 0.1f, 200.0f, "%.1f")) { c.sourceRadiusCm = r; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Size of the bulb: larger means softer shadow edges.");
    }

    if (c.kind == scene::kLightSpot) {
        f32 inner = lightConeDegrees(c.innerCos), outer = lightConeDegrees(c.outerCos);
        if (ImGui::DragFloat("Inner cone (deg)", &inner, 0.5f, 0.0f, 89.0f, "%.1f")) {
            inner = std::min(inner, outer);
            c.innerCos = lightConeCos(inner);
            changed = true;
        }
        if (ImGui::DragFloat("Outer cone (deg)", &outer, 0.5f, 0.0f, 89.0f, "%.1f")) {
            outer = std::max(outer, inner);
            c.outerCos = lightConeCos(outer);
            changed = true;
        }
    }

    ImGui::SeparatorText("Projection");
    changed |= lightAssetField("IES profile", iesPicker, contentDir, true, c.iesProfile);
    if (c.iesProfile) {
        bool peak = (c.flags & scene::kLightIesPeak) != 0;
        if (ImGui::Checkbox("Intensity = IES peak", &peak)) {
            c.flags = peak ? (c.flags | scene::kLightIesPeak) : (c.flags & ~scene::kLightIesPeak);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off (default): the intensity is the profile's average, so the light keeps its\n"
                              "luminous flux whatever the profile's shape. On: the profile's brightest\n"
                              "direction gets exactly this intensity.\n"
                              "The profile's 0 degree axis is the entity's +X; C0 is its +Y.");
    }
    changed |= lightAssetField("Cookie", cookiePicker, contentDir, false, c.cookie);
    if (c.cookie && c.kind != scene::kLightSpot) {
        f32 outer = lightConeDegrees(c.outerCos);
        if (ImGui::DragFloat("Cookie angle (deg)", &outer, 0.5f, 5.0f, 89.0f, "%.1f")) {
            c.outerCos = lightConeCos(outer);
            changed = true;
        }
    }
    return changed;
}
#endif

} // namespace aver::editor
