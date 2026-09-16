// Editor: the material and mode panels, the World Outliner, the Details panel and asset assignment.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_SCENE
// Starts an inline rename on a row, seeded with the name it already shows.
void SandboxApp::beginOutlinerRename(scene::Entity e) {
    outlinerRenaming_ = e;
    outlinerRenameFocus_ = true;
    const auto it = entityLabels_.find(static_cast<u32>(e));
    const std::string cur = it != entityLabels_.end() ? it->second
                                                      : scene::World::instance().name(e);
    std::snprintf(outlinerRenameBuf_, sizeof outlinerRenameBuf_, "%s", cur.c_str());
}

#endif

#if AVER_WITH_IMGUI
// True, with *picked written, when something is chosen. `current` only drives the check mark.
bool SandboxApp::assetPicker(const char* popupId, const std::vector<AssetChoice>& candidates,
                 u64 current, u64* picked) {
#if !AVER_WITH_IMGUI
    (void)popupId; (void)candidates; (void)current; (void)picked;
    return false;
#else
    bool chose = false;
    if (ImGui::BeginPopup(popupId)) {
        if (ImGui::IsWindowAppearing()) {
            assetPickerFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260.0f * dpi_);
        ImGui::InputTextWithHint("##assetPickerFilter", "Search assets...",
                                 assetPickerFilter_, sizeof assetPickerFilter_);
        ImGui::Separator();

        // NEVER A SILENT TRUNCATION, the same rule the node palette follows: a capped list that
        // simply stops reads as "there is no such asset", which is the wrong thing to learn from
        // a full box.
        constexpr usize kShown = 40;
        usize matched = 0, drawn = 0;
        for (const AssetChoice& c : candidates) {
            if (!containsNoCase(c.label, assetPickerFilter_)) continue;
            ++matched;
            if (drawn >= kShown) continue;
            ++drawn;
            if (ImGui::Selectable(c.label.c_str(), c.id == current)) {
                *picked = c.id;
                chose = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (matched == 0)
            ImGui::TextDisabled("%s", candidates.empty() ? "nothing loaded to pick from"
                                                         : "no asset matches");
        else if (matched > drawn)
            ImGui::TextDisabled("...and %zu more; type to narrow", matched - drawn);
        ImGui::EndPopup();
    }
    return chose;
#endif
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
// The particle path, EXTRACTED so the drag-drop target and the picker share one implementation.
// Writing the resolve/hash/assign/mark/log sequence a second time is exactly the "two
// implementations of one fact" shape this file keeps paying for elsewhere.
bool SandboxApp::assignParticleEffect(scene::Entity ent, const std::string& absPath) {
#if AVER_MODULE_PARTICLES
    scene::World& w = scene::World::instance();
    auto* pe = w.component<scene::CParticleEmitter>(ent, scene::kComponentParticleEmitter);
    if (!pe) return false;
    if (lowerExt(std::filesystem::path(absPath)) != ".ocparticle") {
        cbStatus_ = "Only a .ocparticle asset can be assigned to an emitter";
        return false;
    }
    const std::string content = project_.contentDir();
    std::error_code ec;
    std::string rel = content.empty() ? std::string()
                                      : std::filesystem::relative(absPath, content, ec).string();
    if (content.empty() || ec || rel.empty()) {
        cbStatus_ = "Could not resolve the effect to a project-relative path";
        return false;
    }
    for (char& ch : rel) if (ch == '\\') ch = '/';
    pe->effect = fnv1a64(std::string_view(rel));
    markLevelUnsaved();
    cbStatus_ = "Assigned " + std::filesystem::path(absPath).filename().string();
    AVER_INFO("[Particles] entity {} effect set to 0x{:016X} ('{}')", ent, pe->effect, rel);
    return true;
#else
    (void)ent; (void)absPath;
    return false;
#endif
}

// A mesh id IS fnv1a64 of the project-relative path, which is exactly what meshPathById_ is keyed
// on -- so a picked id needs no conversion, unlike the material below.
bool SandboxApp::assignMeshId(scene::Entity ent, u64 meshId) {
    scene::World& w = scene::World::instance();
    auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
    if (!mr) return false;
    mr->mesh = meshId;
    // WITHOUT THIS THE PICTURE DOES NOT CHANGE. The GPU path only re-uploads when dirty is set --
    // the same fix EditorEntitySnapshot records after restoring a mesh renderer, and the same one
    // a reassignment needs.
    mr->dirty = 1;
    markLevelUnsaved();
    const auto it = meshPathById_.find(meshId);
    cbStatus_ = "Assigned " + (it == meshPathById_.end()
                                   ? std::string("mesh")
                                   : std::filesystem::path(it->second).filename().string());
    AVER_INFO("[Editor] entity {} mesh set to 0x{:016X} ('{}')", ent, meshId,
              it == meshPathById_.end() ? std::string("?") : it->second);
    return true;
}

// MATERIAL IDENTITY IS AN INTERNED NAME TOKEN, not a path hash, and getting that wrong is silent:
// an fnv1a64 written into mr->material resolves to nothing, or by coincidence to an unrelated
// surface. content_'s surface materials are keyed on the token aver_scene_material() interns, so the picker
// carries tokens and this writes one straight through.
bool SandboxApp::assignMaterialToken(scene::Entity ent, i32 token) {
    scene::World& w = scene::World::instance();
    auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
    if (!mr) return false;
    mr->material = token;
    mr->dirty = 1;
    markLevelUnsaved();
    const char* nm = aver_scene_material_name(token);
    cbStatus_ = std::string("Assigned surface ") + (nm && *nm ? nm : "(unnamed)");
    AVER_INFO("[Editor] entity {} material set to token {} ('{}')", ent, token, nm ? nm : "");
    return true;
}

#endif
#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_PBR
std::string SandboxApp::saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err) {
    namespace fs = std::filesystem;
    const std::string content = project_.contentDir();
    if (content.empty()) { err = "no project"; return {}; }
    const fs::path dir = fs::path(content) / "Materials";
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        err = "no Content\\Materials directory -- materials are edited from their C# source "
              "there; Binaries\\Materials holds only avermatc's compiled output and has no "
              "source to edit";
        return {};
    }

    std::string firstError;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".cs") continue;
        const std::string path = it->path().string();

        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        std::string out, why;
        if (!fmt::rewriteMaterialScript(text, name, d, nullptr, out, &why)) {
            if (text.find("[AverMaterial(\"" + name + "\")]") != std::string::npos && firstError.empty())
                firstError = why;
            continue;
        }
        if (out == text) return path;

        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        if (!os) { err = "could not open " + path + " for writing"; return {}; }
        os.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!os) { err = "write failed"; return {}; }
        return path;
    }
    err = firstError.empty() ? ("no .cs under Content\\Materials declares '" + name + "'") : firstError;
    return {};
}

#endif
#endif

#if AVER_WITH_IMGUI
// Draws the material half of the Details panel, editing the shared MaterialDesc where there is
// one and the actor's own values otherwise.
// TAKES A HANDLE, not a MeshObj. This whole panel -- ~25 sliders, the IOR/F0 consistency
// check, the texture slots and Save to C# -- was reachable from exactly one call site: the
// editor's PLACEHOLDER objects. A real scene entity's Details offered Transform and Mesh
// and nothing else, which docs/EDITOR.md:276 states outright. The material was always one
// lookup away (content_'s surface materials map the token a CMeshRenderer carries to exactly this
// handle); only the signature stood in the way.
void SandboxApp::materialPanel(pbr::MaterialHandle handle) {
#if AVER_MODULE_PBR
    pbr::MaterialDesc* d = pbr::MaterialLibrary::get().mutableDesc(handle);
    if (!d) { ImGui::TextDisabled("No material (drawing with the fallback)"); return; }
    bool changed = false;

    // ---- ONE UNDO ENTRY PER INTERACTION, not per frame -------------------------------------
    //
    // Every control below writes straight through a MaterialDesc* and called touch(); nothing
    // was ever pushed, so Ctrl+Z after darkening a wall undid whatever came BEFORE it and left
    // the wall dark. EditCmd::Kind had no Material case at all.
    //
    // A DRAG IS ONE EDIT. `changed` is true on every frame of a slider drag, so pushing on it
    // would put a hundred entries on a 64-deep stack and evict everything else the user did.
    // ImGui's IsItemActivated/IsItemDeactivatedAfterEdit bracket the whole interaction, which is
    // the same begin/end shape beginTransformEdit/endTransformEdit already uses for the gizmo.
    //
    // `before` IS SNAPSHOTTED AT THE TOP OF THE FRAME, while nothing is active -- not on the
    // activation frame itself, because ImGui has already written the first drag delta into *d by
    // the time IsItemActivated() can be asked.
    if (!matEditActive_) { matEditBefore_ = *d; matEditHandle_ = handle; }
    bool started = false, finished = false;
    const auto track = [&](bool c) {
        started  |= ImGui::IsItemActivated();
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        return c;
    };
    changed |= track(ImGui::SliderFloat("Metallic", &d->metallicFactor, 0.0f, 1.0f));
    changed |= track(ImGui::SliderFloat("Roughness", &d->roughnessFactor, 0.045f, 1.0f));
    changed |= track(ImGui::SliderFloat("Normal Scale", &d->normalScale, 0.0f, 4.0f));
    changed |= track(ImGui::SliderFloat("Occlusion", &d->occlusionStrength, 0.0f, 1.0f));
    // 0..0.2 covers water (~0.02) through gemstone (~0.17).
    changed |= track(ImGui::SliderFloat("Reflectance", &d->reflectance, 0.0f, 0.2f, "%.3f"));
    changed |= track(ImGui::SliderFloat("Grazing (f90)", &d->f90, 0.0f, 1.0f));

    // IOR AND REFLECTANCE ARE THE SAME PHYSICAL FACT TWICE, which is why this control reports the
    // disagreement instead of quietly letting the two drift. F0 = ((1-n)/(1+n))^2, so 1.52 glass
    // implies 0.0426; a mismatch reflects an amount its own refraction calls impossible, invisible
    // until a grazing angle or the critical angle.
    // REPORTED, NOT ENFORCED: clamping reflectance to the ior would take away a knob authors
    // legitimately reach for (a thin film or coated lens really does deviate).
    changed |= track(ImGui::SliderFloat("IOR", &d->ior, 1.0f, 2.5f, "%.3f"));
    {
        const float n  = d->ior <= 0.0f ? 1.0f : d->ior;
        const float f0 = ((1.0f - n) / (1.0f + n)) * ((1.0f - n) / (1.0f + n));
        if (std::fabs(f0 - d->reflectance) > 0.005f) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.15f, 1.0f), "= F0 %.3f", f0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This IOR implies Reflectance %.3f, but it is set to %.3f.\n"
                                  "Deliberate for a coated or thin-film surface; a mistake otherwise.",
                                  f0, d->reflectance);
        }
    }
    // Transmission is the SUBSTRATE's property and applies whether or not the material is
    // blended: it scales the diffuse lobe (a transmissive surface must not also scatter its full
    // base colour back at you) and, on a blended material, pulls coverage toward 1 - transmission.
    changed |= track(ImGui::SliderFloat("Transmission", &d->transmission, 0.0f, 1.0f));
    if (d->transmission > 0.0f && d->alphaMode != pbr::AlphaMode::Blend) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.15f, 1.0f), "(opaque)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transmission still dims the diffuse lobe here, but this material is\n"
                              "not BLEND translucent, so nothing will be visible THROUGH it.");
    }
    // WEIGHT is the on/off: 0 skips the wrap-diffuse and back-scatter terms entirely, so RADIUS
    // (which only widens the back-scatter lobe those terms produce) has nothing to widen until
    // Weight is above 0. Disabling it at 0 keeps the panel from offering a control that does nothing.
    changed |= track(ImGui::SliderFloat("Subsurface Weight", &d->subsurfaceWeight, 0.0f, 1.0f));
    ImGui::BeginDisabled(d->subsurfaceWeight <= 0.0f);
    changed |= track(ImGui::SliderFloat("Subsurface Radius", &d->subsurfaceRadius, 0.0f, 1.0f));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Only does anything when Subsurface Weight is above 0.");
    changed |= track(ImGui::DragFloat3("Emissive", d->emissiveFactor, 0.01f, 0.0f, 32.0f));

    ImGui::Separator();
    int uvMode = static_cast<int>(d->uvMode);
    if (ImGui::Combo("UV Mapping", &uvMode, "Mesh UVs\0World Aligned\0")) {
        d->uvMode = static_cast<pbr::UvMode>(uvMode);
        changed = true;
    }
    // TRACKED THE SAME WAY, but read AFTER the widget and OUTSIDE its own if: a Combo that was
    // opened and dismissed without a change still activated and deactivated, and an interaction
    // that begins here must still close the bracket rather than leaving it open for whatever
    // control the user touches next.
    track(false);
    if (d->uvMode == pbr::UvMode::WorldAligned) {
        changed |= track(ImGui::SliderFloat("Tile Size (cm)", &d->uvTiling, 5.0f, 2000.0f, "%.0f",
                                      ImGuiSliderFlags_Logarithmic));
    }

    ImGui::Separator();
    for (u32 s = 0; s < pbr::kTextureSlotCount; ++s) {
        char buf[260];
        const std::string& p = d->textures[s].path;
        std::snprintf(buf, sizeof(buf), "%s", p.c_str());
        if (ImGui::InputText(pbr::MaterialLibrary::textureSlotName(static_cast<pbr::TextureSlot>(s)),
                             buf, sizeof(buf))) {
            d->textures[s].path = buf;
            changed = true;
        }
        // A TEXT FIELD IS THE CASE THE BRACKET EXISTS FOR: InputText reports `changed` on every
        // keystroke, so pushing per frame would put one undo entry per LETTER typed into a
        // texture path. IsItemDeactivatedAfterEdit fires once, when focus leaves.
        track(false);
    }
    if (started) matEditActive_ = true;
    // WHETHER ANYTHING MOVED IS ImGui'S ANSWER, NOT A COMPARISON. Diffing the two descs would
    // mean either a memcmp -- wrong, MaterialDesc holds std::strings whose pointers differ
    // without the value differing -- or an enumeration of every field the panel edits, which is
    // a list that goes stale the next time a control is added. Each control's own return value
    // already means "this was edited"; latching that across the interaction is the same fact,
    // and it cannot drift.
    if (changed) { matEditDirty_ = true; pbr::MaterialLibrary::get().touch(handle); }
    // PUSHED ON RELEASE. Clicking a slider without moving it still fires
    // IsItemDeactivatedAfterEdit on some widgets, and an entry that restores the state it was
    // already in is worse than none: it makes Ctrl+Z visibly do nothing, once.
    if (finished && matEditActive_ && matEditHandle_ == handle) {
        matEditActive_ = false;
        if (matEditDirty_) {
            matEditDirty_ = false;
            EditCmd c;
            c.kind = EditCmd::Kind::Material;
            c.matHandle = handle;
            c.matBefore = matEditBefore_;
            c.matAfter  = *d;
            pushEdit(std::move(c));
        }
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!project_.valid() || d->name.empty());
    if (ImGui::Button("Save to C#")) {
        std::string err;
        const std::string file = saveMaterialSource(d->name, *d, err);
        if (file.empty()) {
            matSaveStatus_ = "Could not save: " + err;
            AVER_ERROR("[Material] save failed for '{}': {}", d->name, err);
        } else {
            matSaveStatus_ = "Saved to " + std::filesystem::path(file).filename().string() +
                             " - Compile C# to regenerate the .ocmat";
            AVER_INFO("[Material] '{}' written back to {}", d->name, file);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(project_.valid()
            ? "Rewrites this material's Configure in Content\\Materials.\n"
              "The .ocmat under Binaries is regenerated by Compile C#."
            : "Open a project first - a material belongs to one.");
    if (!matSaveStatus_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", matSaveStatus_.c_str()); }
#else
    ImGui::SliderFloat("Metallic", &o.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &o.roughness, 0.02f, 1.0f);
#endif
}

// The MODE panel: the active editing mode's own tools and settings, on the left.
// WHY A PANEL AND NOT MORE TOOLBAR: the toolbar row is a good place for four brush buttons, a bad
// place for the dozen controls a real sculpt tool set needs (radius, strength, falloff, flatten
// target, a noise generator with four parameters). Those were either absent or buried in a popup
// behind a button labelled with a number. A panel can show its whole surface at once.
void SandboxApp::buildModePanel(Engine& e) {
    (void)e;
    ImGui::Begin("Mode");

    ImGui::PushStyleColor(ImGuiCol_Text, kAverOrange);
    ImGui::TextUnformatted(kEditorModeNames[static_cast<int>(mode_)]);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%s", kEditorModeHints[static_cast<int>(mode_)]);
    ImGui::Separator();
    ImGui::Spacing();

    switch (mode_) {
        case EditorMode::Select:    buildSelectModePanel();    break;
#if AVER_MODULE_LANDSCAPE
        case EditorMode::Landscape: buildLandscapeModePanel(e); break;
        case EditorMode::Foliage:   buildFoliageModePanel();   break;
#else
        case EditorMode::Landscape:
        case EditorMode::Foliage:
            ImGui::TextWrapped("This build has the landscape module switched off.");
            break;
#endif
        case EditorMode::Simulate:  buildSimulateModePanel();  break;
    }
    ImGui::End();
}

// A labelled text field backed by a std::string. True on the frame the value changed.
//
// ImGui edits a fixed char buffer, and the string it is backing may be arbitrarily long, so the
// copy is bounded and the write-back only happens when InputText says something changed -- a
// blind copy every frame would truncate a value the user never touched, on the first frame the
// page was opened.
 bool SandboxApp::editField(const char* label, std::string& value, usize cap) {
    std::vector<char> buf(cap, '\0');
    const usize n = value.size() < cap - 1 ? value.size() : cap - 1;
    std::memcpy(buf.data(), value.data(), n);
    ImGui::PushID(label);
    const bool changed = ImGui::InputText(label, buf.data(), cap);
    ImGui::PopID();
    if (changed) value.assign(buf.data());
    return changed;
}

void SandboxApp::buildSelectModePanel() {
    ImGui::TextDisabled("TRANSFORM");
    const char* names[4] = {"Select", "Move", "Rotate", "Scale"};
    for (int i = 0; i < 4; ++i) {
        const bool on = static_cast<int>(tool_) == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAverOrangeDim);
        if (ImGui::Button(names[i], ImVec2(-1, 0))) tool_ = static_cast<Tool>(i);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hotkey %d", i + 1);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("SPACE");
    if (ImGui::Button(worldSpace_ ? "World" : "Local", ImVec2(-1, 0))) worldSpace_ = !worldSpace_;
    ImGui::Spacing();
    // A CHECKBOX PLUS A STEP, matching the state the editor keeps: snapMove_ et al are whether
    // snapping is on, moveSnap_ et al are the increment -- two values because turning snapping off
    // must not lose the step you had set.
    // Checkbox on its own line, value under it full width: side by side, the value field got
    // whatever the checkbox label left over, clipping "15 deg" to "15 de".
    ImGui::TextDisabled("SNAPPING");
    ImGui::Checkbox("Grid (position)", &snapMove_);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##moveSnap", &moveSnap_, 1.0f, 1.0f, 1000.0f, "%.0f cm");
    ImGui::Checkbox("Angle (rotation)", &snapRot_);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##rotSnap", &rotSnap_, 1.0f, 1.0f, 90.0f, "%.0f deg");
    ImGui::Checkbox("Step (scale)", &snapScale_);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##sclSnap", &scaleSnap_, 0.01f, 0.01f, 1.0f, "%.2f");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Selected: %s", selectionLabel().c_str());
}

void SandboxApp::buildSimulateModePanel() {
    ImGui::TextDisabled("PLAY");
    ImGui::TextWrapped("Runs the game in the viewport: physics ticks, gameplay scripts run, and "
                       "input goes to the game instead of the editor.");
    ImGui::Spacing();
    const bool active = playSessionActive();
    if (!active) {
        if (ImGui::Button("Play", ImVec2(-1, 0))) startPlay();
    } else {
        if (ImGui::Button("Stop", ImVec2(-1, 0))) stopPlay();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Shift+Esc releases the mouse back to the editor.");
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_LANDSCAPE
// The landscape tool set. This is what the mode was missing.
// The level's WATER record, as a panel.
//
// WHY THERE WAS NONE. levelHeader_.waters is populated only by the loader and copied wholesale
// by saveLevel, so a water plane could be authored by hand-editing the .ocworld and in no other
// way -- no menu item, no Add entry, no Outliner row, nothing in Details. The Add menu offers
// Cube, Player Start and Sphere, with Plane and Point Light greyed out.
//
// ONE RECORD, because WaterRenderer holds one level and one wave set; applyLevelWater already
// warns when a file declares more and renders the first. Offering a list here would let someone
// author a second surface the renderer then silently ignores.
void SandboxApp::buildWaterPanel(Engine& e) {
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    ImGui::TextDisabled("WATER");
    if (levelHeader_.waters.empty()) {
        ImGui::TextDisabled("This level has no water.");
        if (ImGui::Button("Add Water", ImVec2(-1, 0))) {
            fmt::OcWaterPlacement wp;
            wp.name = "Water";
            // AT THE CAMERA'S FEET, not at z=0: a plane at the origin is invisible in a level
            // built up on terrain, and "nothing happened" is the worst answer a new button can
            // give. Rounded so the number in the field is one somebody would have typed.
            wp.levelCm = std::floor(camPos_.z / 10.0f) * 10.0f - 100.0;
            wp.infinite = true;
            levelHeader_.waters.assign(1, std::move(wp));
            applyLevelWater(e);
            setUpgradeStatus("Added water");
        }
        uiReg_.track("water.add");
        ImGui::Separator();
        return;
    }

    fmt::OcWaterPlacement& wp = levelHeader_.waters.front();
    bool changed = false;

    {
        char nameBuf[96];
        std::snprintf(nameBuf, sizeof nameBuf, "%s", wp.name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##watername", "Water", nameBuf, sizeof nameBuf)) {
            wp.name = nameBuf; changed = true;
        }
    }
    {
        f32 level = static_cast<f32>(wp.levelCm);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("Level (cm)", &level, 5.0f)) { wp.levelCm = level; changed = true; }
    }
    if (ImGui::Checkbox("Infinite", &wp.infinite)) changed = true;
    if (!wp.infinite) {
        // X AND Y ONLY. The record has no Z extent: the surface is a plane at `level`, and its
        // bounds are the footprint it covers, which is why OcWaterPlacement's bounds are 2D.
        f32 mn[2] = {static_cast<f32>(wp.boundsMin[0]), static_cast<f32>(wp.boundsMin[1])};
        f32 mx[2] = {static_cast<f32>(wp.boundsMax[0]), static_cast<f32>(wp.boundsMax[1])};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat2("Min X/Y", mn, 10.0f)) {
            wp.boundsMin[0] = mn[0]; wp.boundsMin[1] = mn[1]; changed = true;
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat2("Max X/Y", mx, 10.0f)) {
            wp.boundsMax[0] = mx[0]; wp.boundsMax[1] = mx[1]; changed = true;
        }
    }
    {
        char matBuf[96];
        std::snprintf(matBuf, sizeof matBuf, "%s", wp.material.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##watermat", "M_Water", matBuf, sizeof matBuf)) {
            wp.material = matBuf; changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An .ocmat name. Empty draws the renderer's own fallback look.");
    }
    // SIMULATE IS SHOWN BUT REFUSED WHEN INFINITE, which is the one pairing the format can carry
    // and nothing can honour -- a simulated volume is a closed shell and needs a size.
    // applyLevelWater says the same thing in a warning; saying it here stops the author reaching
    // a state that only complains later.
    ImGui::BeginDisabled(wp.infinite);
    if (ImGui::Checkbox("Simulate", &wp.simulate)) changed = true;
    ImGui::EndDisabled();
    if (wp.infinite && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A simulated volume needs bounds. Turn Infinite off first.");

    if (ImGui::Button("Remove Water", ImVec2(-1, 0))) {
        levelHeader_.waters.clear();
        levelHeader_.waves.clear();   // a WAVE names a WATER by name; orphaned it means nothing
        changed = true;
        setUpgradeStatus("Removed water");
    }
    uiReg_.track("water.remove");

    // RE-APPLIED ON EVERY EDIT, so the viewport agrees with the numbers. applyLevelWater is
    // idempotent for the analytic path -- it rebuilds the surface from the record it is handed.
    if (changed) applyLevelWater(e);
    ImGui::Separator();
#else
    (void)e;
#endif
}

void SandboxApp::buildLandscapeModePanel(Engine& e) {
    if (!landscapeLoaded_) {
        // A DEAD END UNTIL NOW: this printed one sentence and returned, so the terrain mode
        // offered nothing at all to a level without terrain -- and nothing anywhere else in the
        // editor could make some either.
        ImGui::TextWrapped("This level has no landscape section.");
        ImGui::Spacing();
        ImGui::TextDisabled("CREATE ONE");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("Samples", &landCreateSamples_, 129, 2049);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grid resolution per side. Must satisfy the quadtree's tiling\n"
                              "rule, which every power-of-two-plus-one in this range does.");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Spacing (cm)", &landCreateSpacingCm_, 25.0f, 400.0f, "%.0f");
        ImGui::TextDisabled("%.0f m across",
                            (landCreateSamples_ - 1) * landCreateSpacingCm_ / 100.0f);
        ImGui::Spacing();
        ImGui::TextDisabled("SHAPE");
        int seed = static_cast<int>(landscapeNoiseParams_.seed);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt("Seed", &seed)) landscapeNoiseParams_.seed = static_cast<u32>(seed);
        editor::panelFloat("Feature Size (cm)", &landscapeNoiseParams_.featureSizeCm, 500.0f, 40000.0f,
                   "%.0f", ImGuiSliderFlags_Logarithmic);
        editor::panelFloat("Amplitude (cm)", &landscapeNoiseParams_.amplitudeCm, 0.0f, 6000.0f, "%.0f");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("Octaves", &landscapeNoiseParams_.octaves, 1, 8);
        ImGui::Spacing();
        ImGui::BeginDisabled(!project_.valid());
        if (ImGui::Button("Create Landscape", ImVec2(-1, 0)))
            createLandscapeForLevel(e.device(), static_cast<u32>(landCreateSamples_),
                                    landCreateSpacingCm_);
        ImGui::EndDisabled();
        uiReg_.track("landscape.create");
        if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Open or create a project first - a landscape is written into one.");
        ImGui::TextDisabled("Writes <project>\\Content\\Landscape\\<Level>.ocland and adds the\n"
                            "LANDSCAPE record, so the next open finds it.");
        return;
    }

    // ---- where the section sits, which had no control at all -------------------------------
    // The LANDSCAPE record's x/y/z were readable only by hand-editing the .ocworld. They are the
    // section's own origin sample in world space, so this is how a terrain is aligned to the
    // rest of a level rather than to wherever the heightfield happened to be authored.
    ImGui::TextDisabled("PLACEMENT");
    {
        f32 org[3] = {landscapeData_.originCm[0], landscapeData_.originCm[1],
                      landscapeData_.originCm[2]};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("Origin (cm)", org, 10.0f)) {
            landscapeData_.originCm[0] = org[0];
            landscapeData_.originCm[1] = org[1];
            landscapeData_.originCm[2] = org[2];
            landscapeDirty_ = true;
            recordLandscapeInLevel();
        }
        uiReg_.track("landscape.origin");
    }
    ImGui::Spacing();

    ImGui::TextDisabled("SCULPT");
    // 6 tools, but only the first 4 have a bound hotkey (SculptRaise..SculptFlatten, keys 1-4) --
    // Ramp and Noise are new and mouse/panel-only for now, so the tooltip below is not shown for
    // them rather than advertising a key that does nothing.
    for (int i = 0; i < 6; ++i) {
        const bool on = static_cast<int>(sculptTool_) == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAverOrangeDim);
        if (ImGui::Button(kSculptToolNames[i], ImVec2(-1, 0))) sculptTool_ = static_cast<SculptTool>(i);
        if (on) ImGui::PopStyleColor();
        if (i < 4 && ImGui::IsItemHovered()) ImGui::SetTooltip("Hotkey %d", i + 1);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("BRUSH");
    editor::panelFloat("Radius (cm)", &sculptRadiusCm_, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelFloat("Strength (cm/s)", &sculptStrengthCm_, 5.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    // FALLOFF is new all the way down: applyBrush's curve was a hardcoded smoothstep with no way
    // to reach it. A hard-edged brush and a soft one are different tools for different jobs, and
    // every terrain editor exposes the difference.
    editor::panelFloat("Falloff", &sculptFalloff_, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 is a hard edge, 1 is a soft smoothstep shoulder.");

    if (sculptTool_ == SculptTool::Flatten) {
        ImGui::Spacing();
        ImGui::TextDisabled("FLATTEN TARGET");
        ImGui::Text("%.0f cm", sculptFlattenTargetCm_);
        ImGui::TextDisabled("Picked from the first click of each stroke.");
    }
    if (sculptTool_ == SculptTool::Ramp) {
        ImGui::Spacing();
        ImGui::TextDisabled("RAMP START");
        ImGui::Text("(%.0f, %.0f) @ %.0f cm", sculptRampStartCm_[0], sculptRampStartCm_[1],
                    sculptRampStartHeightCm_);
        ImGui::TextDisabled("Picked from the first click of each stroke. Strength is the total");
        ImGui::TextDisabled("rise from there to wherever the brush is now.");
    }
    if (sculptTool_ == SculptTool::Noise) {
        ImGui::Spacing();
        ImGui::TextDisabled("NOISE SEED");
        ImGui::Text("%u", sculptNoiseSeed_);
        ImGui::TextDisabled("Derived from the first click of each stroke -- not the clock -- so");
        ImGui::TextDisabled("re-applying the same stroke gives the same terrain.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("GENERATE");
    // The noise generator existed and was reachable from nowhere: terrainHeightAt() was wired
    // only as the height source for procedural tiles past the authored rim, never as something a
    // level author could apply to the section they are editing.
    editor::panelFloat("Feature size (cm)", &landscapeNoiseParams_.featureSizeCm, 500.0f, 50000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelFloat("Amplitude (cm)", &landscapeNoiseParams_.amplitudeCm, 0.0f, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelInt("Octaves", &landscapeNoiseParams_.octaves, 1, 8);
    int seed = static_cast<int>(landscapeNoiseParams_.seed);
    ImGui::TextUnformatted("Seed");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##Seed", &seed)) landscapeNoiseParams_.seed = static_cast<u32>(seed);
    if (ImGui::Button("Generate terrain", ImVec2(-1, 0))) generateLandscapeNoise(e);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replaces every height in this section with the noise above.\n"
                          "One undo step -- Ctrl+Z restores what was here.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(landscapeDirty_ ? "Save Terrain *" : "Save Terrain", ImVec2(-1, 0))) saveLandscape();
    ImGui::TextDisabled("%u x %u samples, %.0f cm spacing",
                        landscapeData_.sampleCount, landscapeData_.sampleCount,
                        landscapeData_.spacingCm);
}

// The Foliage panel's empty-palette action: writes a starter .ocfoliage and opens its tab,
// reusing cbCreateFoliageType() rather than duplicating its write-then-open logic (see that
// function's own comment for why it is unconditional and what it writes). The only thing this
// wrapper adds is WHERE it lands: cbCreateFoliageType() writes into cbSelectedDir_, the Content
// Browser's currently browsed folder, which from this panel could be anything -- read-only engine
// content, some deeply nested subfolder, or empty. Content/Foliage is created if needed and the
// browser is pointed at it first, so a click here always lands somewhere sensible instead of
// wherever the browser last happened to be. cbCreateFoliageType() itself already refreshes the
// palette on success, so a freshly created type is paintable on this very frame.
void SandboxApp::foliagePanelCreateType() {
    if (!project_.valid()) return;
    const std::filesystem::path dir = std::filesystem::path(project_.contentDir()) / "Foliage";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    cbNavigate(dir.string());
    cbCreateFoliageType();
}

void SandboxApp::buildFoliageModePanel() {
    if (foliagePalette_.empty()) {
        ImGui::TextWrapped("No foliage types are authored in this project yet, so Foliage mode "
                           "has nothing to paint.");
        ImGui::Spacing();
        if (ImGui::Button("Create Foliage Type", ImVec2(-1, 0))) foliagePanelCreateType();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes a starter .ocfoliage into Content/Foliage and opens its "
                              "tab -- the palette refreshes as soon as it's saved.");
        return;
    }
    ImGui::TextDisabled("PALETTE");
    ImGui::TextWrapped("Ticked types are placed, chosen at random (weighted by each type's own "
                       "Weight) per instance. Scale, spacing, slope alignment and material are "
                       "authored on each type's own tab -- click " ICON_EDIT " to open it.");
    for (auto& sp : foliagePalette_) {
        ImGui::PushID(sp.assetPath.c_str());
        ImGui::Checkbox(sp.name.c_str(), &sp.enabled);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_EDIT)) assetEditors_.open(sp.assetPath);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open %s", sp.assetPath.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("scale %.2f-%.2f, weight %.2f",
                            sp.type.scaleMin, sp.type.scaleMax, sp.type.weight);
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("BRUSH");
    editor::panelFloat("Radius (cm)", &foliageRadiusCm_, 100.0f, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelFloat("Density", &foliageDensity_, 1.0f, 64.0f, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Placements attempted per brush application.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Hold Shift while painting to erase.");
}

#endif
#endif

#if AVER_WITH_IMGUI
// Marks a level record as one this level now carries, because the author has been editing it.
//
// THE BUG THIS CLOSES. hasLevelSun_/hasLevelSky_/hasLevelFog_ were set ONLY by the loader, and
// saveLevel writes each record only when its flag is set. So on a level whose file carried no
// SUN, every slider under "Directional Light (Sun)" was live in the viewport and thrown away on
// save -- no dirty marker, no warning, and saveLevel's own comment claiming Details edits
// survive was true only for levels that already had the record.
//
// IsAnyItemActive is FRAME-GLOBAL, not scoped to this section, and that is a deliberate trade
// rather than an oversight: dragging a control in another panel while this section happens to
// be selected also sets the flag. The cost of that false positive is one extra record in the
// file, describing exactly the sun the author was already looking at. The cost of the false
// NEGATIVE it replaces was silently discarding their work.
// Notes that a level RECORD (sun, fog, clouds, sky) has been authored, so saveLevel writes it.
//
// AND MARKS THE DOCUMENT DIRTY, WHICH IT DID NOT. levelHasUnsavedEdits() is driven purely by the
// undo serial, and none of these panels push an EditCmd -- so changing the sun angle and closing
// the editor gave no prompt, no autosave, and the change was gone. The record flag alone only
// means "write this IF something else causes a save".
//
// THE TWO ARE THE SAME FACT. Setting `has` is exactly the statement that saveLevel will now emit
// a record it would not have emitted before, i.e. that the document differs from the file. That
// is the definition of dirty, so anything setting one must set the other.
//
// markLevelUnsaved() rather than a serial bump, because these edits are NOT undoable: there is
// no command to walk back to, so the mark must be the one that cannot be cleared by undoing.
// Saving clears it, which is the only thing that should.
//
// THE PREDICATE IS LOOSE AND STAYS LOOSE: IsAnyItemActive() is true for an active item ANYWHERE,
// not just in this section, so dragging an unrelated slider while a sun panel is open marks the
// level dirty. That looseness is pre-existing -- it already decided whether the record got
// written at all -- and the failure it now causes is a needless save prompt, against a failure
// it prevents of silently losing authored lighting. Not a trade worth agonising over.
void SandboxApp::markLevelRecordEdited(bool& has) {
    if (ImGui::IsAnyItemActive()) { has = true; markLevelUnsaved(); }
}

// Draws the World Outliner and the Details panel, each only when Window > ... has it on.
//
// SPLIT SO EACH CAN BE HIDDEN. ImGui::Begin does NOT skip a window whose p_open points at
// false -- that early-out is BeginPopupModal's, not Begin's -- so a hideable panel has to be
// guarded by its caller. These were one function submitting both unconditionally, which is why
// their Window menu items could never do anything.
void SandboxApp::buildPanels(Engine& e) {
    if (showOutliner_) buildOutlinerPanel();
    // TAKES THE ENGINE NOW: the Level branch of the Details panel edits the level's WATER
    // record, and applying that to the live surface needs a device. The (void)e that used to sit
    // here was the sign that nothing in these panels had yet needed one.
    if (showDetails_)  buildDetailsPanel(e);
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
// ASCII lowercase, matching the Content Browser's own filter rather than inventing a second
// rule: a name here is an editor label, not user text needing locale-aware folding.
 std::string SandboxApp::lowerCopy(std::string v) {
    for (char& c : v) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return v;
}

// The name a row and a drag preview show for an entity.
std::string SandboxApp::outlinerLabelFor(scene::Entity e) const {
    const scene::World& w = scene::World::instance();
    const auto lit = entityLabels_.find(static_cast<u32>(e));
    if (lit != entityLabels_.end()) return lit->second;
    const std::string nm = w.name(e);
    return nm.empty() ? ("Entity " + std::to_string((u32)e)) : nm;
}

// Makes a row draggable. Default flags on purpose: TreeNodeEx already opens a collapsed parent
// when a drag hovers it, and that is free unless SourceNoHoldToOpenOthers is passed.
void SandboxApp::drawOutlinerDragSource(scene::Entity ent) {
    if (!ImGui::BeginDragDropSource()) return;
    ImGui::SetDragDropPayload(kOutlinerReparentDragDropType, &ent, sizeof(scene::Entity));
    ImGui::TextUnformatted(outlinerLabelFor(ent).c_str());
    ImGui::EndDragDropSource();
}

// Makes a row a drop target, in two tiers.
//
// A CYCLE IS NEVER OFFERED. The row peeks at the payload BEFORE calling BeginDragDropTarget, so
// dropping onto yourself or your own descendant has no target at all -- no highlight, nothing
// to click. That is GraphEditor's and BtEditor's idiom: filter before offering, rather than
// accepting and then explaining. World::setParent refuses a cycle anyway; this is so the UI
// never proposes one.
//
// AN OFF-LEVEL ENDPOINT IS OFFERED AND REFUSED, with a reason. It would succeed in the live
// world and be GONE on the next save, because saveLevel writes a parent only for entities the
// level owns. A drop that visibly works and quietly reverts is a worse trap than one that says
// why it cannot happen -- which is the whole reason the two commits before this one exist.
void SandboxApp::drawOutlinerDropTarget(scene::Entity ent) {
    scene::World& w = scene::World::instance();
    scene::Entity dragged = scene::kInvalidEntity;
    if (const ImGuiPayload* peek = ImGui::GetDragDropPayload())
        if (peek->IsDataType(kOutlinerReparentDragDropType) &&
            peek->DataSize == static_cast<int>(sizeof(scene::Entity)))
            dragged = *static_cast<const scene::Entity*>(peek->Data);

    if (dragged != scene::kInvalidEntity && outlinerIsAncestorOf(w, dragged, ent)) return;

    if (!ImGui::BeginDragDropTarget()) return;
    const bool offLevel = dragged != scene::kInvalidEntity &&
                          reparentLegality(dragged, ent) == ReparentLegality::OffLevel;
    if (offLevel) {
        // PEEK ONLY, so ImGui never paints the row as about to accept. The delivering branch is
        // structurally unreachable for this case, not merely un-taken.
        ImGui::AcceptDragDropPayload(kOutlinerReparentDragDropType,
                                     ImGuiDragDropFlags_AcceptPeekOnly);
        ImGui::SetTooltip("'%s' is not part of the saved level. This relationship would be lost "
                          "on the next save.",
                          outlinerLabelFor(isLevelOwned(dragged) ? ent : dragged).c_str());
    } else if (const ImGuiPayload* payload =
                   ImGui::AcceptDragDropPayload(kOutlinerReparentDragDropType)) {
        if (payload->DataSize == static_cast<int>(sizeof(scene::Entity)))
            pushReparent(*static_cast<const scene::Entity*>(payload->Data), ent);
    }
    ImGui::EndDragDropTarget();
}

// Draws one row and, when it is open, its children beneath it.
void SandboxApp::drawOutlinerRow(const OutlinerRow& row,
                     const std::unordered_map<u32, std::vector<const OutlinerRow*>>& children,
                     int depth) {
    const auto it = children.find(static_cast<u32>(row.ent));
    const bool hasKids = it != children.end() && !it->second.empty();

    // NEITHER _Framed NOR _FramePadding, and that is the gates constraint rather than taste.
    // For a plain TreeNodeEx the vertical padding is min(CurrLineTextBaseOffset, FramePadding.y)
    // and that offset is 0 at the start of a row, so the row is exactly as tall as the
    // Selectable it replaces. Either flag would add FramePadding.y*2. This panel is a
    // ratio-sized dock node and cannot reach the Level viewport rect anyway, but the rule costs
    // nothing to hold and the reason is worth writing down. SpanAvailWidth matches the Content
    // Browser's own folder tree rather than inventing a second convention.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    // THE WHOLE SET IS HIGHLIGHTED, not just the anchor -- a multi-select the user cannot SEE
    // is indistinguishable from a broken one, and the first thing they would do is click again
    // and lose it.
    if (sel_ == kSelScene && (selEntity_ == row.ent || multiIsSelected(row.ent)))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasKids) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow;
        // A scene with no hierarchy in it has to look exactly as it did before this change.
        if (depth == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    // THE ROW BECOMES A TEXT FIELD while it is being renamed, rather than opening a dialog:
    // the name is edited where it is read, which is what every file browser and every other
    // editor's outliner does, and it keeps the tree's shape from jumping under the cursor.
    if (outlinerRenaming_ == row.ent) {
        ImGui::SetNextItemWidth(-1.0f);
        if (outlinerRenameFocus_) { ImGui::SetKeyboardFocusHere(); outlinerRenameFocus_ = false; }
        const bool done = ImGui::InputText(("##ren" + std::to_string((u32)row.ent)).c_str(),
                                           outlinerRenameBuf_, sizeof outlinerRenameBuf_,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        // Escape abandons; Enter or clicking away commits. Deactivation covers both the click
        // and the Escape, so the key has to be tested first.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            outlinerRenaming_ = scene::kInvalidEntity;
        } else if (done || ImGui::IsItemDeactivatedAfterEdit()) {
            renameEntity(row.ent, outlinerRenameBuf_);
            outlinerRenaming_ = scene::kInvalidEntity;
        } else if (ImGui::IsItemDeactivated()) {
            outlinerRenaming_ = scene::kInvalidEntity;   // clicked away without editing
        }
        // The children still have to be walked, or renaming a parent collapses the tree for a
        // frame. TreeNodeEx is skipped, so nothing was pushed and nothing must be popped.
        if (hasKids) for (const OutlinerRow* c : it->second) drawOutlinerRow(*c, children, depth + 1);
        return;
    }

    // EVERY DRAWN ROW, IN ORDER, so a shift-click has a range to walk. Recorded here rather
    // than rebuilt on demand because only this walk knows the tree's filtered, sorted, expanded
    // shape -- the same reason the Content Browser ranges over its `shown` list and not the
    // folder's contents.
    outlinerOrder_.push_back(row.ent);

    const std::string label = "  " + row.shown + "##e" + std::to_string((u32)row.ent);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    // IsItemToggledOpen separates "clicked the arrow" from "clicked the label" on one node, so
    // expanding a parent does not also select it.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        // SAME KEYS AS THE CONTENT BROWSER NEXT DOOR, and as every file browser: shift extends a
        // range from the anchor, ctrl toggles one, a bare click replaces. Making the two panels
        // disagree about this would be worse than either behaviour on its own.
        const ImGuiIO& cio = ImGui::GetIO();
        if (cio.KeyShift && sel_ == kSelScene && selEntity_ != scene::kInvalidEntity)
            multiRange(row.ent);
        else if (cio.KeyCtrl)
            multiToggle(row.ent);
        else
            multiSetSingle(row.ent);
    }
    // F2 AND RIGHT-CLICK > RENAME, the two gestures a file browser has trained everyone to
    // expect -- and the ones the Content Browser next door already implements for a FILE. The
    // Outliner had neither, for an entity.
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        // Renaming is a one-entity act, so it collapses the selection rather than renaming the
        // anchor of a set and leaving the rest looking selected but untouched.
        multiSetSingle(row.ent);
        beginOutlinerRename(row.ent);
    }
    if (ImGui::BeginPopupContextItem(("##ctx" + std::to_string((u32)row.ent)).c_str())) {
        // A right-click on a row that is ALREADY part of the selection keeps the whole set --
        // otherwise "select five, right-click, Delete" would delete one, which is the single most
        // annoying way for a multi-select to be half-implemented. Right-clicking OUTSIDE the set
        // selects just that row, as everywhere else.
        if (!multiIsSelected(row.ent)) multiSetSingle(row.ent);
        else { sel_ = kSelScene; selEntity_ = row.ent; }
        if (ImGui::MenuItem("Rename", "F2")) beginOutlinerRename(row.ent);
        // THE OTHER HALF OF WHAT A RIGHT-CLICK IS FOR. This menu offered Rename and nothing else,
        // so the one place a person looks to delete a thing in a tree had no way to. Goes through
        // deleteSelection() rather than destroyEntity() so it captures the subtree for undo -- the
        // row above has already made this entity the selection.
        // DEFERRED, NOT DONE HERE, for two reasons that both bite. TreeNodeEx has already
        // PUSHED for a row with children, and the matching TreePop is below -- returning early
        // from here would leave ImGui's tree stack unbalanced. And this walk holds references
        // into `children` and `row`, which destroying an entity mid-walk invalidates. The
        // request is answered after the whole tree is drawn.
        if (ImGui::MenuItem("Delete", editor::chordToString(
                keybinds_.chordFor(editor::CommandId::EditDelete)).c_str())) {
            outlinerDeleteRequest_ = row.ent;
        }
        ImGui::EndPopup();
    }
    uiReg_.track(("outliner.row." + std::to_string((u32)row.ent)).c_str());

    drawOutlinerDragSource(row.ent);
    drawOutlinerDropTarget(row.ent);

    if (hasKids && open) {
        for (const OutlinerRow* c : it->second) drawOutlinerRow(*c, children, depth + 1);
        ImGui::TreePop();
    }
}

#endif
#endif

#if AVER_WITH_IMGUI
void SandboxApp::buildOutlinerPanel() {
    ImGui::Begin("World Outliner", &showOutliner_);
    // So the edit verbs work on a selection made HERE -- see the dispatch in handleManip.
    outlinerFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // A FILTER, because an alphabetical tree of six thousand entities is a list you scroll past,
    // not one you find anything in. Sponza alone lists a few hundred.
    //
    // MATCHES A ROW BY NAME AND KEEPS ITS ANCESTORS. Hiding a parent whose child matched would
    // orphan the match -- the tree is drawn by walking roots down, so a row whose parent is gone
    // is never reached and the filter would appear to find nothing. Keeping the chain is what
    // makes a hit visible in the place it actually lives.
    {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", outlinerFilter_.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##outlinerFilter", ICON_SEARCH " Filter by name", buf, sizeof buf))
            outlinerFilter_ = buf;
        uiReg_.track("outliner.filter");
    }
    if (!hideEditorScene_)
        for (int i=0;i<(int)objects_.size();++i) {
            if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) { sel_=i; selEntity_=kInvalidId; }
            uiReg_.track(("outliner.placeholder." + std::to_string(i)).c_str());
        }
#if AVER_MODULE_SCENE
    {
        scene::World& w = scene::World::instance();
        const u32 n = w.count();

        // PASS ONE: which entities are listed at all. The three filters are unchanged.
        std::vector<OutlinerRow> rows;
        std::unordered_set<u32> survived;
        rows.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            // Chunk-streamed entities are excluded on purpose: potentially hundreds of them come
            // and go as the camera moves, and this list is for what a designer placed. Their
            // live count is in the Chunk Streaming stats window instead (buildChunkStreamingPanel).
            if (anyChunkWorldOwns(ent)) continue;
            // The graph-driven drone is excluded for the identical reason: transient, not
            // authored, tracked separately (see the [Drone] AVER_INFO lines / the Details panel
            // if selected directly some other way -- there isn't one; it just isn't listed here).
            if (ent == droneEntity_) continue;
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            const std::string nm = w.name(ent);
            // Anything drawable, plus anything named -- an empty used as a parent is still a
            // node someone needs to be able to reach. RELAXED for the tree: an unnamed,
            // mesh-less entity is also kept once it HAS a child, because hiding a group pivot
            // the instant somebody drops onto it would make a successful reparent look like it
            // had silently failed. childCount is the raw engine count, so a pivot whose children
            // are all chunk- or drone-filtered shows as an empty-looking leaf; narrow enough to
            // accept rather than add a third pass over the survivor set.
            if (!mr && nm.empty() && w.childCount(ent) == 0) continue;
            rows.push_back(OutlinerRow{ent, w.parent(ent), outlinerLabelFor(ent)});
            survived.insert(static_cast<u32>(ent));
        }

        // PASS 1b: THE NAME FILTER, applied after the rows exist so ancestors can be kept.
        //
        // A row matches on a case-insensitive substring of its shown label. Its ANCESTORS are
        // kept too, even when they do not match: the tree below is drawn by walking roots
        // downward, so a row whose parent was dropped is simply never visited -- filtering
        // naively would hide every nested match and the box would look broken on exactly the
        // deep hierarchies it is most needed for. Descendants of a match are NOT kept: "show me
        // the thing I named" should not unfold its entire subtree.
        if (!outlinerFilter_.empty()) {
            const std::string needle = lowerCopy(outlinerFilter_);
            std::unordered_map<u32, const OutlinerRow*> byEnt;
            for (const OutlinerRow& r : rows) byEnt[static_cast<u32>(r.ent)] = &r;

            std::unordered_set<u32> keep;
            for (const OutlinerRow& r : rows) {
                if (lowerCopy(r.shown).find(needle) == std::string::npos) continue;
                keep.insert(static_cast<u32>(r.ent));
                // Walk up through the rows we actually have, not through World: an ancestor that
                // was already excluded by the filters above is not a row and must not be revived.
                for (scene::Entity p = r.par; p != scene::kInvalidEntity;) {
                    const auto it = byEnt.find(static_cast<u32>(p));
                    if (it == byEnt.end()) break;
                    if (!keep.insert(static_cast<u32>(p)).second) break;   // already walked
                    p = it->second->par;
                }
            }
            std::vector<OutlinerRow> kept;
            kept.reserve(keep.size());
            for (const OutlinerRow& r : rows)
                if (keep.count(static_cast<u32>(r.ent))) kept.push_back(r);
            rows.swap(kept);
            survived.clear();
            for (const OutlinerRow& r : rows) survived.insert(static_cast<u32>(r.ent));
        }

        // PASS TWO, and it HAS to be a second pass: w.at() walks the dense array, which is
        // swap-with-last on destroy and carries no ordering guarantee, so a child can appear
        // before its parent. Testing parent membership against a set still being filled would
        // promote the children of a later-indexed parent to the root list.
        std::unordered_map<u32, std::vector<const OutlinerRow*>> children;
        std::vector<const OutlinerRow*> roots;
        for (const OutlinerRow& r : rows) {
            if (r.par != scene::kInvalidEntity && survived.count(static_cast<u32>(r.par)))
                children[static_cast<u32>(r.par)].push_back(&r);
            else
                roots.push_back(&r);   // a true root, OR one whose parent is filtered out or
                                       // gone: promoted so it stays reachable, never dropped.
        }
        // ALPHABETICAL, not scan order: linkToParent PREPENDS, so the engine's child order is
        // newest-first and the list would visibly reshuffle every time anything is attached.
        const auto byLabel = [](const OutlinerRow* a, const OutlinerRow* b) { return a->shown < b->shown; };
        std::sort(roots.begin(), roots.end(), byLabel);
        for (auto& kv : children) std::sort(kv.second.begin(), kv.second.end(), byLabel);

        if (!roots.empty() && !hideEditorScene_) ImGui::Separator();
        // Rebuilt from scratch every frame: rows appear and vanish as folders expand and the
        // filter changes, and a stale order would range over rows that are no longer on screen.
        outlinerOrder_.clear();
        for (const OutlinerRow* r : roots) drawOutlinerRow(*r, children, 0);

        // THE DEFERRED DELETE, answered here: the tree is fully drawn, every TreePop is paired,
        // and nothing below reads the row structures any more. Routed through deleteSelection so
        // it captures the subtree for undo exactly as the menu and the key do.
        if (outlinerDeleteRequest_ != scene::kInvalidEntity) {
            const scene::Entity doomed = outlinerDeleteRequest_;
            outlinerDeleteRequest_ = scene::kInvalidEntity;
            if (scene::World::instance().valid(doomed)) {
                sel_ = kSelScene; selEntity_ = doomed;
                deleteSelection();
            }
        }

        // DROP HERE TO UNPARENT. Promoting something to a root can never cycle, so this target
        // needs no legality peek; pushReparent still refuses an off-level entity.
        ImGui::InvisibleButton("##outlinerRootDrop",
                               ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing()));
        uiReg_.track("outliner.rootDropZone");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(kOutlinerReparentDragDropType))
                if (payload->DataSize == static_cast<int>(sizeof(scene::Entity)))
                    pushReparent(*static_cast<const scene::Entity*>(payload->Data),
                                 scene::kInvalidEntity);
            ImGui::EndDragDropTarget();
        }
    }
#endif
    ImGui::Separator();
    // Unguarded: the sun/sky/post-process pseudo-entries exist whether or not there is a scene.
    if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) { sel_=-2; selEntity_=kInvalidId; }
    uiReg_.track("outliner.sun");
    if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3))        { sel_=-3; selEntity_=kInvalidId; }
    uiReg_.track("outliner.sky");
    if (ImGui::Selectable("  Post Process", sel_==-4))            { sel_=-4; selEntity_=kInvalidId; }
    uiReg_.track("outliner.postProcess");
    ImGui::End();
}

void SandboxApp::buildDetailsPanel(Engine& e) {
    ImGui::Begin("Details", &showDetails_);
    detailsFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (sel_>=0 && sel_<(int)objects_.size()){
        MeshObj& o=objects_[sel_]; ImGui::TextUnformatted(o.name.c_str()); ImGui::Separator();
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            // BRACKETED FOR UNDO, exactly as the scene-entity branch below does. These three
            // drags were the only transform edits in the editor that pushed nothing: the
            // machinery already handles placeholder objects (selectedXform and endTransformEdit
            // both branch for them, and the GIZMO drives these same objects through it), so
            // dragging a placeholder in the viewport was undoable and typing the same number
            // here was not.
            ImGui::DragFloat3("Location (cm)", &o.pos.x, 1.0f);
            if (ImGui::IsItemActivated()) beginTransformEdit();
            bool objDone = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::DragFloat3("Rotation", &o.rotDeg.x, 1.0f);
            if (ImGui::IsItemActivated()) beginTransformEdit();
            objDone = objDone || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::DragFloat3("Scale", &o.scale.x, 0.01f, 0.02f, 100.f);
            if (ImGui::IsItemActivated()) beginTransformEdit();
            objDone = objDone || ImGui::IsItemDeactivatedAfterEdit();
            if (objDone) endTransformEdit();
        }
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Base Color", o.color);
            materialPanel(o.material);
        }
        ImGui::Checkbox("Visible", &o.visible);
#if AVER_MODULE_SCENE
    } else if (sel_==kSelScene && scene::World::instance().valid(selEntity_)) {
        scene::World& w = scene::World::instance();
        const std::string nm = w.name(selEntity_);
        const auto lit = entityLabels_.find(static_cast<u32>(selEntity_));
        // EDITABLE, WHICH IT HAS NEVER BEEN. This was TextUnformatted and there was no rename
        // affordance anywhere in the editor -- not here, not in the Outliner, no F2, no context
        // menu -- so a level of "Cube 1..40" stayed that way. entityLabels_ was written only at
        // spawn/paste/duplicate/load and never from anything a person did.
        //
        // COMMITTED ON ENTER OR ON LOSING FOCUS, not per keystroke, for the reason the material
        // panel spells out: otherwise every letter typed is its own undo entry.
        {
            char nameBuf[128];
            std::snprintf(nameBuf, sizeof nameBuf, "%s",
                          lit != entityLabels_.end() ? lit->second.c_str()
                                                     : (nm.empty() ? "" : nm.c_str()));
            ImGui::SetNextItemWidth(-1.0f);
            const bool entered = ImGui::InputTextWithHint(
                "##entityname", nm.empty() ? "(unnamed entity)" : nm.c_str(),
                nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
            if (entered || ImGui::IsItemDeactivatedAfterEdit())
                renameEntity(selEntity_, nameBuf);
            uiReg_.track("details.entityName");
        }
        ImGui::TextDisabled("#%u  %s", (u32)selEntity_, nm.c_str());
        // MULTI-SELECTION. Computed once and reused by Transform/Mesh/Material below, which all
        // apply to the whole set; name and components stay single-entity (the anchor alone, per
        // multiSel_'s own header comment -- "one entity to talk about"), so this doubles as the
        // reminder that the name field above speaks for the anchor only.
        const std::vector<scene::Entity> multiSelected = selectedEntities();
        if (multiSelected.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(anchor of %d selected)", (int)multiSelected.size());
        }
        ImGui::Separator();
        if (multiSelected.size() > 1)
            ImGui::TextDisabled("%d entities selected -- Transform/Mesh/Material below apply to "
                                "all of them; name/components affect the anchor only.",
                                (int)multiSelected.size());
        if (const auto* loc = w.component<scene::CLocal>(selEntity_, scene::kComponentLocal)) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
              if (multiSelected.size() <= 1) {
                Transform xf = loc->xf;
                Vec3 euler = eulerDegFromQuat(xf.rotation);
                bool moved = ImGui::DragFloat3("Location (cm)", &xf.position.x, 1.0f);
                if (ImGui::IsItemActivated()) beginTransformEdit();
                bool done = ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::DragFloat3("Rotation", &euler.x, 1.0f)) {
                    xf.rotation = quatFromEulerDeg(euler); moved = true;
                }
                if (ImGui::IsItemActivated()) beginTransformEdit();
                done = done || ImGui::IsItemDeactivatedAfterEdit();
                moved |= ImGui::DragFloat3("Scale", &xf.scale.x, 0.5f, 0.01f, 100000.0f);
                if (ImGui::IsItemActivated()) beginTransformEdit();
                done = done || ImGui::IsItemDeactivatedAfterEdit();
                if (moved) w.setLocalTransform(selEntity_, xf);
                if (done) endTransformEdit();
              } else {
                // MULTI-SELECTION TRANSFORM, PER COMPONENT. The anchor's own value is shown --
                // same DragFloat3 calls as the single-entity branch above -- but a component that
                // is actually EDITED this frame is a SET applied to every selected entity, not the
                // gizmo's world-space DELTA (see the drag handler in SandboxViewport.cpp): typing
                // Z=0 puts every selected prop on the floor at its own X/Y, rather than sliding the
                // whole group by however far the anchor moved. Untouched components are left
                // exactly where each entity's own transform already had them -- which is why a
                // component is diffed against `orig*`, the anchor's reading from the TOP of this
                // frame before any widget touched it, rather than assumed from which DragFloat3
                // fired.
                //
                // forEachMultiMoved SUPPLIES THE SET, both for the "(mixed)" check below and for
                // the apply: it already excludes the anchor and any entity whose parent is also
                // selected, the same rule the gizmo drag uses so a selected child is not moved
                // twice (endTransformEdit's own multiMoveBefore_/alsoMoved diff -- unchanged here
                // -- then turns whatever this writes into the one undo entry for the gesture).
                Transform xf = loc->xf;
                Vec3 euler = eulerDegFromQuat(xf.rotation);
                const Vec3 origPos = xf.position, origEuler = euler, origScale = xf.scale;

                bool posMixed[3] = {false, false, false};
                bool rotMixed[3] = {false, false, false};
                bool scaleMixed[3] = {false, false, false};
                // SAME TOLERANCE nearlySameXform/nearlySameTransform use elsewhere in this class:
                // a "(mixed)" flag is a judgement call about two numbers meaning the same thing,
                // not the bit-exact touch test the apply below needs.
                forEachMultiMoved([&](scene::Entity, const Transform& oxf) {
                    const Vec3 oe = eulerDegFromQuat(oxf.rotation);
                    if (std::fabs(oxf.position.x - origPos.x) > 1e-4f) posMixed[0] = true;
                    if (std::fabs(oxf.position.y - origPos.y) > 1e-4f) posMixed[1] = true;
                    if (std::fabs(oxf.position.z - origPos.z) > 1e-4f) posMixed[2] = true;
                    if (std::fabs(oe.x - origEuler.x) > 1e-2f) rotMixed[0] = true;
                    if (std::fabs(oe.y - origEuler.y) > 1e-2f) rotMixed[1] = true;
                    if (std::fabs(oe.z - origEuler.z) > 1e-2f) rotMixed[2] = true;
                    if (std::fabs(oxf.scale.x - origScale.x) > 1e-4f) scaleMixed[0] = true;
                    if (std::fabs(oxf.scale.y - origScale.y) > 1e-4f) scaleMixed[1] = true;
                    if (std::fabs(oxf.scale.z - origScale.z) > 1e-4f) scaleMixed[2] = true;
                });
                // (mixed) NEXT TO A ROW WHOSE THREE AXES ARE ONE DragFloat3, so it cannot sit on
                // just the disagreeing axis the way a per-field marker would -- SameLine plus a
                // hover tooltip is the same "flag it, explain it on hover" idiom the IOR/F0
                // consistency check above (materialPanel) already uses for the same reason.
                const auto showMixedTag = [&](const bool m[3]) {
                    if (!(m[0] || m[1] || m[2])) return;
                    ImGui::SameLine();
                    ImGui::TextDisabled("(mixed)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("X %s   Y %s   Z %s across the selection.\n"
                                          "Editing a field sets that value on every selected entity.",
                                          m[0] ? "mixed" : "matches",
                                          m[1] ? "mixed" : "matches",
                                          m[2] ? "mixed" : "matches");
                };

                bool moved = ImGui::DragFloat3("Location (cm)", &xf.position.x, 1.0f);
                if (ImGui::IsItemActivated()) beginTransformEdit();
                bool done = ImGui::IsItemDeactivatedAfterEdit();
                showMixedTag(posMixed);
                if (ImGui::DragFloat3("Rotation", &euler.x, 1.0f)) {
                    xf.rotation = quatFromEulerDeg(euler); moved = true;
                }
                if (ImGui::IsItemActivated()) beginTransformEdit();
                done = done || ImGui::IsItemDeactivatedAfterEdit();
                showMixedTag(rotMixed);
                moved |= ImGui::DragFloat3("Scale", &xf.scale.x, 0.5f, 0.01f, 100000.0f);
                if (ImGui::IsItemActivated()) beginTransformEdit();
                done = done || ImGui::IsItemDeactivatedAfterEdit();
                showMixedTag(scaleMixed);

                if (moved) {
                    // TOUCHED, not "nonzero": the axis (or axes) a widget above actually wrote
                    // this frame, found the same way the mixed check above found disagreement --
                    // by comparing against the pre-edit reading.
                    const bool touchPos[3]   = {xf.position.x != origPos.x,
                                                xf.position.y != origPos.y,
                                                xf.position.z != origPos.z};
                    const bool touchRot[3]   = {euler.x != origEuler.x,
                                                euler.y != origEuler.y,
                                                euler.z != origEuler.z};
                    const bool touchScale[3] = {xf.scale.x != origScale.x,
                                                xf.scale.y != origScale.y,
                                                xf.scale.z != origScale.z};
                    w.setLocalTransform(selEntity_, xf);
                    forEachMultiMoved([&](scene::Entity ent, const Transform& oxf) {
                        Transform t = oxf;
                        if (touchPos[0])   t.position.x = xf.position.x;
                        if (touchPos[1])   t.position.y = xf.position.y;
                        if (touchPos[2])   t.position.z = xf.position.z;
                        if (touchRot[0] || touchRot[1] || touchRot[2]) {
                            Vec3 oe = eulerDegFromQuat(oxf.rotation);
                            if (touchRot[0]) oe.x = euler.x;
                            if (touchRot[1]) oe.y = euler.y;
                            if (touchRot[2]) oe.z = euler.z;
                            t.rotation = quatFromEulerDeg(oe);
                        }
                        if (touchScale[0]) t.scale.x = xf.scale.x;
                        if (touchScale[1]) t.scale.y = xf.scale.y;
                        if (touchScale[2]) t.scale.z = xf.scale.z;
                        w.setLocalTransform(ent, t);
                    });
                }
                if (done) endTransformEdit();
              }
            }
        }
        if (auto* mr = w.component<scene::CMeshRenderer>(selEntity_, scene::kComponentMeshRenderer)) {
            if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool vis = (mr->flags & scene::kMeshRendererVisible) != 0;
                if (ImGui::Checkbox("Visible", &vis)) {
                    if (vis) mr->flags |=  scene::kMeshRendererVisible;
                    else     mr->flags &= ~scene::kMeshRendererVisible;
                }
                // A SESSION-ONLY HIDE, AND THE TOOLTIP SAYS SO. No placement record carries
                // visibility (OcWorldPlacement), and both level loaders set kMeshRendererVisible on
                // every mesh they instantiate, so a hidden mesh comes back visible on reopen whether
                // or not the level was saved. That is also why this toggle neither pushes an undo
                // entry nor marks the level unsaved: dirtying the document would prompt a save that
                // cannot keep the change.
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hides this mesh for the current session only.\n"
                                      "Visibility is not saved with the level.");
                // THE FIELD IS NOW REASSIGNABLE. It printed this hex id and offered nothing --
                // no picker, and not even a drop target: the only mesh drag-drop in the editor
                // lands on the 3D VIEWPORT and SPAWNS A NEW ENTITY, which is a different verb.
                // So there was no way, anywhere, to point an existing entity at another mesh.
                const auto meshIt = meshPathById_.find(mr->mesh);
                ImGui::TextDisabled("mesh  %s", meshIt == meshPathById_.end()
                                                    ? "(unloaded)" : meshIt->second.c_str());
                if (ImGui::Button("Change Mesh...")) ImGui::OpenPopup("##pickMesh");
                uiReg_.track("details.mesh.pick");
                {
                    // CANDIDATES ARE THE MESHES ACTUALLY LOADED, from the map populated for
                    // exactly this ("the foliage palette, a future asset picker"). Listing every
                    // .ocmesh in the content index instead would offer meshes the device has
                    // refused or that were never loaded, and picking one would blank the entity.
                    std::vector<AssetChoice> cands;
                    cands.reserve(meshPathById_.size());
                    for (const auto& kv : meshPathById_) cands.push_back({kv.second, kv.first});
                    std::sort(cands.begin(), cands.end(),
                              [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                    u64 picked = 0;
                    if (assetPicker("##pickMesh", cands, mr->mesh, &picked)) {
                        if (multiSelected.size() <= 1) {
                            assignMeshId(selEntity_, picked);
                        } else {
                            // APPLIES TO THE WHOLE SELECTION, same as Transform above -- pick a
                            // mesh once and every selected entity that HAS a CMeshRenderer takes
                            // it, not just the anchor whose slot the picker read from.
                            // assignMeshId is already a per-entity, all-or-nothing call (false and
                            // a no-op for anything without the component), so this is that same
                            // call in a loop rather than a second implementation of it.
                            int n = 0;
                            for (const scene::Entity ent : multiSelected)
                                if (assignMeshId(ent, picked)) ++n;
                            if (n > 1) {
                                const auto mit = meshPathById_.find(picked);
                                cbStatus_ = "Assigned " +
                                    (mit == meshPathById_.end()
                                         ? std::string("mesh")
                                         : std::filesystem::path(mit->second).filename().string()) +
                                    " to " + std::to_string(n) + " entities";
                            }
                        }
                    }
                }
            }

            // THE MATERIAL, which a scene entity's Details has never offered. The scene stores a
            // NAME TOKEN, not a material handle -- content_'s surface materials map between them,
            // populated when the project's .ocmat files load -- so this is the one lookup that
            // stood between the panel and the entities anybody actually edits.
            //
            // Says WHICH surface by name, because the desc behind it is SHARED: every entity
            // using M_Floor is looking at these same sliders, and editing them here moves all of
            // them. That is the material system working as designed, and it is the sort of thing
            // a panel should say out loud rather than let somebody discover.
#if AVER_MODULE_PBR
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                // THE SAME FALLBACK THE DRAW USES. This panel reads mr->material directly, so
                // without this it would report "(none)" for an entity the renderer is happily
                // drawing with the mesh's own material -- a panel disagreeing with the picture.
                const i32 shown = mr->material ? mr->material : content_.meshDefaultMaterial(mr->mesh);
                const char* surfaceName = aver_scene_material_name(shown);

                // A MATERIAL COULD NOT BE ASSIGNED AT ALL BEFORE THIS -- not by picker, and not
                // by drag either, because isPlaceableAssetExt refuses to start a drag on a
                // .ocmat. The panel could edit the parameters of whatever material the mesh's own
                // slot already named, and nothing could change WHICH material that was.
                if (ImGui::Button("Change Material...")) ImGui::OpenPopup("##pickMaterial");
                uiReg_.track("details.material.pick");
                {
                    // TOKENS, NOT PATH HASHES. content_'s surface materials map is keyed on the
                    // interned name token, which is what mr->material holds; an fnv1a64 here would
                    // resolve to nothing or, worse, to an unrelated surface by coincidence.
                    std::vector<AssetChoice> cands;
                    cands.reserve(content_.surfaceMaterials().size());
                    for (const auto& kv : content_.surfaceMaterials()) {
                        if (!kv.second) continue;   // interned but no .ocmat loaded behind it
                        // `matName`, not `nm`: this Details branch's own entity-name `nm` (set
                        // above, from selEntity_) is still in scope here, and this shadowed it
                        // (C4456) despite naming an unrelated thing -- a MATERIAL's display name.
                        const char* matName = aver_scene_material_name(kv.first);
                        cands.push_back({matName && *matName ? matName : "(unnamed)",
                                         static_cast<u64>(static_cast<u32>(kv.first))});
                    }
                    std::sort(cands.begin(), cands.end(),
                              [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                    u64 picked = 0;
                    if (assetPicker("##pickMaterial", cands,
                                    static_cast<u64>(static_cast<u32>(shown)), &picked)) {
                        if (multiSelected.size() <= 1) {
                            assignMaterialToken(selEntity_, static_cast<i32>(static_cast<u32>(picked)));
                        } else {
                            // Same shape as the mesh picker just above: one pick, applied to
                            // every selected entity that has a CMeshRenderer.
                            int n = 0;
                            for (const scene::Entity ent : multiSelected)
                                if (assignMaterialToken(ent, static_cast<i32>(static_cast<u32>(picked)))) ++n;
                            if (n > 1) {
                                const char* pickedName =
                                    aver_scene_material_name(static_cast<i32>(static_cast<u32>(picked)));
                                cbStatus_ = std::string("Assigned surface ") +
                                    (pickedName && *pickedName ? pickedName : "(unnamed)") +
                                    " to " + std::to_string(n) + " entities";
                            }
                        }
                    }
                }
                const pbr::MaterialHandle authored = content_.authoredFor(shown);
                if (!authored) {
                    ImGui::TextDisabled("Surface '%s' has no .ocmat loaded.",
                                        surfaceName && *surfaceName ? surfaceName : "(none)");
                    ImGui::TextDisabled("Drawing with the flat fallback; author one under Content\\Materials.");
                } else {
                    ImGui::TextDisabled("Surface  %s", surfaceName && *surfaceName ? surfaceName : "(unnamed)");
                    ImGui::TextDisabled("Shared: editing this changes every entity using it.");
                    materialPanel(authored);
                }
            }
#endif
        }
#if AVER_MODULE_PARTICLES
        // The authoring surface DECIDED components need: visible and editable the same way
        // CMeshRenderer just above is. No picker widget beyond drag-drop exists for
        // CMeshRenderer::mesh either, so an emitter's own effect id follows that same shape.
        if (auto* pe = w.component<scene::CParticleEmitter>(selEntity_, scene::kComponentParticleEmitter)) {
            if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool stopped = (pe->flags & scene::kParticleEmitterStopped) != 0;
                if (ImGui::Checkbox("Stopped", &stopped)) {
                    if (stopped) pe->flags |=  scene::kParticleEmitterStopped;
                    else         pe->flags &= ~scene::kParticleEmitterStopped;
                }
                ImGui::TextDisabled("effect id 0x%llx", (unsigned long long)pe->effect);
                // Drop a .ocparticle from the Content Browser directly onto this row to point this
                // emitter at it -- the SAME id space content_.loadProjectParticleEffects() populates, so a
                // freshly authored effect resolves the moment it lands here.
                // THROUGH THE SHARED HELPER NOW. The resolve/validate/hash/assign/mark/log
                // sequence used to be written out here, and the picker below would have been a
                // second copy of it -- the "two implementations of one fact" shape this file
                // keeps paying for. assignParticleEffect owns it; both callers just hand it a path.
                // THE BROWSER'S ONE PAYLOAD. The Content Browser sends its whole selection under
                // kCbMoveDragDropType (ImGui keeps exactly one payload per drag, see the viewport
                // drop target), so this row takes the first .ocparticle in it. kAssetDragDropType
                // is still accepted from any source that sends a single asset.
                if (ImGui::BeginDragDropTarget()) {
                    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kCbMoveDragDropType);
                    if (!payload) payload = ImGui::AcceptDragDropPayload(kAssetDragDropType);
                    if (payload && payload->Data) {
                        const std::string blob(
                            static_cast<const char*>(payload->Data),
                            payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
                        bool assigned = false;
                        for (const std::string& one : editor::splitDropPayload(blob)) {
                            if (lowerExt(std::filesystem::path(one)) != ".ocparticle") continue;
                            assignParticleEffect(selEntity_, one);
                            assigned = true;
                            break;
                        }
                        if (!assigned) cbStatus_ = "Only a .ocparticle effect can be dropped on this emitter";
                    }
                    ImGui::EndDragDropTarget();
                }
                // And the same assignment without needing the Content Browser open at all.
                if (ImGui::Button("Change Effect...")) ImGui::OpenPopup("##pickEffect");
                uiReg_.track("details.effect.pick");
                {
                    // CANDIDATES COME FROM THE CONTENT INDEX, not from a loaded-effects map:
                    // unlike a mesh, an effect is resolved when it is ASSIGNED (so a freshly
                    // authored one works the moment it lands), so listing only already-loaded
                    // effects would hide exactly the file the author just made.
                    // content_.index() MAPS id -> ABSOLUTE path, not relative, and an earlier draft
                    // of this comment said relative. Its KEY is fnv1a64 of the project-relative
                    // spelling -- the same id space pe->effect holds, so the listed id is the
                    // effect id and needs no hashing here -- but its VALUE is
                    // `it->path().string()` straight off the directory iterator (content_.adopt()).
                    // The two halves genuinely disagree, which is why the
                    // label has to be derived rather than used as-is.
                    //
                    // LABELLED RELATIVE, so this popup reads like the Mesh and Material ones a few
                    // rows above instead of showing the developer's whole local directory tree.
                    std::vector<AssetChoice> cands;
                    for (const auto& kv : content_.index()) {
                        if (lowerExt(std::filesystem::path(kv.second)) != ".ocparticle") continue;
                        cands.push_back({cbRelativeToContent(kv.second), kv.first});
                    }
                    std::sort(cands.begin(), cands.end(),
                              [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                    u64 picked = 0;
                    if (assetPicker("##pickEffect", cands, pe->effect, &picked)) {
                        // ASSIGNED BY PATH, not by writing the id straight in, so it goes through
                        // the identical resolve/validate the drop does -- including the extension
                        // check and the project-relative normalisation. Writing pe->effect here
                        // would be the second implementation this extraction exists to avoid.
                        // PASSED STRAIGHT THROUGH, because content_.pathFor()'s value is ALREADY
                        // absolute. The previous line joined it onto contentDir() first, which
                        // happened to produce the right answer only because std::filesystem's
                        // operator/ DISCARDS the left side when the right is absolute -- correct
                        // by accident, and silently wrong the day that map starts storing
                        // relative paths, which its own key spelling suggests it should.
                        const std::string path = content_.pathFor(picked);
                        if (!path.empty())
                            assignParticleEffect(selEntity_, path);
                    }
                }
                ImGui::TextDisabled("age %.2fs   seed 0x%08x", pe->age, pe->seed);
            }
        }
#endif

#if AVER_MODULE_PHYSICS
        // ---- Soft Body, and the physics viewer for it ----
        // A PANEL THAT READS THE SIMULATION, not just the component: the component says what was
        // authored, but is it simulating, how many particles, did it take the skinned path, is it
        // being drawn are answerable only from the live body and render feature.
        if (auto* sb = w.component<scene::CSoftBody>(selEntity_, scene::kComponentSoftBody)) {
            if (ImGui::CollapsingHeader(ICON_TUNE " Soft Body", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool disabled = (sb->flags & scene::kSoftBodyDisabled) != 0;
                if (ImGui::Checkbox("Simulated", &disabled)) {
                    // The checkbox reads POSITIVELY and the flag is stored negatively -- see
                    // kSoftBodyDisabled's own comment for why the flag has to mean that.
                    if (disabled) sb->flags &= ~scene::kSoftBodyDisabled;
                    else          sb->flags |=  scene::kSoftBodyDisabled;
                }
                // Shown inverted from the member, so the label is the state rather than its negation.
                disabled = (sb->flags & scene::kSoftBodyDisabled) == 0;

                f32 maxDist = scene::softBodyMaxDistanceCm(*sb);
                if (ImGui::DragFloat("Max distance (cm)", &maxDist, 0.1f, 0.0f, 500.0f, "%.2f"))
                    sb->maxDistanceCm = maxDist;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far a vertex may leave where bone skinning would have\n"
                                      "put it. 0 reads as the default (%.0f cm), not as pinned.\n"
                                      "Only reaches the solver on a mesh that HAS a rig.",
                                      static_cast<double>(scene::kSoftBodyDefaultMaxDistanceCm));

                ImGui::DragFloat("Compliance", &sb->compliance, 0.0001f, 0.0f, 1.0f, "%.4f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Inverse stiffness of the edge constraints.\n0 is inextensible.");

                ImGui::SeparatorText("Simulation");
                // STRAIGHT FROM THE PHYSICS ABI, not from the component. `body` is the handle the
                // renderer stored; everything below is asked of Jolt this frame.
                if (sb->body == 0) {
                    ImGui::TextDisabled(ICON_WARNING " no body yet");
                    ImGui::TextWrapped("A body is created on the first frame this entity is drawn "
                                       "with a mesh whose asset has resolved. If it never appears, "
                                       "the mesh has no triangles, or this build has no soft-body "
                                       "render feature.");
                } else {
                    const i32 particles = aver_phys_softbody_vertex_count(sb->body);
                    ImGui::Text("Body %d   %d particle%s", sb->body, particles,
                                particles == 1 ? "" : "s");
                    // THE ANSWER TO "WHY IS IT NOT MOVING". A body exists and reports zero
                    // particles only when Jolt took the mesh and found nothing to simulate.
                    if (particles == 0)
                        ImGui::TextDisabled(ICON_WARNING " the body has no particles");

                    // Where its particles actually are, so "is it simulating" is answerable
                    // without a debugger: a body at rest and a body falling look identical in
                    // every other readout here.
                    std::vector<f32> pos(static_cast<usize>(particles) * 3, 0.0f);
                    const i32 got = particles > 0
                        ? aver_phys_softbody_vertices(sb->body, pos.data(), particles) : 0;
                    if (got > 0) {
                        f32 lo[3] = { 1e30f,  1e30f,  1e30f};
                        f32 hi[3] = {-1e30f, -1e30f, -1e30f};
                        for (i32 v = 0; v < got; ++v)
                            for (int c = 0; c < 3; ++c) {
                                const f32 x = pos[static_cast<usize>(v) * 3 + c];
                                lo[c] = x < lo[c] ? x : lo[c];
                                hi[c] = x > hi[c] ? x : hi[c];
                            }
                        ImGui::TextDisabled("bounds  x %.0f..%.0f   y %.0f..%.0f   z %.0f..%.0f",
                                            lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
                        ImGui::TextDisabled("extent  %.0f x %.0f x %.0f cm",
                                            hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]);
                    }
                }

#if AVER_MODULE_RENDER_SOFTBODY
                // IS IT ACTUALLY BEING DRAWN? Simulating and drawing are separate failures with
                // identical symptoms from the viewport, and this is the one line that tells them
                // apart: a non-zero draw handle means the scene pass substitutes simulated vertices for the authored mesh.
                const bool drawn = softBodyScene_ && softBodyScene_->drawHandle(selEntity_) != 0;
                ImGui::TextDisabled("%s drawn from the simulation: %s",
                                    drawn ? ICON_VISIBILITY : ICON_WARNING, drawn ? "yes" : "no");
                if (softBodyScene_)
                    ImGui::TextDisabled("%u resident, %u simulated last frame",
                                        softBodyScene_->residentCount(),
                                        softBodyScene_->simulatedLastFrame());
#else
                ImGui::TextDisabled(ICON_WARNING " this build has no soft-body render feature, "
                                    "so the simulation is not drawn");
#endif
            }
        }
#endif

        // ---- Add Component ----
        // THERE WAS NO WAY TO ADD A COMPONENT FROM THE EDITOR AT ALL before this: every panel
        // above renders only when its component is ALREADY on the entity, so one no importer or
        // template attaches was unreachable without hand-editing a level file.
        ImGui::Separator();
        if (ImGui::Button(ICON_ADD " Add Component")) ImGui::OpenPopup("addComponent");
        if (ImGui::BeginPopup("addComponent")) {
            // Listed by hand rather than walked from the component registry, deliberately: the
            // registry knows every component's NAME and SIZE, nothing about whether attaching one
            // from a menu is meaningful (CWorld/CLocal are written by the transform pass and would
            // corrupt an entity by hand). An allow-list is the same information, kept visible.
            struct Addable { u32 id; const char* name; const char* tip; };
            static const Addable kAddable[] = {
                {scene::kComponentSoftBody, ICON_TUNE " Soft Body",
                 "Simulate this entity's mesh instead of posing it: sag, drape, squash, collide."},
            };
            for (const Addable& a : kAddable) {
                const bool present = w.hasComponent(selEntity_, a.id);
                ImGui::BeginDisabled(present);
                if (ImGui::MenuItem(a.name)) {
                    // addComponent hands back ZERO-FILLED storage and never runs a constructor,
                    // so the defaults are written over it here -- the same fix World::create()
                    // makes, and the bug CSynapseAgent already paid for once.
                    if (a.id == scene::kComponentSoftBody) {
                        if (auto* c = static_cast<scene::CSoftBody*>(
                                w.addComponent(selEntity_, a.id)))
                            *c = scene::CSoftBody{};
                    }
                    cbStatus_ = std::string("Added ") + a.name;
                    // NOT UNDOABLE, SO IT MUST AT LEAST BE DIRTY. Adding a component pushes no
                    // EditCmd (there is no Kind for it), so without this the level closed clean
                    // and the component was gone. A prompt the author can answer beats a silent
                    // loss. Remove Component, just below, does not have this problem: it captures
                    // the whole component before dropping it, so it can afford to be undoable
                    // instead.
                    markLevelUnsaved();
                }
                ImGui::EndDisabled();
                if (present && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("This entity already has one.");
                else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", a.tip);
            }
            ImGui::EndPopup();
        }

        // ---- Remove Component ----
        // scene::World::removeComponent is implemented and tested (tests/scene/src/SceneTest.cpp)
        // but had zero editor callers -- Add Component's own comment above used to say so outright.
        // kRemovable lists exactly what kAddable offers, for the same reason kAddable is an
        // allow-list rather than a walk of the component registry: CLocal/CWorld/CHierarchy are
        // what makes the row an entity at all, and removing one by menu would corrupt it, not
        // simplify it.
        ImGui::SameLine();
        if (ImGui::Button(ICON_DELETE " Remove Component")) ImGui::OpenPopup("removeComponent");
        if (ImGui::BeginPopup("removeComponent")) {
            struct Removable { u32 id; const char* name; };
            static const Removable kRemovable[] = {
                {scene::kComponentSoftBody, ICON_TUNE " Soft Body"},
            };
            bool anyPresent = false;
            for (const Removable& r : kRemovable) {
                const bool present = w.hasComponent(selEntity_, r.id);
                anyPresent = anyPresent || present;
                ImGui::BeginDisabled(!present);
                if (ImGui::MenuItem(r.name)) {
                    // UNDOABLE: removeComponentFromSelection captures the component byte-exact
                    // before dropping it, so undo puts back exactly what was there -- unlike Add
                    // Component above, which has no "before" to capture and falls back to a bare
                    // markLevelUnsaved().
                    if (removeComponentFromSelection(r.id))
                        cbStatus_ = std::string("Removed ") + r.name;
                }
                ImGui::EndDisabled();
                if (!present && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("This entity does not have one.");
            }
            if (!anyPresent) ImGui::TextDisabled("Nothing removable is attached.");
            ImGui::EndPopup();
        }
#endif
    } else if (sel_==-2){
        ImGui::TextUnformatted("Directional Light (Sun)"); ImGui::Separator();
        {
            f32 elev = 0.0f, azim = 0.0f;
            sky_.sunAngles(elev, azim);
            bool moved = ImGui::SliderFloat("Elevation", &elev, -20.0f, 90.0f, "%.1f deg");
            moved |= ImGui::SliderFloat("Azimuth", &azim, -180.0f, 180.0f, "%.1f deg");
            // Written back only on a real move: the angle round trip is lossy.
            if (moved) sky_.setSunAngles(elev, azim);
        }
        ImGui::SliderFloat("Intensity", &sky_.sunIntensity, 0.0f, 8.0f, "%.2f");
        bool useTemp = sky_.sunTemperatureK > 0.0f;
        if (ImGui::Checkbox("Use Colour Temperature", &useTemp))
            sky_.sunTemperatureK = useTemp ? 5500.0f : 0.0f;
        if (useTemp) {
            ImGui::SliderFloat("Temperature", &sky_.sunTemperatureK, 1500.0f, 12000.0f, "%.0f K");
        } else {
            ImGui::ColorEdit3("Colour", sunColor_);
        }
        ImGui::SliderFloat("Angular Size", &sky_.sunAngularDiameterDeg, 0.05f, 8.0f, "%.2f deg");
        markLevelRecordEdited(hasLevelSun_);
    } else if (sel_==-3){
        ImGui::TextUnformatted("Sky + Atmosphere"); ImGui::Separator();
        {
            int model = sky_.model == rhi::SkyModel::Physical ? 1 : 0;
            const char* names[] = {"Authored (two-colour dome)", "Physical (Rayleigh + Mie + ozone)"};
            if (ImGui::Combo("Sky Model", &model, names, 2))
                sky_.model = model ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Physical derives the dome below from the sun's elevation.\n"
                                  "Sunset stops being a colour somebody picked and becomes geometry.");
        }
        const bool physicalSky = sky_.model == rhi::SkyModel::Physical;

        ImGui::BeginDisabled(physicalSky);
        ImGui::ColorEdit3("Zenith", skyZenith_); ImGui::ColorEdit3("Horizon", skyHorizon_);
        ImGui::SliderFloat("Atmosphere Height", &sky_.atmosphereHeight, 0.05f, 4.0f, "%.2f");
        ImGui::EndDisabled();
        if (physicalSky) {
            ImGui::TextDisabled("^ derived from the sun's elevation while the model is Physical");
            if (ImGui::TreeNode("Air")) {
                ImGui::SliderFloat("Mie Scatter", &sky_.air.mieScatter, 0.0f, 4e-2f, "%.5f /km",
                                   ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Aerosol. Raise it for haze, dust or a coastal day; the\n"
                                      "sun's halo and the pale band at the horizon both grow.");
                ImGui::SliderFloat("Mie Extinction", &sky_.air.mieExtinction, 0.0f, 4e-2f, "%.5f /km",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Mie Anisotropy", &sky_.air.miePhaseG, 0.0f, 0.95f, "%.2f");
                ImGui::SliderFloat("Rayleigh Height", &sky_.air.rayleighScaleKm, 1.0f, 20.0f, "%.1f km");
                ImGui::SliderFloat("Mie Height", &sky_.air.mieScaleKm, 0.2f, 8.0f, "%.2f km");
                ImGui::SliderFloat("Multi-Scatter", &sky_.air.multiScatterGain, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stands in for every bounce after the first. 1.70 is\n"
                                      "calibrated: it puts diffuse light at 15-30%% of direct\n"
                                      "and the zenith's blue/red between 2.5 and 5.");
                ImGui::SliderFloat("Planet Radius", &sky_.air.planetRadiusKm, 100.0f, 20000.0f, "%.0f km");
                ImGui::SliderFloat("Air Depth", &sky_.air.atmosphereHeightKm, 5.0f, 200.0f, "%.0f km");
                ImGui::SliderInt("Sky Steps", &sky_.air.viewSteps, 4, 96);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Samples along a sky ray. 32 is within 3%% of a converged\n"
                                      "march; below about 16 the horizon starts to band.");
                ImGui::SliderInt("Aerial Steps", &sky_.air.aerialSteps, 1, 16);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Samples along the air between the camera and a surface.\n"
                                      "This one runs per shaded pixel, so it is the expensive one.");
                ImGui::TreePop();
            }
        }
        ImGui::ColorEdit3("Ground", sky_.groundAlbedo);
        ImGui::SliderFloat("Ground Blend", &sky_.groundBlend, 0.0f, 1.0f, "%.2f");
        // RANGE 0..32, LOGARITHMIC, AND IT USED TO STOP AT 2.
        //
        // This is the multiplier on the only term that fills an enclosed space -- sky light
        // through the openings -- and an arcade needs far more of it than a slider that stops at
        // 2 can express. MEASURED on PTTest Sponza at exposure 1, luminance percentiles of the
        // 3D viewport, sunlit surfaces reaching 237 in every row:
        //
        //   Sky Light  1    p50   8   p90 48    <- the median pixel is essentially black
        //   Sky Light  4    p50  15   p90 48
        //   Sky Light 16    p50  34   p90 75    <- shadows readable, sun unchanged
        //
        // The sun is correctly scaled: a directly lit surface reaches 237/255 with no exposure
        // at all. What was missing was the fill, and the control for it could not reach the
        // value the scene wanted -- so "everything is too dark" was, in part, unauthorable.
        // Logarithmic because the useful range spans two orders of magnitude and the interesting
        // end is the bottom.
        ImGui::SliderFloat("Sky Light", &sunAmbient_, 0.0f, 32.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);

        ImGui::Separator();
        ImGui::TextUnformatted("Height Fog");
        ImGui::ColorEdit3("Fog Tint", fogColor_);
        ImGui::SliderFloat("Fog Density", &fogDensity_, 1e-7f, 1e-3f, "%.2e",
                           ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extinction per cm. Visibility = 3.912 / density.\n"
                              "2e-6 clear (20 km)   4e-6 light haze (10 km)\n"
                              "8e-6 haze (5 km)     8e-5 fog (500 m)");
        ImGui::SliderFloat("Height Falloff", &sky_.fogFalloff, 0.0f, 0.02f, "%.5f",
                           ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = uniform distance fog (the old behaviour)");
        ImGui::DragFloat("Fog Height", &sky_.fogHeight, 1.0f);
        ImGui::DragFloat("Fog Start", &sky_.fogStart, 1.0f, 0.0f, 1e6f);
        ImGui::SliderFloat("Max Opacity", &sky_.fogMaxOpacity, 0.0f, 1.0f, "%.2f");
        // FOG IS ITS OWN RECORD, so it needs its own mark. markLevelRecordEdited went in
        // for SUN and SKY and this was missed, which left Fog Tint and Fog Density still
        // silently discarded on a level whose file carries no FOG line.
        markLevelRecordEdited(hasLevelFog_);
#if AVER_MODULE_SCENE
        if (chunkWorld_) {
            const world::StreamSettings& mst = chunkWorld_->settings().stream;
            const f32 boundaryCm = static_cast<f32>(mst.loadRadius) * static_cast<f32>(mst.chunkSizeCm);
            ImGui::Checkbox("Match Fog To Streaming Radius", &matchFogToStreamRadius_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Recomputes Fog Density every frame -- overriding the slider\n"
                                  "above -- so opacity AT the load boundary (%.0fcm here) equals\n"
                                  "the target below. NOT a free win: hiding a boundary this close\n"
                                  "typically needs an order of magnitude more density than a\n"
                                  "level's authored default, i.e. a visibly foggier world.", boundaryCm);
            if (matchFogToStreamRadius_) {
                ImGui::SliderFloat("Target Opacity At Boundary", &fogMatchTargetOpacity_, 0.5f, 0.99f, "%.2f");
                const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
                ImGui::TextDisabled("boundary %.0fcm -> density %.2e", boundaryCm, matched);
            }
        }
#endif

        ImGui::Separator();
        ImGui::TextUnformatted("Volumetric Clouds");
        ImGui::Checkbox("Clouds", &sky_.cloudsEnabled);
        if (sky_.cloudsEnabled) {
            ImGui::SliderFloat("Coverage", &sky_.cloudCoverage, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Density", &sky_.cloudDensity, 0.0f, 4.0f, "%.2f");
            ImGui::DragFloat("Layer Bottom", &sky_.cloudBottom, 100.0f);
            ImGui::DragFloat("Layer Top", &sky_.cloudTop, 100.0f);
            f32 featureSize = sky_.cloudScale > 1e-9f ? 1.0f / sky_.cloudScale : 50000.0f;
            if (ImGui::DragFloat("Feature Size", &featureSize, 100.0f, 100.0f, 5e6f))
                sky_.cloudScale = 1.0f / std::fmax(featureSize, 1.0f);
            ImGui::DragFloat2("Wind", sky_.cloudWind, 5.0f);
        }
        // CLOUDS IS ITS OWN RECORD, so it needs its own mark, for the same reason FOG did. The
        // Clouds checkbox is above the `if`, deliberately: turning clouds OFF is an edit that
        // has to reach the file, and marking only inside the enabled branch would make "off"
        // the one cloud setting that could never be saved.
        markLevelRecordEdited(hasLevelClouds_);
        markLevelRecordEdited(hasLevelSky_);
        // WATER LIVES BESIDE THE SKY, not in a mode of its own: it is a property OF THE LEVEL,
        // exactly like the sun and the fog above it, and the panel a person already opens to set
        // the weather is where they will look for it. No markLevelRecordEdited -- unlike SUN and
        // FOG, waters ride through saveLevel as part of levelHeader_ rather than through a
        // has-flag, so editing the vector IS the edit.
        ImGui::Separator();
        buildWaterPanel(e);
    } else if (sel_==-4){
        ImGui::TextUnformatted("Post Process"); ImGui::Separator();
        ImGui::BeginDisabled(post_.autoExposure);
        ImGui::SliderFloat("Exposure", &post_.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        ImGui::Checkbox("Auto Exposure", &post_.autoExposure);
        if (post_.autoExposure) {
            ImGui::SliderFloat("Middle Grey", &post_.exposureKey, 0.02f, 0.6f, "%.3f");
            ImGui::SliderFloat("Adapt Speed", &post_.exposureSpeed, 0.1f, 20.0f, "%.1f/s");
            ImGui::SliderFloat("Exposure Min", &post_.exposureMin, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Exposure Max", &post_.exposureMax, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        }
        ImGui::Separator();
        ImGui::SliderFloat("Bloom", &post_.bloomIntensity, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zero skips the whole bloom pyramid, not just its weight");
        if (post_.bloomIntensity > 0.0f) {
            ImGui::SliderFloat("Threshold", &post_.bloomThreshold, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("Knee", &post_.bloomKnee, 0.0f, 2.0f, "%.2f");
        }
    } else ImGui::TextDisabled("Select an actor in the World Outliner");
    ImGui::End();

}

#endif

#if AVER_MODULE_SCENE
#if AVER_WITH_IMGUI
// A pure-ImGui debug window. Guarded because uiActive() is a RUNTIME test and cannot make the
// ImGui:: names exist for the compiler -- see the mouse-capture block in onUpdate for the same trap.
void SandboxApp::buildChunkStreamingPanel() {
    if (!chunkWorld_) return;
    const world::StreamStats& s = chunkStreamStats_;
    // ANCHORED TO THE VIEWPORT, not the window's top-left corner. It used to sit at a fixed
    // (12, 60) from the window origin, the viewport's corner too until a docked panel appeared on
    // the left, after which this overlay covered the mode panel's tool buttons.
    ImGui::SetNextWindowPos(ImVec2(vpX_ + 12.0f * dpi_, vpY_ + 60.0f * dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (!ImGui::Begin("Chunk Streaming", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    ImGui::Text("resident:  %u chunks / %u entities", s.residentChunks, s.residentEntities);
    ImGui::Text("this frame: +%u loaded, -%u evicted", s.loadedThisUpdate, s.evictedThisUpdate);
    ImGui::Text("in flight, %u/%u", s.pendingLoads, s.failedLoads);
    ImGui::SameLine();
    ImGui::TextDisabled("(pending / failed)");
    if (s.failedLoads > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "!");
    }
    ImGui::Text("total loads: %u   avg load: %.2f ms", s.totalLoads,
                s.totalLoads > 0 ? s.totalLoadMs / static_cast<f64>(s.totalLoads) : 0.0);
    ImGui::Text("last load: %.2f ms", s.lastLoadMs);
    ImGui::Text("resident triangles: %llu", static_cast<unsigned long long>(residentTriangleCount()));
    ImGui::Separator();
    ImGui::TextDisabled("%s", chunkWorld_->settings().worldDir.c_str());
    ImGui::End();
}

#endif
#endif

} // namespace aver
