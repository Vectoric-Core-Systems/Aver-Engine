// Editor: material and mode panels, World Outliner, Details panel, asset assignment.
// Split out of the 29,952-line SandboxApp.cpp on 2026-09-16 (method bodies moved verbatim); the
// class is declared in SandboxApp.hpp.

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

        // NEVER A SILENT TRUNCATION (same rule as the node palette): a capped list that just
        // stops reads as "no such asset", the wrong thing to learn from a full box.
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
// Extracted so the drag-drop target and the picker share one resolve/hash/assign/mark/log
// implementation instead of two ("two implementations of one fact" -- a recurring shape here).
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

// A mesh id IS fnv1a64 of the project-relative path (what meshPathById_ is keyed on), so a picked
// id needs no conversion, unlike the material below.
bool SandboxApp::assignMeshId(scene::Entity ent, u64 meshId) {
    scene::World& w = scene::World::instance();
    auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
    if (!mr) return false;
    mr->mesh = meshId;
    // WITHOUT THIS THE PICTURE DOES NOT CHANGE: the GPU path only re-uploads when dirty is set
    // (same fix EditorEntitySnapshot applies when restoring a mesh renderer).
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

// MATERIAL IDENTITY IS AN INTERNED NAME TOKEN, not a path hash -- an fnv1a64 here would resolve
// to nothing, or by coincidence to an unrelated surface, and fail silently. content_'s surface
// materials are keyed on the token aver_scene_material() interns, so the picker carries tokens.
bool SandboxApp::assignMaterialToken(scene::Entity ent, i32 token) {
    scene::World& w = scene::World::instance();
    auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
    if (!mr) return false;
    mr->material = token;
    mr->dirty = 1;
    markLevelUnsaved();
#if AVER_MODULE_PBR
    // ON-DEMAND LOAD: the Details material picker below now lists every project material stem
    // (editor::projectMaterialStems), not only whichever ones level-scoped residency currently keeps
    // resident, so a pick can name a surface nothing has bound yet -- content_.authoredFor(token)
    // would otherwise keep reporting it unbound until some later level happened to need it too.
    if (!content_.authoredFor(token)) {
        const char* nameForLoad = aver_scene_material_name(token);
        if (nameForLoad && *nameForLoad) {
            const pbr::MaterialHandle h = content_.materialForSurface(nameForLoad);
            if (h) content_.bindSurfaceMaterial(token, h);
        }
    }
#endif
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
// Draws the material half of the Details panel: the shared MaterialDesc where there is one, the
// actor's own values otherwise.
// TAKES A HANDLE, not a MeshObj -- this ~25-slider panel (IOR/F0 check, texture slots, Save to
// C#) used to be reachable only from placeholder objects (a scene entity's Details offered just
// Transform and Mesh; docs/EDITOR.md:276), even though content_'s surface materials already map
// a CMeshRenderer's token straight to a handle -- only the signature stood in the way.
// GUARDED ABOVE THE SIGNATURE, not inside the body, for the same reason as saveMaterialSource
// above: pbr::MaterialHandle in the parameter list means a PBR=OFF tree can't compile this
// definition at all; SandboxApp.hpp's declaration carries the same guard.
#if AVER_MODULE_PBR
void SandboxApp::materialPanel(pbr::MaterialHandle handle) {
    pbr::MaterialDesc* d = pbr::MaterialLibrary::get().mutableDesc(handle);
    if (!d) { ImGui::TextDisabled("No material (drawing with the fallback)"); return; }
    bool changed = false;

    // ---- ONE UNDO ENTRY PER INTERACTION, not per frame ----
    // Every control here wrote straight through a MaterialDesc* via touch() with nothing ever
    // pushed (EditCmd::Kind had no Material case at all), so Ctrl+Z undid whatever came before
    // the edit, not the edit itself.
    // A DRAG IS ONE EDIT: `changed` is true every frame of a slider drag, so pushing on it would
    // put a hundred entries on the 64-deep undo stack and evict everything else the user did.
    // IsItemActivated/IsItemDeactivatedAfterEdit bracket the whole interaction instead (the same
    // shape beginTransformEdit/endTransformEdit use for the gizmo).
    // `before` is snapshotted at the top of the frame, not on the activation frame itself --
    // ImGui has already written the first drag delta into *d by the time IsItemActivated() fires.
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

    // IOR and Reflectance are the same physical fact twice; F0 = ((1-n)/(1+n))^2, so 1.52 glass
    // implies 0.0426 -- a mismatch implies a physically impossible reflectance, invisible until a
    // grazing or critical angle.
    // REPORTED, NOT ENFORCED: clamping reflectance to the IOR would remove a knob authors
    // legitimately need (a thin film or coated lens really does deviate).
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
    // Transmission is a SUBSTRATE property, applying whether or not the material is blended: it
    // scales the diffuse lobe (a transmissive surface must not also scatter its full base colour
    // back at you) and, when blended, pulls coverage toward 1 - transmission.
    changed |= track(ImGui::SliderFloat("Transmission", &d->transmission, 0.0f, 1.0f));
    if (d->transmission > 0.0f && d->alphaMode != pbr::AlphaMode::Blend) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.00f, 0.62f, 0.15f, 1.0f), "(opaque)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Transmission still dims the diffuse lobe here, but this material is\n"
                              "not BLEND translucent, so nothing will be visible THROUGH it.");
    }
    // WEIGHT is the on/off: 0 skips the wrap-diffuse/back-scatter terms, so RADIUS and COLOR have
    // nothing to widen or tint until Weight > 0 -- disabled at 0 to avoid offering controls that do
    // nothing.
    changed |= track(ImGui::SliderFloat("Subsurface Weight", &d->subsurfaceWeight, 0.0f, 1.0f));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Subsurface strength: light wraps past the shadow edge, passes through "
                          "thin parts lit from behind (leaves, curtains, ears) and through them from "
                          "the sky. 0 = off.");
    ImGui::BeginDisabled(d->subsurfaceWeight <= 0.0f);
    changed |= track(ImGui::SliderFloat("Subsurface Radius", &d->subsurfaceRadius, 0.0f, 1.0f));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How deep light scatters, 0.25-5 cm: thin parts under it glow when lit "
                          "from behind, thicker ones do not. Also widens the glow seen looking "
                          "toward the light.");
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Only does anything when Subsurface Weight is above 0.");
    changed |= track(ImGui::ColorEdit3("Subsurface Color", d->subsurfaceColor));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The colour light takes inside the material, multiplied onto the base "
                          "colour. White keeps the surface colour; skin wants a deep red, leaves "
                          "yellow-green, wax orange.");
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Only does anything when Subsurface Weight is above 0.");
    ImGui::EndDisabled();
    changed |= track(ImGui::DragFloat3("Emissive", d->emissiveFactor, 0.01f, 0.0f, 32.0f));
    changed |= track(ImGui::DragFloat("Light Intensity", &d->lightIntensity, 0.05f, 0.0f, 20.0f));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Makes this material a light source when ray tracing is on -- a MULTIPLIER on "
                          "the light Emissive and this draw's size already, physically, cast: 1 is "
                          "exactly that, 2 is twice it. 0 = not a light, coloured by Emissive "
                          "(white if none). A bounding sphere bigger than the visible bulb "
                          "over-lights; lower this or use a separate bulb mesh.");

    ImGui::Separator();
    int uvMode = static_cast<int>(d->uvMode);
    if (ImGui::Combo("UV Mapping", &uvMode, "Mesh UVs\0World Aligned\0")) {
        d->uvMode = static_cast<pbr::UvMode>(uvMode);
        changed = true;
    }
    // Tracked the same way but read AFTER the widget, outside its own if: a Combo opened and
    // dismissed without a change still activates/deactivates and must still close the bracket.
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
        // A text field is the case the bracket exists for: InputText reports `changed` per
        // keystroke, so a per-frame push would cost one undo entry per LETTER typed;
        // IsItemDeactivatedAfterEdit fires once, when focus leaves.
        track(false);
    }
    if (started) matEditActive_ = true;
    // Whether anything moved is ImGui's answer, not a diff: a memcmp is wrong (MaterialDesc holds
    // std::strings whose pointers can differ without the value differing) and a field-by-field
    // comparison goes stale the next time a control is added. Each control's own return value
    // already means "edited"; latching that across the interaction can't drift.
    if (changed) { matEditDirty_ = true; pbr::MaterialLibrary::get().touch(handle); }
    // Pushed on release: clicking a slider without moving it can still fire
    // IsItemDeactivatedAfterEdit, and a no-op entry makes Ctrl+Z visibly do nothing, once.
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
}
#endif  // AVER_MODULE_PBR

// The MODE panel: the active editing mode's own tools and settings, on the left.
// A panel, not more toolbar: a toolbar row suits four brush buttons, not the dozen controls a
// real sculpt tool set needs (radius, strength, falloff, flatten target, a 4-param noise gen) --
// previously absent or buried in a popup behind a numbered button. A panel shows its whole
// surface at once.
void SandboxApp::buildModePanel(Engine& e) {
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
        case EditorMode::Simulate:  buildSimulateModePanel(e);  break;
    }
    ImGui::End();
}

// A labelled text field backed by a std::string. True on the frame the value changed.
// ImGui edits a fixed char buffer while the backing string may be arbitrarily long, so the copy
// is bounded and write-back only happens on a real InputText change -- a blind copy every frame
// would truncate an untouched value the first frame the page opens.
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
    // A checkbox plus a step: snapMove_ et al are on/off, moveSnap_ et al are the increment, kept
    // separate so turning snapping off doesn't lose the set step.
    // Checkbox on its own line, value below at full width -- side by side clipped "15 deg" to "15 de".
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

void SandboxApp::buildSimulateModePanel(Engine& e) {
    ImGui::TextDisabled("SIMULATE");
    ImGui::TextWrapped("Runs the game in the viewport without possessing the player: physics and "
                       "gameplay scripts tick while the editor keeps the camera, selection and "
                       "gizmos. Possess takes control of the player; Eject gives it back.");
    ImGui::Spacing();
    // launchPlay/stopPlay are AVER_MODULE_FRAMEWORK entry points (spawn a pawn under a GameMode,
    // drive aver_fw_play_state); without this guard the button still links (playSessionActive()
    // has its own #else returning false) but calls into code a
    // framework-off tree never compiled -- the mismatch module-matrix.ps1 exists to catch. The
    // whole block is guarded, not just the two calls: a Play that always offers Play, never Stop,
    // is a control wired to a game mode that was never there, not a degraded feature.
#if AVER_MODULE_FRAMEWORK
    const bool active = playSessionActive();
    if (!active) {
        if (ImGui::Button("Play", ImVec2(-1, 0))) launchPlay(e, PlayMode::Simulate);
    } else {
        if (ImGui::Button(playEjected() ? "Possess" : "Eject", ImVec2(-1, 0))) togglePlayEject();
        if (ImGui::Button("Stop", ImVec2(-1, 0))) stopPlay();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s releases the mouse back to the editor.",
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayReleaseMouse)).c_str());
#else
    (void)e;
    ImGui::TextWrapped("This build has the gameplay framework module switched off, so there is no "
                       "game mode to run and Play does nothing here.");
#endif
}

#endif

#if AVER_WITH_IMGUI
// The level's WATER record, as a panel.
// Previously unreachable except by hand-editing the .ocworld: levelHeader_.waters is populated
// only by the loader/saveLevel, with no menu item, Add entry, Outliner row or Details section
// (the Add menu offers Cube, Player Start, Sphere; Plane and Point Light are greyed out).
// ONE RECORD ONLY: WaterRenderer holds one level and one wave set (GameWater::applyLevel warns
// and renders just the first when a file declares more), so a list here would let someone author
// a second surface the renderer silently ignores.
void SandboxApp::buildWaterPanel(Engine& e) {
    // Guarded on AVER_MODULE_FLUIDS, not SCENE: water_ is a game::GameWater declared under FLUIDS
    // alone in SandboxApp.hpp (see that member's comment on why water is independent of
    // Particles/Scene); everything else here (levelHeader_, camPos_) is unconditional. A
    // fluids-off, scene-on tree used to compile this as the SCENE branch and call
    // water_.applyLevel on an undeclared member.
#if AVER_WITH_IMGUI && AVER_MODULE_FLUIDS
    ImGui::TextDisabled("WATER");
    if (levelHeader_.waters.empty()) {
        ImGui::TextDisabled("This level has no water.");
        if (ImGui::Button("Add Water", ImVec2(-1, 0))) {
            fmt::OcWaterPlacement wp;
            wp.name = "Water";
            // At the camera's feet, not z=0 (invisible on terrain built up from the origin).
            // Rounded so the field shows a number somebody would actually have typed.
            wp.levelCm = std::floor(camPos_.z / 10.0f) * 10.0f - 100.0;
            wp.infinite = true;
            levelHeader_.waters.assign(1, std::move(wp));
            water_.applyLevel(*e.device(), levelHeader_);
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
        // X/Y only: the surface is a plane at `level`, so bounds are its 2D footprint.
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
    // Shown but disabled when Infinite: a simulated volume needs a closed shell/size, which the
    // format can express but nothing can honour. GameWater::applyLevel warns on this too.
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

    // RE-APPLIED ON EVERY EDIT, so the viewport agrees with the numbers. GameWater::applyLevel is
    // idempotent for the analytic path -- it rebuilds the surface from the record it is handed.
    if (changed) water_.applyLevel(*e.device(), levelHeader_);
    ImGui::Separator();
#else
    (void)e;
#endif
}

// The landscape tool set: what Landscape mode was missing.
// GUARD BOUNDARY MOVED HERE: it used to start about a hundred lines higher and wrap
// buildWaterPanel too (swept in only because it sat just above this function), but water is a
// level property (like sun/fog -- see the Sky panel's comment) touching only levelHeader_.waters
// and water_, and an AVER_MODULE_LANDSCAPE=OFF build died on that mismatch. The call site below,
// guarded on AVER_WITH_IMGUI alone, was already correct. Everything from here down genuinely
// reads landscape_.
#if AVER_MODULE_LANDSCAPE
void SandboxApp::buildLandscapeModePanel(Engine& e) {
    if (!landscape_.loaded()) {
        // Previously a dead end: this printed one line and returned, and nothing else in the
        // editor could create a landscape either.
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
        int seed = static_cast<int>(landscape_.noiseParams().seed);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt("Seed", &seed)) landscape_.noiseParams().seed = static_cast<u32>(seed);
        editor::panelFloat("Feature Size (cm)", &landscape_.noiseParams().featureSizeCm, 500.0f, 40000.0f,
                   "%.0f", ImGuiSliderFlags_Logarithmic);
        editor::panelFloat("Amplitude (cm)", &landscape_.noiseParams().amplitudeCm, 0.0f, 6000.0f, "%.0f");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("Octaves", &landscape_.noiseParams().octaves, 1, 8);
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

    // ---- PLACEMENT: previously no control at all (x/y/z only reachable by hand-editing the
    // .ocworld) -- the section's own origin sample in world space, for aligning terrain to a level.
    ImGui::TextDisabled("PLACEMENT");
    {
        f32 org[3] = {landscape_.data().originCm[0], landscape_.data().originCm[1],
                      landscape_.data().originCm[2]};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("Origin (cm)", org, 10.0f)) {
            landscape_.setOriginCm(org[0], org[1], org[2]);
            recordLandscapeInLevel();
        }
        uiReg_.track("landscape.origin");
    }
    ImGui::Spacing();

    ImGui::TextDisabled("SCULPT");
    // 6 tools; only the first 4 have a hotkey (SculptRaise..SculptFlatten, keys 1-4). Ramp/Noise
    // are mouse/panel-only for now, so no tooltip advertises a key that does nothing.
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
    // FALLOFF is new: applyBrush's curve was a hardcoded smoothstep with no way to reach it, and
    // every terrain editor exposes hard vs. soft edge as separate tools.
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
    // The noise generator existed but was unreachable: terrainHeightAt() was wired only as the
    // height source for procedural tiles past the authored rim, not as an author-facing tool.
    editor::panelFloat("Feature size (cm)", &landscape_.noiseParams().featureSizeCm, 500.0f, 50000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelFloat("Amplitude (cm)", &landscape_.noiseParams().amplitudeCm, 0.0f, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    editor::panelInt("Octaves", &landscape_.noiseParams().octaves, 1, 8);
    int seed = static_cast<int>(landscape_.noiseParams().seed);
    ImGui::TextUnformatted("Seed");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##Seed", &seed)) landscape_.noiseParams().seed = static_cast<u32>(seed);
    if (ImGui::Button("Generate terrain", ImVec2(-1, 0))) generateLandscapeNoise(e);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replaces every height in this section with the noise above.\n"
                          "One undo step -- Ctrl+Z restores what was here.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(landscape_.dirty() ? "Save Terrain *" : "Save Terrain", ImVec2(-1, 0))) saveLandscape();
    ImGui::TextDisabled("%u x %u samples, %.0f cm spacing",
                        landscape_.data().sampleCount, landscape_.data().sampleCount,
                        landscape_.data().spacingCm);
}

// The Foliage panel's empty-palette action: writes a starter .ocfoliage and opens its tab, reusing
// cbCreateFoliageType() rather than duplicating its write-then-open logic (see that function's own
// comment). This wrapper only fixes WHERE it lands: cbCreateFoliageType() writes into
// cbSelectedDir_ (the Content Browser's currently browsed folder), which from this panel could be
// anything -- read-only engine content, a deeply nested subfolder, or empty -- so Content/Foliage
// is created and browsed to first. cbCreateFoliageType() already refreshes the palette, so the
// new type paints immediately.
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
// Marks a level record (sun/sky/fog/clouds) as authored, so saveLevel writes it, and marks the
// level dirty -- the same fact twice: setting `has` means saveLevel now emits a record it would
// not have before, i.e. the document differs from the file.
//
// FIXES: hasLevelSun_ etc. were previously set only by the loader, so a level with no SUN record
// silently threw away every edit on save (saveLevel's own comment claiming Details edits survive
// was true only for levels that already had the record). levelHasUnsavedEdits() is driven purely
// by the undo serial and these panels push no EditCmd, so nothing else marked it dirty either.
//
// IsAnyItemActive() is FRAME-GLOBAL BY DESIGN: dragging a control elsewhere while this section is
// open also sets the flag -- one spurious record beats silently discarding real edits.
//
// markLevelUnsaved(), not a serial bump: these edits are not undoable, so the mark must survive
// undo and clear only on save.
void SandboxApp::markLevelRecordEdited(bool& has) {
    if (ImGui::IsAnyItemActive()) { has = true; markLevelUnsaved(); }
}

// Draws the World Outliner and Details panel, each only when Window > ... has it on.
// SPLIT SO EACH CAN BE HIDDEN: ImGui::Begin does not skip a window whose p_open is false (that
// early-out is BeginPopupModal's, not Begin's), so hiding needs a caller-side guard -- previously
// one function submitted both unconditionally, so their Window menu items could never do anything.
void SandboxApp::buildPanels(Engine& e) {
    if (showOutliner_) buildOutlinerPanel();
    // Takes the Engine now (previously `(void)e`, since nothing here needed one): the Level
    // branch edits the WATER record, and applying it to the live surface needs a device.
    if (showDetails_)  buildDetailsPanel(e);
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
// ASCII lowercase, matching the Content Browser's filter: an editor label, not locale text.
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
// A CYCLE IS NEVER OFFERED: the row peeks at the payload before BeginDragDropTarget, so dropping
// onto yourself or a descendant has no target at all (GraphEditor's/BtEditor's filter-before-
// offering idiom). World::setParent refuses a cycle anyway; this keeps the UI from proposing one.
//
// AN OFF-LEVEL ENDPOINT IS OFFERED AND REFUSED, WITH A REASON: it would succeed live but be gone
// on the next save (saveLevel writes a parent only for entities the level owns) -- a drop that
// visibly works and quietly reverts is worse than one that explains why it can't happen.
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
        // Peek only, so ImGui never paints the row as about to accept -- the delivering branch is
        // structurally unreachable here, not merely un-taken.
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

// Draws one row and, when it is open, its children beneath it. `recordOrder` is false for a row
// whose place in outlinerOrder_ the caller has already recorded (see below).
void SandboxApp::drawOutlinerRow(const OutlinerRow& row,
                     const std::unordered_map<u32, std::vector<const OutlinerRow*>>& children,
                     int depth, bool recordOrder) {
    const auto it = children.find(static_cast<u32>(row.ent));
    const bool hasKids = it != children.end() && !it->second.empty();

    // NEITHER _Framed NOR _FramePadding -- a gates constraint, not taste: for a plain TreeNodeEx
    // the vertical padding is min(CurrLineTextBaseOffset, FramePadding.y), which is 0 at a row's
    // start, so the row matches the Selectable it replaces exactly; either flag adds
    // FramePadding.y*2. (This panel is a ratio-sized dock node and can't reach the Level viewport
    // rect anyway, but the rule costs nothing to hold.) SpanAvailWidth matches the Content
    // Browser's own folder tree.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    // The whole set is highlighted, not just the anchor: an invisible multi-select looks broken,
    // and the first thing a user does with a "broken" selection is click again and lose it.
    if (sel_ == kSelScene && (selEntity_ == row.ent || multiIsSelected(row.ent)))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasKids) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow;
        // A scene with no hierarchy in it has to look exactly as it did before this change.
        if (depth == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    // The row becomes a text field while renaming, rather than a dialog: edited where it's read,
    // as every file browser and outliner does, keeping the tree's shape from jumping.
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

    // Every drawn row, in order, so a shift-click has a range to walk: recorded here because only
    // this walk knows the filtered/sorted/expanded shape (same reason the Content Browser ranges
    // over its `shown` list, not the folder's contents). NOT FOR A ROW THE CLIPPER DRAWS: only the
    // rows on screen are drawn there, so the walk would record a window of the list. A flat list's
    // whole order was recorded by rebuildOutlinerCache (root order IS the visual order when nothing
    // nests); a tree's leaf roots are copied in a run at a time by drawOutlinerTreeRows. Both pass
    // recordOrder = false.
    if (recordOrder) outlinerOrder_.push_back(row.ent);

    const std::string label = "  " + row.shown + "##e" + std::to_string((u32)row.ent);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    // IsItemToggledOpen separates "clicked the arrow" from "clicked the label" on one node, so
    // expanding a parent does not also select it.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        // Same keys as the Content Browser and every file browser: shift extends from the
        // anchor, ctrl toggles, a bare click replaces -- the two panels must agree on this.
        const ImGuiIO& cio = ImGui::GetIO();
        if (cio.KeyShift && sel_ == kSelScene && selEntity_ != scene::kInvalidEntity)
            multiRange(row.ent);
        else if (cio.KeyCtrl)
            multiToggle(row.ent);
        else
            multiSetSingle(row.ent);
    }
    // F2 and right-click > Rename: the gestures a file browser trains everyone to expect, and
    // ones the Content Browser next door already has for a FILE. The Outliner had neither.
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        // Renaming is a one-entity act, so it collapses the selection rather than renaming the
        // anchor of a set and leaving the rest looking selected but untouched.
        multiSetSingle(row.ent);
        beginOutlinerRename(row.ent);
    }
    if (ImGui::BeginPopupContextItem(("##ctx" + std::to_string((u32)row.ent)).c_str())) {
        outlinerCtxRow_ = row.ent;   // re-marked every frame it stays open; see drawOutlinerFlatRows
        // Right-click on a row already in the selection keeps the whole set (otherwise "select
        // five, right-click, Delete" deletes one); right-click outside it selects just that row.
        if (!multiIsSelected(row.ent)) multiSetSingle(row.ent);
        else { sel_ = kSelScene; selEntity_ = row.ent; }
        if (ImGui::MenuItem("Rename", "F2")) beginOutlinerRename(row.ent);
        // Rename was the only entry; this adds Delete via deleteSelection() (not destroyEntity(),
        // so the subtree is captured for undo -- the row above already made this entity the
        // selection).
        // DEFERRED, NOT DONE HERE: TreeNodeEx has pushed for a row with children (unbalancing the
        // tree stack on an early return), and this walk holds references into `children`/`row`
        // that destroying an entity mid-walk would invalidate. Answered after the whole tree draws.
        if (ImGui::MenuItem("Delete", editor::chordToString(
                keybinds_.chordFor(editor::CommandId::EditDelete)).c_str())) {
            outlinerDeleteRequest_ = row.ent;
        }
        ImGui::EndPopup();
    }
    // Only a row that is on screen: a scrolled-out row of an open subtree has no rect a click
    // could reach, and naming it cost a heap string per row per frame.
    if (ImGui::IsItemVisible())
        uiReg_.track(("outliner.row." + std::to_string((u32)row.ent)).c_str());

    drawOutlinerDragSource(row.ent);
    drawOutlinerDropTarget(row.ent);

    if (hasKids && open) {
        for (const OutlinerRow* c : it->second) drawOutlinerRow(*c, children, depth + 1);
        ImGui::TreePop();
    }
}

// THE ROOTS THE CLIPPED DRAWS KEEP SUBMITTED WHEREVER THEY SCROLL: the one being renamed, the one
// being dragged (read from the drag payload) and the one whose context menu was open last frame.
// Writes the index into `roots` of each of those that is a root and returns how many it found (0-3);
// the scan stops at the last distinct one, and is skipped outright when nothing is pinned, which is
// nearly always.
static int outlinerPinnedRoots(const std::vector<scene::Entity>& roots, scene::Entity renaming,
                               scene::Entity ctxRow, int (&out)[3]) {
    scene::Entity dragged = scene::kInvalidEntity;
    if (const ImGuiPayload* peek = ImGui::GetDragDropPayload())
        if (peek->IsDataType(kOutlinerReparentDragDropType) &&
            peek->DataSize == static_cast<int>(sizeof(scene::Entity)))
            dragged = *static_cast<const scene::Entity*>(peek->Data);
    const scene::Entity pins[3] = {renaming, dragged, ctxRow};
    int pinned = 0;                      // distinct pins, so the scan below stops at the last one
    for (int a = 0; a < 3; ++a) {
        bool dup = pins[a] == scene::kInvalidEntity;
        for (int b = 0; b < a && !dup; ++b) dup = pins[b] == pins[a];
        if (!dup) ++pinned;
    }
    int found = 0;
    const int count = static_cast<int>(roots.size());
    for (int i = 0; pinned > 0 && i < count; ++i) {
        const scene::Entity ent = roots[static_cast<usize>(i)];
        if (ent != renaming && ent != dragged && ent != ctxRow) continue;
        out[found++] = i;
        --pinned;
    }
    return found;
}

// DRAWS A FLAT CACHE (no row has a parent listed) WITH ONLY THE ROWS ON SCREEN SUBMITTED. Every row
// is then a leaf of one uniform height, so ImGuiListClipper can skip straight to the visible window.
// Against a 51k-root level the per-row work (a label string, an ID string, a context-popup ID, a
// UI-registry name, a children-map lookup, a selection test) used to run for ALL of the rows every
// frame; it is now paid for the few dozen on screen. The clipper's row pitch is the FLOOR of the
// text line height with spacing, because that is what ItemSize advances a leaf TreeNodeEx by (it
// truncates the cursor to whole pixels), so the rows the clipper positions and the rows drawn under
// them agree at any DPI scale.
//
// THREE ROWS STAY SUBMITTED WHEREVER THEY SCROLL: the one being renamed, the one being dragged and
// the one whose context menu was open last frame (see outlinerPinnedRoots).
// ImGui only keeps an item's ACTIVE state (a text field's focus, a drag source) alive while the
// item is submitted every frame -- an item ImGui clips itself still counts, a row the clipper never
// submits does not -- so wheel-scrolling toward a far drop target mid-drag would cancel the drag,
// and scrolling away from a rename would orphan its text field.
void SandboxApp::drawOutlinerFlatRows() {
    const int count = static_cast<int>(outlinerRoots_.size());
    ImGuiListClipper clipper;
    clipper.Begin(count, std::floor(ImGui::GetTextLineHeightWithSpacing()));

    // The row whose context menu was open last frame is pinned too; drawing it re-marks it if the
    // menu is still open, so the mark lapses one frame after the menu closes.
    const scene::Entity ctxRow = outlinerCtxRow_;
    outlinerCtxRow_ = scene::kInvalidEntity;
    int pinAt[3] = {-1, -1, -1};
    const int pinCount = outlinerPinnedRoots(outlinerRootEnts_, outlinerRenaming_, ctxRow, pinAt);
    for (int p = 0; p < pinCount; ++p) clipper.IncludeItemByIndex(pinAt[p]);

    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            drawOutlinerRow(*outlinerRoots_[static_cast<usize>(i)], outlinerChildren_, 0, false);
}

// DRAWS A TREE CACHE (some row has a parent listed) WITHOUT SUBMITTING THE LEAF ROOTS THAT ARE OFF
// SCREEN. A tree used to draw EVERY root every frame, unclipped, because its rows are not one
// uniform list; but a level with tens of thousands of roots and a handful of parents (NeonDistrict
// in Play: 52.8k placements, plus the pawn with its body, view node and gun as children) is almost
// all uniform leaves. So the roots are split at the roots that HAVE listed children
// (outlinerParentRoots_, found once by the rebuild): each maximal run of leaf roots between two
// parents goes through its own ImGuiListClipper, exactly as drawOutlinerFlatRows does for a whole
// flat list, and each parent is drawn by drawOutlinerRow with its open subtree, as before. Roots
// keep their alphabetical order, so the rows come out in the same places as they always did; the
// clipper only declines to build the ones nobody can see. Per frame that is the rows on screen, the
// parents and their subtrees, plus one memcpy of the root entities into outlinerOrder_.
//
// outlinerOrder_ is the DRAW order, as for any tree: a leaf run's entities are copied in whole
// (the clipper only submits a window of the run, so a per-row push could not see the rest), and a
// parent's row records itself and its open rows when drawOutlinerRow draws it. The one deliberate
// difference from the unclipped walk: a leaf root being renamed is now listed in the order, as it
// always was in a flat list (drawOutlinerRow returns before recording a row that is a text field).
//
// THE SAME THREE ROWS STAY SUBMITTED WHEREVER THEY SCROLL as in the flat path (see
// drawOutlinerFlatRows), each included in the clipper of the run it falls in. A parent and its
// subtree are always submitted, so a pin there needs nothing.
void SandboxApp::drawOutlinerTreeRows() {
    const int count = static_cast<int>(outlinerRoots_.size());
    outlinerOrder_.clear();
    outlinerOrder_.reserve(outlinerRootEnts_.size());

    const scene::Entity ctxRow = outlinerCtxRow_;
    outlinerCtxRow_ = scene::kInvalidEntity;
    int pinAt[3] = {-1, -1, -1};
    const int pinCount = outlinerPinnedRoots(outlinerRootEnts_, outlinerRenaming_, ctxRow, pinAt);
    const float rowPitch = std::floor(ImGui::GetTextLineHeightWithSpacing());

    const auto drawLeafRun = [&](int from, int to) {
        if (from >= to) return;
        outlinerOrder_.insert(outlinerOrder_.end(), outlinerRootEnts_.begin() + from,
                              outlinerRootEnts_.begin() + to);
        ImGuiListClipper clipper;
        clipper.Begin(to - from, rowPitch);
        for (int p = 0; p < pinCount; ++p)
            if (pinAt[p] >= from && pinAt[p] < to) clipper.IncludeItemByIndex(pinAt[p] - from);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                drawOutlinerRow(*outlinerRoots_[static_cast<usize>(from + i)], outlinerChildren_, 0,
                                false);
    };

    int next = 0;
    for (const u32 parentAt : outlinerParentRoots_) {
        drawLeafRun(next, static_cast<int>(parentAt));
        drawOutlinerRow(*outlinerRoots_[parentAt], outlinerChildren_, 0);
        next = static_cast<int>(parentAt) + 1;
    }
    drawLeafRun(next, count);
}

// THE O(1) HALF OF "IS THE OUTLINER CACHE STALE", read every frame. refreshOutlinerCache() rebuilds
// (two std::string constructions per entity, two unreserved hash containers, a string-compare sort)
// when this moves, and outlinerAuditDiverged() covers what it cannot see.
//
// WHY NOT A HASH OVER EVERY ENTITY, WHICH THIS WAS: that walk (valid, destroyPending, a chunk-
// ownership lookup, a component lookup, a name hash, parent and childCount per entity) ran on every
// frame to learn that nothing changed, and against a 50k-entity level it cost more than the rest of
// an idle frame put together. And it cannot be replaced by a revision counter bumped where things
// change: spawn, destroy, rename, reparent, component add/remove, chunk streaming and PIE spawns live
// in World.cpp, SandboxLevelLoad.cpp, SandboxMcp.cpp and the world module, none of which this cache
// may touch, and scene::World carries no revision of its own that fits (worldRevision() is
// per-entity and tracks the TRANSFORM). So nothing can TELL this cache when they happen. What it can
// read for free is:
//   - w.count(): every create, destroy and chunk stream in/out moves it. A create and a destroy in
//     the same frame net to the same count; that, and every other change below the stamp, is the
//     audit's job, not this function's;
//   - the filter text and the drone handle, which change which rows are listed;
//   - entityLabels_.size(): the load/paste/spawn/rename override map outlinerLabelFor() reads;
//   - the level's own entity list (size, first and last handle): a level load or reload can leave
//     the entity count and the label map exactly where they were, but it cannot reuse a handle.
// What is NOT in it, on purpose: the edit stamp (see outlinerEditMark). Nearly every editor command
// is a transform, a visibility toggle or a material pick, none of which changes a row, so folding it
// in here would rebuild the whole tree (a string per entity, a sort) on every gizmo release.
u64 SandboxApp::outlinerSignature() const {
    const scene::World& w = scene::World::instance();
    u64 h = kFnv1a64OffsetBasis;
    const auto mix = [&h](u64 v) { h = (h ^ v) * kFnv1a64Prime; };
    mix(w.count());
    mix(fnv1a64(outlinerFilter_));
    mix(static_cast<u64>(droneEntity_));
    mix(static_cast<u64>(entityLabels_.size()));
    mix(static_cast<u64>(levelEntities_.size()));
    mix(levelEntities_.empty() ? 0u : static_cast<u64>(levelEntities_.front()));
    mix(levelEntities_.empty() ? 0u : static_cast<u64>(levelEntities_.back()));
    return h;
}

// EVERYTHING rebuildOutlinerCache() READS ABOUT THE ENTITY IN ONE DENSE SLOT, as one fingerprint.
// The rebuild records one per slot; the audit recomputes a slice of them each frame and compares.
// Same fields, and the same early-outs, as the filters in the rebuild's pass one: what the row
// would show (the name, or the label that overrides it), whether it is listed at all (valid, not
// pending destroy, not chunk-streamed, not the drone, has a mesh or a name or a child), and where
// it hangs (parent).
//
// DESTROY SWAPS THE LAST LIVE ENTITY INTO A FREED SLOT (w.at()'s dense order), so a destroy shows up
// as a mismatch at that slot too -- an extra rebuild the audit did not strictly need, accepted on
// purpose: an unnecessary rebuild costs one frame, a missed one shows a stale tree.
u64 SandboxApp::outlinerSlotSignature(u32 slot) const {
    const scene::World& w = scene::World::instance();
    u64 h = kFnv1a64OffsetBasis;
    const auto mix = [&h](u64 v) { h = (h ^ v) * kFnv1a64Prime; };
    const scene::Entity ent = w.at(slot);
    mix(static_cast<u64>(ent));
    const bool valid = w.valid(ent);
    mix(valid ? 1u : 0u);
    if (!valid) return h;
    const bool pending = w.destroyPending(ent);
    mix(pending ? 1u : 0u);
    if (pending) return h;
    const bool chunkOwned = anyChunkWorldOwns(ent);
    mix(chunkOwned ? 1u : 0u);
    if (chunkOwned) return h;
    if (ent == droneEntity_) return h;   // the stamp folds the drone handle in already
    const bool hasMesh = w.hasComponent(ent, scene::kComponentMeshRenderer);
    const char* nm = w.name(ent);
    mix(hasMesh ? 1u : 0u);
    mix(fnv1a64(std::string_view(nm)));
    mix(static_cast<u64>(w.parent(ent)));
    // childCount only changes the drawn rows through the pivot-keep rule (an unnamed, mesh-less
    // entity survives once it has a child), so it only needs hashing for the entities that rule
    // could apply to.
    if (!hasMesh && *nm == '\0') mix(static_cast<u64>(w.childCount(ent)));
    // The row's label, when entityLabels_ overrides the name: looked up per slot rather than by
    // walking the whole map, since a slice only needs the labels of ITS entities.
    const auto lit = entityLabels_.find(static_cast<u32>(ent));
    mix(lit != entityLabels_.end() ? 1u + fnv1a64(lit->second) : 0u);
    return h;
}

// WHERE THE EDITOR'S OWN COMMANDS LEAVE A TRACE: editSerialNext_ and both undo stack sizes. Every
// command the editor issues (rename, reparent, spawn, delete, paste, a component edit, a transform)
// goes through pushEdit, undo or redo, and each of those moves at least one of the three -- so
// when this moves, the change MAY have been to a row. Only "may": most commands are transforms,
// visibility toggles and material picks that change none, so refreshOutlinerCache answers a move
// with one full audit pass (outlinerAuditDiverged(true), a hash per slot) and rebuilds only if that
// finds a difference. A rename or reparent made in the UI is thus reflected the same frame; a
// gizmo release costs one cheap pass, not a rebuild of the tree.
u64 SandboxApp::outlinerEditMark() const {
    u64 h = kFnv1a64OffsetBasis;
    const auto mix = [&h](u64 v) { h = (h ^ v) * kFnv1a64Prime; };
    mix(editSerialNext_);
    mix(static_cast<u64>(undoStack_.size()));
    mix(static_cast<u64>(redoStack_.size()));
    return h;
}

// THE OTHER HALF: catches whatever outlinerSignature() cannot see -- a script or the MCP renaming
// or reparenting, a mesh added or removed, a same-frame create+destroy -- by re-fingerprinting dense
// slots against what the last rebuild recorded. True when one differs. A SLICE per frame normally;
// `full` checks every slot (see outlinerEditMark for when).
//
// WHAT THIS PROVES: every slot is re-checked at least once per cycle (kAuditCycleFrames frames, four
// times that while Play runs; the slice is ceil(n / cycle) slots, advancing round the array), so any
// change to a slot's fingerprint that lasts that long is caught, at a per-frame cost bounded by the
// slice. A level under kAuditMinSlice slots is re-checked in FULL every frame, which is the old
// behaviour exactly. A change that reverts before its slot's turn comes round is one the tree never
// needed to show.
bool SandboxApp::outlinerAuditDiverged(bool full) {
    const u32 n = scene::World::instance().count();
    // The stamp holds the count, so a mismatch here means the rebuild has not run since it moved.
    if (outlinerSlotSigs_.size() != n) return true;
    // FOUR TIMES AS LONG A CYCLE WHILE PLAYING: a spawn or destroy moves the stamp's entity count and
    // is not the audit's job, and a Play frame is many times an idle one, so the slice (n / cycle
    // slots of hashing, about 0.2 ms at 52.8k entities) is not worth paying at the idle rate.
    constexpr u32 kAuditCycleFrames = 32;
    constexpr u32 kAuditMinSlice    = 256;
    const u32 cycle = outlinerPlaying() ? kAuditCycleFrames * 4 : kAuditCycleFrames;
    const u32 slice = full ? n
        : std::min(n, std::max(kAuditMinSlice, (n + cycle - 1) / cycle));
    u32 i = outlinerAuditCursor_ < n ? outlinerAuditCursor_ : 0;
    for (u32 k = 0; k < slice; ++k) {
        if (outlinerSlotSignature(i) != outlinerSlotSigs_[i]) return true;
        if (++i == n) i = 0;
    }
    outlinerAuditCursor_ = i;
    return false;
}

// True while any kind of Play is running. A build without the framework has no Play, so it is false.
bool SandboxApp::outlinerPlaying() const {
#if AVER_MODULE_FRAMEWORK
    return anyPlayActive();
#else
    return false;
#endif
}

// Rebuilds the cache when the stamp moved or an audit found a slot that no longer matches.
//
// WHILE PLAY RUNS THE REBUILD IS RATE-LIMITED to one per kPlayRebuildInterval. A rebuild is one
// string per entity plus a string-compare sort (about 45 ms at 52.8k entities), and every spawn or
// destroy moves the stamp's entity count: the pawn, its controller, its camera node and its gun
// arrive over the first frames of a session and each one cost a full rebuild. The first change
// after the interval goes through at once (a Play press right after a load pays one at the start),
// the rest wait for the interval to run out and are then caught in one rebuild, because nothing
// below the early-out touches the cache or its stamp. The tree may show a row for an entity that is
// gone, or miss one, for up to the interval; a row's entity is handle-checked wherever it is acted
// on (renameEntity, pushReparent, selectedEntities, the deferred delete), and the audit already left
// a rename or reparent unseen for up to a cycle. NOT THROTTLED: when no Play is running (so Stop
// rebuilds on the very next frame), and the frame an editor command moved the edit mark (a rename
// made in the UI still shows at once).
void SandboxApp::refreshOutlinerCache() {
    constexpr double kPlayRebuildInterval = 0.5;   // seconds
    const u64 editMark = outlinerEditMark();
    const bool edited = editMark != outlinerEditMark_;
    if (outlinerCacheValid_ && !edited && outlinerPlaying() &&
        ImGui::GetTime() - outlinerRebuiltAt_ < kPlayRebuildInterval)
        return;
    const u64 sig = outlinerSignature();
    outlinerEditMark_ = editMark;
    if (outlinerCacheValid_ && sig == outlinerCacheSig_ && !outlinerAuditDiverged(edited)) return;
    rebuildOutlinerCache();
    outlinerCacheSig_ = sig;
    outlinerCacheValid_ = true;
    outlinerRebuiltAt_ = ImGui::GetTime();
}

// Redoes the three passes and the sort that buildOutlinerPanel used to run unconditionally every
// frame, filling outlinerRows_/outlinerChildren_/outlinerRoots_. Called only when
// refreshOutlinerCache() finds the cached result stale (the stamp moved, or the audit caught a slot).
void SandboxApp::rebuildOutlinerCache() {
    scene::World& w = scene::World::instance();
    const u32 n = w.count();

    // PASS ONE: which entities are listed at all. The three filters are unchanged.
    std::vector<OutlinerRow> rows;
    std::unordered_set<u32> survived;
    // One fingerprint per dense slot, recorded as the rows are decided, for outlinerAuditDiverged().
    std::vector<u64> slotSigs(n);
    rows.reserve(n);
    survived.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        slotSigs[i] = outlinerSlotSignature(i);
        const scene::Entity ent = w.at(i);
        if (!w.valid(ent) || w.destroyPending(ent)) continue;
        // Chunk-streamed entities are excluded on purpose: hundreds can come and go as the
        // camera moves; this list is what a designer placed (live count: buildChunkStreamingPanel).
        if (anyChunkWorldOwns(ent)) continue;
        // The drone is excluded for the same reason: transient, not authored (tracked via the
        // [Drone] AVER_INFO lines instead; it has no Details entry either).
        if (ent == droneEntity_) continue;
        const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        // A RAW POINTER, NOT A std::string: this only needs to test emptiness, and the label
        // below (outlinerLabelFor) reads the name again anyway -- building a std::string here
        // just to throw it away was one of two per-entity allocations this cache exists to avoid.
        const char* nm = w.name(ent);
        // Anything drawable or named is kept, plus an unnamed mesh-less entity once it HAS a
        // child (else hiding a pivot the instant something reparents onto it looks like a
        // failed reparent). childCount is the raw engine count, so a pivot whose children are
        // all filtered shows as an empty leaf -- accepted over a third pass on survivors.
        if (!mr && *nm == '\0' && w.childCount(ent) == 0) continue;
        rows.push_back(OutlinerRow{ent, w.parent(ent), outlinerLabelFor(ent)});
        survived.insert(static_cast<u32>(ent));
    }

    // PASS 1b: the name filter, applied after rows exist so ancestors can be kept. Matches a
    // case-insensitive substring of the label; ancestors are kept too (a dropped parent's row
    // is never visited by the root-down walk below) but descendants of a match are NOT --
    // "show me the thing I named" shouldn't unfold its subtree.
    if (!outlinerFilter_.empty()) {
        const std::string needle = lowerCopy(outlinerFilter_);
        std::unordered_map<u32, const OutlinerRow*> byEnt;
        byEnt.reserve(rows.size());
        for (const OutlinerRow& r : rows) byEnt[static_cast<u32>(r.ent)] = &r;

        std::unordered_set<u32> keep;
        keep.reserve(rows.size());
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

    // outlinerRows_ TAKES OWNERSHIP FIRST, before outlinerChildren_/outlinerRoots_ are built: both
    // point INTO outlinerRows_, so it must reach its final address before anything takes a
    // pointer into it. The swap moves rows in without copying every OutlinerRow::shown string.
    outlinerRows_.swap(rows);

    // PASS TWO, and it has to be separate: w.at() walks the dense array (swap-with-last on
    // destroy, no ordering guarantee), so a child can appear before its parent -- testing
    // membership against a still-filling set would wrongly promote children to roots.
    outlinerChildren_.clear();
    outlinerChildren_.reserve(outlinerRows_.size());
    outlinerRoots_.clear();
    for (const OutlinerRow& r : outlinerRows_) {
        if (r.par != scene::kInvalidEntity && survived.count(static_cast<u32>(r.par)))
            outlinerChildren_[static_cast<u32>(r.par)].push_back(&r);
        else
            outlinerRoots_.push_back(&r);   // true root, or one whose parent was filtered out or
                                            // gone -- promoted so it stays reachable.
    }
    // ALPHABETICAL, not scan order: linkToParent PREPENDS, so the engine's child order is
    // newest-first and the list would visibly reshuffle every time anything is attached.
    const auto byLabel = [](const OutlinerRow* a, const OutlinerRow* b) { return a->shown < b->shown; };
    std::sort(outlinerRoots_.begin(), outlinerRoots_.end(), byLabel);
    for (auto& kv : outlinerChildren_) std::sort(kv.second.begin(), kv.second.end(), byLabel);

    // What the audit compares against, and where it starts: a fresh cycle from slot 0.
    outlinerSlotSigs_.swap(slotSigs);
    outlinerAuditCursor_ = 0;

    // THE ROOTS AS A PLAIN ARRAY, and which of them have children, both found HERE so the per-frame
    // draws never touch a row to learn them: the roots are alphabetical, so their rows sit all over
    // outlinerRows_ and a pass over them is a cache miss per root (52k of them every frame).
    // outlinerParentRoots_ is empty for a flat list, which skips the lookups.
    outlinerRootEnts_.clear();
    outlinerRootEnts_.reserve(outlinerRoots_.size());
    outlinerParentRoots_.clear();
    for (usize i = 0; i < outlinerRoots_.size(); ++i) {
        const scene::Entity ent = outlinerRoots_[i]->ent;
        outlinerRootEnts_.push_back(ent);
        if (!outlinerChildren_.empty() && outlinerChildren_.count(static_cast<u32>(ent)))
            outlinerParentRoots_.push_back(static_cast<u32>(i));
    }

    // A FLAT LIST'S ORDER IS RECORDED HERE, ONCE. Nothing nests (no row has a listed parent), so the
    // roots ARE the rows in visual order, and buildOutlinerPanel submits only the ones on screen
    // (drawOutlinerFlatRows) -- so drawOutlinerRow cannot be what fills outlinerOrder_ any more, and
    // multiRange/selectAllInOutliner/the Select All enable checks all need the whole list. A tree
    // rebuilds its order each frame (drawOutlinerTreeRows): the open rows move with the folders.
    outlinerFlat_ = outlinerChildren_.empty();
    if (outlinerFlat_) outlinerOrder_ = outlinerRootEnts_;
}

#endif
#endif

#if AVER_WITH_IMGUI
void SandboxApp::buildOutlinerPanel() {
    const bool visible = ImGui::Begin("World Outliner", &showOutliner_);
    // So the edit verbs work on a selection made HERE -- see the dispatch in handleManip.
    outlinerFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // COLLAPSED, OR BEHIND ANOTHER DOCK TAB: Begin returned false, so nothing below would be seen
    // (its items are all skipped), yet this used to walk every row anyway. Same early-out as
    // buildProfilerPanel. The one thing still owed is outlinerOrder_: Select All (the Select menu,
    // the viewport chord) ranges over it, and it used to be refreshed by this very walk even with
    // the window hidden -- roots only, since a skipped TreeNodeEx never opens. The cache refresh is
    // cheap now (see outlinerSignature), so keep that promise here.
    if (!visible) {
#if AVER_MODULE_SCENE
        refreshOutlinerCache();
        if (!outlinerFlat_) outlinerOrder_ = outlinerRootEnts_;
#endif
        ImGui::End();
        return;
    }

    // A FILTER: an alphabetical tree of six thousand entities is scrolled past, not searched
    // (Sponza alone lists a few hundred).
    // Matches by name and keeps ancestors, so a hit stays reachable in the walked-from-root tree
    // (see Pass 1b below for the mechanics).
    // GUARDED WITH WHAT IT FILTERS: outlinerFilter_ lives behind AVER_MODULE_SCENE in
    // SandboxApp.hpp, while this panel draws under AVER_WITH_IMGUI alone -- a scene-less editor
    // still opens the Outliner, just with only placeholder objects, not worth filtering.
#if AVER_MODULE_SCENE
    {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", outlinerFilter_.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##outlinerFilter", ICON_SEARCH " Filter by name", buf, sizeof buf))
            outlinerFilter_ = buf;
        uiReg_.track("outliner.filter");
    }
#endif
    if (!hideEditorScene_)
        for (int i=0;i<(int)objects_.size();++i) {
            if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) { sel_=i; selEntity_=kInvalidId; }
            uiReg_.track(("outliner.placeholder." + std::to_string(i)).c_str());
        }
#if AVER_MODULE_SCENE
    {
        // REBUILT ONLY WHEN refreshOutlinerCache() FINDS IT STALE. Against a 12k+-entity import
        // (Jungle Ruins: terrain tiles plus thousands of alpha-masked foliage instances) the three
        // passes and the std::sort() below used to run unconditionally here, on every single ImGui
        // frame, even an idle one. See outlinerSignature() and outlinerAuditDiverged() for what
        // says stale, and rebuildOutlinerCache() for what this used to do unconditionally.
        refreshOutlinerCache();

        if (!outlinerRoots_.empty() && !hideEditorScene_) ImGui::Separator();
        // A FLAT LIST submits only the rows on screen, and its order was recorded by the rebuild;
        // see drawOutlinerFlatRows.
        // A TREE submits the roots that have children and their open rows, and only the on-screen
        // rows of each run of childless roots between them (drawOutlinerTreeRows). Its order is the
        // DRAW order, not the cache above: rebuilt every frame because rows appear and vanish as
        // folders expand and collapse, and a stale order would range over rows no longer on screen.
        // Cheap regardless of cache state -- a copy of the childless roots' entities plus one push
        // per open row of a parent, and selection highlighting inside drawOutlinerRow reads
        // sel_/selEntity_/multiSel_ live, so a selection change needs no cache invalidation either.
        if (outlinerFlat_) drawOutlinerFlatRows();
        else               drawOutlinerTreeRows();

        // The deferred delete, answered here: the tree is fully drawn, every TreePop is paired,
        // and nothing below reads the row structures. Routed through deleteSelection so it
        // captures the subtree for undo too. destroyEntity() flips destroyPending() and erases
        // entityLabels_ (which moves outlinerSignature()'s size) synchronously, deleteSelection
        // pushes an undo entry (which moves outlinerEditMark(), so the next refresh audits every
        // slot and sees the pending destroy), and the flush that retires the entity moves the entity
        // count -- so the cache rebuilds on its own next frame, no cache poke needed here.
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
#if AVER_MODULE_VOXI
    // FOLIAGE: one INFORMATIONAL row when the level has any, never a per-instance one -- foliage
    // lives outside the entity/draw system entirely (ray-traced only, no collision, GameFoliage.hpp's
    // own header comment), so there is nothing here to select, reparent or drag, and
    // voxiRenderer_.foliageStats() is an O(1) read of numbers setFoliage already computed, not a
    // walk over however many million instances a level scattered.
    if (const voxi::VoxiRenderer::FoliageStats fs = voxiRenderer_.foliageStats(); fs.instances > 0) {
        ImGui::Separator();
        ImGui::TextDisabled("  Foliage  (%u type%s, %u instance%s)", fs.prototypes,
                             fs.prototypes == 1 ? "" : "s", fs.instances, fs.instances == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            const std::string files = levelHeader_.foliageFiles.empty()
                ? std::string("an unknown file")
                : levelHeader_.foliageFiles.size() == 1
                    ? levelHeader_.foliageFiles.front()
                    : std::to_string(levelHeader_.foliageFiles.size()) + " files";
            ImGui::SetTooltip("Ray-traced instanced foliage from %s -- static, not individually "
                              "selectable or editable per instance.", files.c_str());
        }
    }
#endif
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
            // Bracketed for undo, as the scene-entity branch below does: these three drags were
            // the only transform edits pushing nothing (selectedXform/endTransformEdit already
            // branch for placeholder objects via the gizmo, so dragging was undoable but typing
            // the same number here was not).
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
            // GUARDED BECAUSE BOTH SIDES ARE: MeshObj::material only exists under this same #if,
            // and so does materialPanel. Base Color is the object's own float[3] and stays.
#if AVER_MODULE_PBR
            materialPanel(o.material);
#endif
        }
        ImGui::Checkbox("Visible", &o.visible);
#if AVER_MODULE_SCENE
    } else if (sel_==kSelScene && scene::World::instance().valid(selEntity_)) {
        scene::World& w = scene::World::instance();
        const std::string nm = w.name(selEntity_);
        const auto lit = entityLabels_.find(static_cast<u32>(selEntity_));
        // Editable, which it never was: no rename affordance existed anywhere (not here, not the
        // Outliner, no F2, no context menu) so a level of "Cube 1..40" stayed that way;
        // entityLabels_ was written only at spawn/paste/duplicate/load, never by a person.
        // Committed on Enter or losing focus, not per keystroke -- else every letter typed is its
        // own undo entry (see materialPanel's comment).
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
        // Multi-selection, computed once and reused by Transform/Mesh/Material below (which apply
        // to the whole set); name and components stay single-entity, the anchor alone -- per
        // multiSel_'s own header comment, "one entity to talk about".
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
                // MULTI-SELECTION TRANSFORM, PER COMPONENT: shows the anchor's value via the same
                // DragFloat3 calls as the single-entity branch, but an edited component is SET on
                // every selected entity, not the gizmo's world-space delta (SandboxViewport.cpp)
                // -- typing Z=0 floors every prop at its own X/Y. Diffed against `orig*` (anchor's
                // pre-edit reading) so untouched components stay put.
                //
                // forEachMultiMoved supplies the set (for "(mixed)" and the apply): excludes the
                // anchor and any entity whose parent is selected too (so a selected child isn't
                // moved twice), same rule as the gizmo drag; endTransformEdit's
                // multiMoveBefore_/alsoMoved diff still yields one undo entry.
                Transform xf = loc->xf;
                Vec3 euler = eulerDegFromQuat(xf.rotation);
                const Vec3 origPos = xf.position, origEuler = euler, origScale = xf.scale;

                bool posMixed[3] = {false, false, false};
                bool rotMixed[3] = {false, false, false};
                bool scaleMixed[3] = {false, false, false};
                // Same tolerance as nearlySameXform/nearlySameTransform elsewhere: "(mixed)" is a
                // judgement call about equal-enough numbers, not the bit-exact touch test below.
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
                // "(mixed)" sits beside the whole DragFloat3 row (one control, three axes), so it
                // can't mark just the disagreeing axis -- same flag-it/explain-on-hover idiom as
                // the IOR/F0 check in materialPanel.
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
                    // "Touched", not "nonzero": which axis a widget wrote this frame, found the
                    // same way the mixed check above did -- by comparing to the pre-edit reading.
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
                bool vis = authoredVisible(selEntity_);
                if (ImGui::Checkbox("Visible", &vis)) {
                    // Applies to the whole selection (same rule as the Mesh/Material pickers
                    // below), but unlike them pushes undo: Visible is authored, level-saved data
                    // (owner decision, Unreal-style), not session bookkeeping, so a stray click
                    // needs to be as recoverable as a transform edit -- one EditCmd per click
                    // restores every touched entity (EditCmd::VisibilityChange).
                    EditCmd c;
                    c.kind = EditCmd::Kind::Visibility;
                    for (const scene::Entity ent : multiSelected) {
                        if (!w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer)) continue;
                        const bool before = authoredVisible(ent);
                        if (before == vis) continue;   // already there: no dead entry for it
                        setAuthoredVisible(ent, vis);
                        c.visibility.push_back({editIdFor(ent), before, vis});
                    }
                    if (!c.visibility.empty()) pushEdit(std::move(c));
                }
                // Saved with the level, Unreal-style -- unlike H/Shift+H/Ctrl+H (temporary,
                // editor-only, see hideSelection in SandboxViewport.cpp; can disagree with this).
                if (ImGui::IsItemHovered()) {
                    const bool hHidden = std::find(editorHidden_.begin(), editorHidden_.end(),
                                                   selEntity_) != editorHidden_.end();
                    if (hHidden)
                        ImGui::SetTooltip("Saved with the level.\n"
                                          "H hides it in the editor only, without changing this.\n"
                                          "(hidden in editor, Ctrl+H to show)");
                    else
                        ImGui::SetTooltip("Saved with the level.\n"
                                          "H hides it in the editor only, without changing this.");
                }
                // COLLIDES: the .ocworld `nocollide` flag, which until now could only be authored by
                // editing the file. Whole-selection and one undo entry per click, exactly like
                // Visible above (EditCmd::Collision). Off removes each entity's static body on the
                // spot; on fits a fresh one -- both inside setEntityCollide.
                {
                    const auto collides = [&](scene::Entity ent) {
                        const auto it = entityCollide_.find(static_cast<u32>(ent));
                        return it == entityCollide_.end() ? true : it->second;   // absent = the default
                    };
                    // Disabled for an entity the level does not own (a class instance, a preview): saveLevel
                    // writes nocollide per level placement only, so the edit would dirty the level and be lost.
                    const bool levelOwned = isLevelOwned(selEntity_);
                    ImGui::BeginDisabled(!levelOwned);
                    bool col = collides(selEntity_);
                    if (ImGui::Checkbox("Collides", &col)) {
                        EditCmd c;
                        c.kind = EditCmd::Kind::Collision;
                        for (const scene::Entity ent : multiSelected) {
                            if (!w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer)) continue;
                            if (!isLevelOwned(ent)) continue;   // a mixed selection edits only what would be saved
                            const bool before = collides(ent);
                            if (before == col) continue;   // already there: no dead entry for it
                            setEntityCollide(ent, col);
                            c.collide.push_back({editIdFor(ent), before, col});
                        }
                        if (!c.collide.empty()) pushEdit(std::move(c));
                    }
                    uiReg_.track("details.mesh.collides");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (levelOwned)
                            ImGui::SetTooltip("Saved with the level (off writes nocollide).\n"
                                              "Off removes its static body, so nothing collides with it.");
                        else
                            ImGui::SetTooltip("This entity is not one of the level's own placements, so\n"
                                              "the level would not save this setting.");
                    }
                    ImGui::EndDisabled();
                }
                // ANIMATION: the .ocworld anim/animspeed/animtime/animonce tokens -- a transform clip
                // this placement plays in Play (a car on its route, a fan). Whole-selection and one undo
                // entry per change, like Collides above (EditCmd::Animation). A skeletal mesh is left
                // out (its CAnimator is a skeletal clock), and nothing is editable while Play has object
                // animation live: it has already captured each entity's base.
                if (!w.hasComponent(selEntity_, scene::kComponentSkeletalMesh)) {
                    ImGui::SeparatorText("Animation");
                    if (levelIsLegacyOcmap_) {
                        ImGui::TextDisabled("The legacy .ocmap format cannot store an animation.");
                    } else {
                        std::vector<scene::Entity> targets;   // the selected meshes this section applies to
                        for (const scene::Entity ent : multiSelected)
                            if (w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer) &&
                                !w.hasComponent(ent, scene::kComponentSkeletalMesh))
                                targets.push_back(ent);
                        const auto animOf = [&](scene::Entity ent) {
                            const EntityAnim* a = entityAnim(ent);
                            return a ? *a : EntityAnim{};
                        };
                        const EntityAnim cur = animOf(selEntity_);
                        const bool live = anim::animSystem().objectAnimationLive();
                        // Only the level's own placements save an animation (saveLevel writes the tokens
                        // per levelEntities_ entry); on a class instance or preview an edit would be lost.
                        const bool levelOwned = isLevelOwned(selEntity_);
                        if (live) {
                            ImGui::TextDisabled("Stop Play to edit the animation.");
                        } else if (!levelOwned) {
                            ImGui::TextDisabled("Not saved: not one of the level's placements.");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("This entity is not one of the level's own placements (a class\n"
                                                  "instance or a preview), so the level would not save an animation on it.");
                        }
                        ImGui::BeginDisabled(live || !levelOwned);

                        // The clip, and why it would not move this mesh if it would not.
                        ImGui::TextDisabled("clip  %s", cur.clip.empty() ? "(none)" : cur.clip.c_str());
                        if (!cur.clip.empty()) {
                            const u64 clipId = fnv1a64(std::string_view(cur.clip));
                            const bool indexed = !content_.pathFor(clipId).empty();
                            const fmt::OcAnimation* clip = indexed ? anim::animSystem().clip(clipId) : nullptr;
                            if (!indexed)
                                ImGui::TextDisabled(ICON_WARNING " not found under the project's Content");
                            else if (!clip)
                                ImGui::TextDisabled(ICON_WARNING " the clip could not be loaded");
                            else if (!(clip->flags & fmt::kOcAnimObject))
                                ImGui::TextDisabled(ICON_WARNING " a skeletal clip: it will not move this mesh");
                            else
                                ImGui::TextDisabled("%.2f s", static_cast<double>(clip->duration));
                        }
                        if (ImGui::Button("Change Clip...")) ImGui::OpenPopup("##pickAnim");
                        uiReg_.track("details.anim.clip");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("A transform clip this mesh plays in Play: a car on a route, a fan.\n"
                                              "The placement is where it sits at the start time; the clip\n"
                                              "moves it from there. Saved with the level (anim).");
                        // Built only while the popup is open: listing means reading every project .ocanim
                        // once (AnimSystem caches them) to keep the object clips and drop the skeletal ones.
                        if (ImGui::IsPopupOpen("##pickAnim")) {
                            std::vector<AssetChoice> clips;
                            for (const auto& kv : content_.index()) {
                                if (lowerExt(std::filesystem::path(kv.second)) != ".ocanim") continue;
                                const fmt::OcAnimation* c = anim::animSystem().clip(kv.first);
                                if (c && (c->flags & fmt::kOcAnimObject))
                                    clips.push_back({cbRelativeToContent(kv.second), kv.first});
                            }
                            std::sort(clips.begin(), clips.end(),
                                      [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                            std::vector<AssetChoice> cands;
                            cands.push_back({"(None)", 0});
                            cands.insert(cands.end(), clips.begin(), clips.end());
                            u64 picked = 0;
                            const u64 currentId = cur.clip.empty() ? 0 : fnv1a64(std::string_view(cur.clip));
                            if (assetPicker("##pickAnim", cands, currentId, &picked)) {
                                // The path is the identity (id = fnv1a64 of it), so it is derived from the
                                // index's file and checked to hash back to the pick.
                                const std::string rel = picked ? cbRelativeToContent(content_.pathFor(picked))
                                                               : std::string();
                                if (picked && fnv1a64(std::string_view(rel)) != picked) {
                                    cbStatus_ = "Could not resolve the clip to a project-relative path";
                                } else {
                                    EditCmd c;
                                    c.kind = EditCmd::Kind::Animation;
                                    for (const scene::Entity ent : targets) {
                                        if (!isLevelOwned(ent)) continue;   // a mixed selection edits only what would be saved
                                        const EntityAnim before = animOf(ent);
                                        EntityAnim after = before;
                                        after.clip = rel;
                                        if (rel.empty()) after = EntityAnim{};   // None: back to the defaults
                                        if (before == after) continue;           // already there: no dead entry
                                        applyEntityAnim(ent, after);
                                        c.animation.push_back({editIdFor(ent), before, after});
                                    }
                                    if (!c.animation.empty()) pushEdit(std::move(c));
                                }
                            }
                        }

                        // Speed, start time and Play once tune the clips already on the selection (nothing
                        // to tune on an entity without one). A drag is ONE undo entry: animEditBefore_ is the
                        // selection as it stood when the widget was grabbed, before that frame's edit lands.
                        const auto snapshot = [&] {
                            animEditBefore_.clear();
                            for (const scene::Entity ent : targets)
                                if (const EntityAnim* a = entityAnim(ent))
                                    animEditBefore_.push_back({editIdFor(ent), *a, *a});
                        };
                        const auto commit = [&] {
                            EditCmd c;
                            c.kind = EditCmd::Kind::Animation;
                            for (EditCmd::AnimChange& ch : animEditBefore_) {
                                const scene::Entity ent = entityForEdit(ch.id);
                                const EntityAnim* now = w.valid(ent) ? entityAnim(ent) : nullptr;
                                if (!now || *now == ch.before) continue;
                                ch.after = *now;
                                c.animation.push_back(ch);
                            }
                            animEditBefore_.clear();
                            if (!c.animation.empty()) pushEdit(std::move(c));
                        };
                        const auto tune = [&](auto&& set) {
                            for (const scene::Entity ent : targets) {
                                const EntityAnim* a = entityAnim(ent);
                                if (!a) continue;
                                EntityAnim next = *a;
                                set(next);
                                applyEntityAnim(ent, next);
                            }
                        };
                        ImGui::BeginDisabled(cur.clip.empty());
                        f32 speed = cur.speed;
                        const bool speedEdited = ImGui::DragFloat("Speed##animSpeed", &speed, 0.01f, 0.01f, 100.0f, "%.2f");
                        if (ImGui::IsItemActivated() || (speedEdited && animEditBefore_.empty())) snapshot();
                        if (speedEdited) tune([&](EntityAnim& n) { n.speed = speed; });
                        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
                        uiReg_.track("details.anim.speed");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Playback speed. Saved with the level (animspeed).");
                        f32 startTime = cur.time;
                        const bool timeEdited = ImGui::DragFloat("Start time (s)##animTime", &startTime, 0.01f, 0.0f, 100000.0f, "%.2f");
                        if (ImGui::IsItemActivated() || (timeEdited && animEditBefore_.empty())) snapshot();
                        if (timeEdited) tune([&](EntityAnim& n) { n.time = startTime; });
                        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
                        uiReg_.track("details.anim.time");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Where in the clip the placement stands, in seconds.\n"
                                              "The mesh sits at its placement at this time and moves relative\n"
                                              "to it. Saved with the level (animtime).");
                        bool once = cur.once;
                        if (ImGui::Checkbox("Play once##animOnce", &once)) {
                            snapshot();
                            tune([&](EntityAnim& n) { n.once = once; });
                            commit();
                        }
                        uiReg_.track("details.anim.once");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Play through once and hold the last pose, instead of looping.\n"
                                              "Saved with the level (animonce).");
                        ImGui::EndDisabled();
                        ImGui::EndDisabled();
                    }
                }
#if AVER_MODULE_PHYSICS
                // VEHICLE: the `vehicle <preset>` token of a placement that drives in Play. Read-only, the
                // way the file's other authored-elsewhere tokens are shown: the generator writes it, and
                // the preset is not edited here.
                if (const std::string* vehicle = level_.vehiclePresetOf(selEntity_)) {
                    ImGui::TextDisabled("vehicle  %s", vehicle->c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("A physics car in Play (suspension, tyres, gravity, collisions), driven along\n"
                                          "the level's lanes (<level>.oclanes). Sized from this mesh's bounds and the\n"
                                          "preset. Set by the vehicle token on the placement; not editable here.");
                }
#endif
                // Now reassignable: previously just a printed hex id, with no picker and not even
                // a drop target -- the only mesh drag-drop in the editor lands on the 3D viewport
                // and spawns a NEW entity, a different verb.
                const auto meshIt = meshPathById_.find(mr->mesh);
                ImGui::TextDisabled("mesh  %s", meshIt == meshPathById_.end()
                                                    ? "(unloaded)" : meshIt->second.c_str());
                if (ImGui::Button("Change Mesh...")) ImGui::OpenPopup("##pickMesh");
                uiReg_.track("details.mesh.pick");
                // BUILT ONLY WHILE THE POPUP IS OPEN, like the animation picker above: this used to
                // copy and sort every loaded mesh path (thousands, in a big level) on every frame
                // the Details panel showed a mesh, with the popup shut. Kept across the frames it
                // stays open; see PickerCands.
                if (ImGui::IsPopupOpen("##pickMesh")) {
                    // Candidates are meshes actually loaded (from the map populated for this --
                    // "the foliage palette, a future asset picker"); listing every .ocmesh in the
                    // content index could offer one the device refused or never loaded, blanking
                    // the entity if picked.
                    PickerCands& pc = pickMeshCands_;
                    const int frame = ImGui::GetFrameCount();
                    if (pc.lastFrame < frame - 1 || pc.stamp != meshPathById_.size()) {
                        pc.list.clear();
                        pc.list.reserve(meshPathById_.size());
                        for (const auto& kv : meshPathById_) pc.list.push_back({kv.second, kv.first});
                        std::sort(pc.list.begin(), pc.list.end(),
                                  [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                        pc.stamp = meshPathById_.size();
                    }
                    pc.lastFrame = frame;
                    u64 picked = 0;
                    if (assetPicker("##pickMesh", pc.list, mr->mesh, &picked)) {
                        if (multiSelected.size() <= 1) {
                            assignMeshId(selEntity_, picked);
                        } else {
                            // Applies to the whole selection, same as Transform above: every
                            // entity with a CMeshRenderer takes the pick, not just the anchor.
                            // assignMeshId is already per-entity all-or-nothing (false, a no-op,
                            // for anything without the component), so this just loops it rather
                            // than reimplementing.
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
                } else {
                    pickMeshCands_.drop();
                }
            }

            // The material, which Details never offered: the scene stores a NAME TOKEN, not a
            // handle -- content_'s surface materials map between them once .ocmat files load.
            // The one lookup that stood between the panel and the entities anybody actually edits.
            // Named explicitly because the desc is SHARED: every entity using M_Floor edits the
            // same sliders (the material system working as designed, not a bug) -- which the
            // panel should say rather than let someone discover.
#if AVER_MODULE_PBR
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Same fallback the draw uses: without it, an entity drawn fine via the mesh's
                // own material would report "(none)" here -- the panel disagreeing with the picture.
                const i32 shown = mr->material ? mr->material : content_.meshDefaultMaterial(mr->mesh);
                const char* surfaceName = aver_scene_material_name(shown);

                // Previously unassignable at all -- no picker, and no drag either
                // (isPlaceableAssetExt refuses .ocmat): the panel could edit parameters but never
                // change WHICH material.
                if (ImGui::Button("Change Material...")) ImGui::OpenPopup("##pickMaterial");
                uiReg_.track("details.material.pick");
                // BUILT ONLY WHILE THE POPUP IS OPEN: editor::projectMaterialStems below ENUMERATES
                // the project's Materials directories, which this used to do on every frame the
                // Details panel showed a mesh entity, popup or not. Kept across the frames it stays
                // open (see PickerCands), so an open picker does not re-read the disk per frame.
                if (ImGui::IsPopupOpen("##pickMaterial")) {
                    // EVERY PROJECT MATERIAL STEM, not only the ones level-scoped residency
                    // (levelScopedMaterialsEnabled()) currently keeps bound: with that on, a level
                    // holds only its OWN surfaces in content_.surfaceMaterials(), and listing just
                    // that map here would make every other material in the project unreachable from
                    // this picker. editor::projectMaterialStems() is the same Binaries-then-Content
                    // enumeration loadProjectMaterials() used to drive; picking one that is not yet
                    // resident loads it on demand -- see assignMaterialToken()'s own comment.
                    //
                    // Tokens, not path hashes: content_'s surface map is keyed on the interned token
                    // mr->material holds; an fnv1a64 here could resolve to nothing, or worse, to an
                    // unrelated surface by coincidence. aver_scene_material() interns (or reuses) the
                    // token for a name without loading anything, so every stem gets one whether or
                    // not a .ocmat is behind it yet.
                    PickerCands& pc = pickMaterialCands_;
                    const int frame = ImGui::GetFrameCount();
                    if (pc.lastFrame < frame - 1 || pc.stamp != content_.surfaceMaterials().size()) {
                        std::unordered_set<i32> seen;
                        std::vector<AssetChoice>& cands = pc.list;
                        cands.clear();
                        const std::vector<std::string> stems =
                            editor::projectMaterialStems(project_.binariesDir(), project_.contentDir());
                        cands.reserve(stems.size());
                        for (const std::string& stem : stems) {
                            const i32 stemToken = aver_scene_material(0, stem.c_str());
                            if (!seen.insert(stemToken).second) continue;
                            cands.push_back({stem, static_cast<u64>(static_cast<u32>(stemToken))});
                        }
                        // A surface already bound (foliage, landscape, a placement override) whose name
                        // is not a bare stem under either Materials folder -- rare, but not listing it
                        // would make an already-assigned material vanish from its own picker.
                        for (const auto& kv : content_.surfaceMaterials()) {
                            if (!kv.second || !seen.insert(kv.first).second) continue;
                            // `matName`, not `nm`: the entity-name `nm` set above is still in scope
                            // here and would be shadowed (C4456) by an unrelated MATERIAL name.
                            const char* matName = aver_scene_material_name(kv.first);
                            cands.push_back({matName && *matName ? matName : "(unnamed)",
                                             static_cast<u64>(static_cast<u32>(kv.first))});
                        }
                        std::sort(cands.begin(), cands.end(),
                                  [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                        pc.stamp = content_.surfaceMaterials().size();
                    }
                    pc.lastFrame = frame;
                    u64 picked = 0;
                    if (assetPicker("##pickMaterial", pc.list,
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
                } else {
                    pickMaterialCands_.drop();
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
        // Visible/editable the same way CMeshRenderer is above; no picker beyond drag-drop exists
        // for CMeshRenderer::mesh either, so the emitter's effect id follows the same shape.
        if (auto* pe = w.component<scene::CParticleEmitter>(selEntity_, scene::kComponentParticleEmitter)) {
            if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool stopped = (pe->flags & scene::kParticleEmitterStopped) != 0;
                if (ImGui::Checkbox("Stopped", &stopped)) {
                    if (stopped) pe->flags |=  scene::kParticleEmitterStopped;
                    else         pe->flags &= ~scene::kParticleEmitterStopped;
                }
                ImGui::TextDisabled("effect id 0x%llx", (unsigned long long)pe->effect);
                // Drop a .ocparticle from the Content Browser to point this emitter at it -- same
                // id space content_.loadProjectParticleEffects() populates, so a freshly authored
                // effect resolves immediately. Through the shared helper (assignParticleEffect,
                // "two implementations of one fact") rather than a second resolve/validate/hash/
                // assign/mark/log copy -- the picker below would otherwise have been that second copy.
                // THE BROWSER'S ONE PAYLOAD: it sends its selection under kCbMoveDragDropType
                // (ImGui keeps one payload per drag, see the viewport drop target), so this row
                // takes the first .ocparticle in it; kAssetDragDropType is still accepted from a
                // single-asset source.
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
                // BUILT ONLY WHILE THE POPUP IS OPEN: the scan below walks the WHOLE content index
                // (every asset in the project) with a path parse per entry, and this used to run on
                // every frame the Details panel showed an emitter. Kept across the frames it stays
                // open (see PickerCands).
                if (ImGui::IsPopupOpen("##pickEffect")) {
                    // Candidates come from the content index, not a loaded-effects map: an effect
                    // resolves on ASSIGNMENT (unlike a mesh), so listing only loaded ones would
                    // hide the file the author just made.
                    // content_.index() maps id -> ABSOLUTE path (an earlier draft of this comment
                    // said relative -- it's not). Its KEY is fnv1a64 of the project-relative path
                    // (pe->effect's id space, so no hashing needed), but its VALUE is
                    // `it->path().string()` straight off the directory iterator (content_.adopt());
                    // the two disagree, which is why the label is derived rather than used as-is.
                    // Labelled relative, so this popup reads like the Mesh/Material ones above
                    // instead of the developer's whole local directory tree.
                    PickerCands& pc = pickEffectCands_;
                    const int frame = ImGui::GetFrameCount();
                    if (pc.lastFrame < frame - 1 || pc.stamp != content_.index().size()) {
                        std::vector<AssetChoice>& cands = pc.list;
                        cands.clear();
                        for (const auto& kv : content_.index()) {
                            if (lowerExt(std::filesystem::path(kv.second)) != ".ocparticle") continue;
                            cands.push_back({cbRelativeToContent(kv.second), kv.first});
                        }
                        std::sort(cands.begin(), cands.end(),
                                  [](const AssetChoice& a, const AssetChoice& b) { return a.label < b.label; });
                        pc.stamp = content_.index().size();
                    }
                    pc.lastFrame = frame;
                    u64 picked = 0;
                    if (assetPicker("##pickEffect", pc.list, pe->effect, &picked)) {
                        // Assigned by path, not by writing the id directly, so it goes through the
                        // same resolve/validate the drop does -- extension check and project-
                        // relative normalisation included (writing pe->effect would duplicate it).
                        // Passed straight through: content_.pathFor()'s value is already absolute.
                        // Joining it onto contentDir() first (as before) was correct only by
                        // accident (operator/ discards the left side when the right is absolute) --
                        // and would break the day that map starts storing relative paths, which its
                        // own key spelling suggests it should.
                        const std::string path = content_.pathFor(picked);
                        if (!path.empty())
                            assignParticleEffect(selEntity_, path);
                    }
                } else {
                    pickEffectCands_.drop();
                }
                ImGui::TextDisabled("age %.2fs   seed 0x%08x", pe->age, pe->seed);
            }
        }
#endif

#if AVER_MODULE_PHYSICS
        // ---- Soft Body, and the physics viewer for it ----
        // Reads the SIMULATION, not just the component: whether it's simulating, particle count,
        // skinned path, and whether it's drawn are answerable only from the live body/render feature.
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

                    // Where its particles actually are, so "is it simulating" needs no debugger:
                    // at rest and falling look identical in every other readout here.
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
                // Is it actually drawn? Simulating and drawing fail identically from the
                // viewport; a non-zero draw handle means simulated vertices replace the authored mesh.
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
        // Previously impossible from the editor: every panel above renders only when its
        // component already exists, so one no importer/template attaches needed a hand-edited file.
        ImGui::Separator();
        if (ImGui::Button(ICON_ADD " Add Component")) ImGui::OpenPopup("addComponent");
        if (ImGui::BeginPopup("addComponent")) {
            // Listed by hand, not walked from the registry: the registry knows name/size, nothing
            // about whether menu-attaching one is safe (CWorld/CLocal would corrupt an entity).
            struct Addable { u32 id; const char* name; const char* tip; };
            static const Addable kAddable[] = {
                {scene::kComponentSoftBody, ICON_TUNE " Soft Body",
                 "Simulate this entity's mesh instead of posing it: sag, drape, squash, collide."},
            };
            for (const Addable& a : kAddable) {
                const bool present = w.hasComponent(selEntity_, a.id);
                ImGui::BeginDisabled(present);
                if (ImGui::MenuItem(a.name)) {
                    // addComponent hands back zero-filled storage with no constructor run, so
                    // defaults are written here -- same fix as World::create(); CSynapseAgent hit this bug once.
                    if (a.id == scene::kComponentSoftBody) {
                        if (auto* c = static_cast<scene::CSoftBody*>(
                                w.addComponent(selEntity_, a.id)))
                            *c = scene::CSoftBody{};
                    }
                    cbStatus_ = std::string("Added ") + a.name;
                    // Not undoable, so at least mark dirty: adding pushes no EditCmd, so without
                    // this the level closed clean and the component was silently gone. Remove
                    // Component below has no such problem -- it captures the component and can be undoable.
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
        // scene::World::removeComponent was implemented and tested (tests/scene/src/SceneTest.cpp)
        // but had zero editor callers. kRemovable mirrors kAddable's allow-list for the same
        // reason: CLocal/CWorld/CHierarchy
        // make the row an entity at all, and removing one by menu would corrupt it, not simplify it.
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
                    // Undoable: removeComponentFromSelection captures the component byte-exact
                    // before dropping it, unlike Add Component above (no "before" to capture).
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
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("White (255, 255, 255) is the physical default -- the light\n"
                                  "arriving at the top of the atmosphere. The physical sky tints\n"
                                  "it toward orange by elevation on its own.");
        }
        ImGui::SliderFloat("Angular Size", &sky_.sunAngularDiameterDeg, 0.05f, 8.0f, "%.2f deg");
        // hasLevelSun_ lives in SandboxApp.hpp's AVER_MODULE_SCENE block beside loadLevel/
        // saveLevel: only a scene-on build ever runs the loader that populates it.
#if AVER_MODULE_SCENE
        markLevelRecordEdited(hasLevelSun_);
#endif
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
        // Range 0..32, logarithmic (used to stop at 2): the multiplier on the only term that fills
        // an enclosed space (sky light through openings), and an arcade needs far more of it than
        // a slider stopping at 2 can express. MEASURED on PTTest Sponza at exposure 1,
        // 3D-viewport luminance percentiles (sunlit surfaces reach 237 in every row):
        //
        //   Sky Light  1    p50   8   p90 48    <- median pixel essentially black
        //   Sky Light  4    p50  15   p90 48
        //   Sky Light 16    p50  34   p90 75    <- shadows readable, sun unchanged
        //
        // The sun itself is correctly scaled (237/255 with no exposure); the fill just couldn't
        // reach the value the scene needed -- so "everything is too dark" was, in part,
        // unauthorable. Logarithmic: the useful range spans two orders of magnitude and the
        // interesting end is the bottom.
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
        // FOG is its own record and needs its own mark: markLevelRecordEdited went in for SUN/SKY
        // but missed FOG, silently discarding Fog Tint/Density on a level with no FOG line.
        // Guarded for the same reason as hasLevelSun_ above.
#if AVER_MODULE_SCENE
        markLevelRecordEdited(hasLevelFog_);
        if (streaming_.enabled()) {
            const world::StreamSettings& mst = streaming_.settings().stream;
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
        // CLOUDS is its own record, same reason as FOG. The Clouds checkbox sits above the `if`
        // deliberately: turning clouds OFF must reach the file, so marking only inside the
        // enabled branch would make "off" the one setting that could never be saved.
#if AVER_MODULE_SCENE
        markLevelRecordEdited(hasLevelClouds_);
        markLevelRecordEdited(hasLevelSky_);
#endif
        // Water lives beside the Sky, not its own mode: it's a level property like sun/fog, and
        // the weather panel is where an author looks for it. No markLevelRecordEdited needed --
        // unlike SUN/FOG it rides through saveLevel as part of levelHeader_, so editing IS the edit.
        ImGui::Separator();
        buildWaterPanel(e);
    } else if (sel_==-4){
        ImGui::TextUnformatted("Post Process"); ImGui::Separator();
        ImGui::Checkbox("Eye Adaptation", &post_.autoExposure);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The exposure adjusts automatically as the view gets brighter or\n"
                              "darker, like eyes -- quickly toward light, more slowly toward dark.");
        // Stops, not the raw linear post_.exposure: a linear drag has no scale, which is how one
        // landed at 4.05 (+2 stops) and masked the engine's tuned defaults (RHI.hpp) every launch.
        // EV reads the same whether it's compensation on Eye Adaptation or a fixed exposure.
        f32 ev = (post_.exposure > 0.0f) ? std::log2(post_.exposure) : 0.0f;
        if (ImGui::SliderFloat("Brightness", &ev, -3.0f, 3.0f, "%+.1f EV"))
            post_.exposure = std::exp2(ev);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(post_.autoExposure
                ? "Compensation on top of Eye Adaptation's own exposure:\n"
                  "0 = as tuned, +1 = twice as bright, -1 = half."
                : "Fixed exposure relative to 1, with Eye Adaptation off:\n"
                  "0 = as tuned, +1 = twice as bright, -1 = half.");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) post_.exposure = 1.0f;
        if (post_.autoExposure) {
            // Live readout, so a stuck-looking value reads as adaptation working, not broken (see
            // IDevice::postExposureReadout; it lags the eye by a frame or two). Clamps/histogram
            // cuts are tuned engine defaults (RHI.hpp), not panel-exposed dials.
            f32 metered = 0.0f;
            if (e.device() && e.device()->postExposureReadout(metered))
                ImGui::TextDisabled("Adapted to %+.1f EV", std::log2(metered));
        }
        // NIGHT VISION works on a fixed exposure too (Krawczyk/Myszkowski/Seidel 2005, rod-driven
        // scotopic vision), so it sits beside Eye Adaptation rather than inside its block.
        bool nightVision = post_.nightVision > 0.5f;
        if (ImGui::Checkbox("Night Vision", &nightVision))
            post_.nightVision = nightVision ? 1.0f : 0.0f;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("In very dim light colour fades and shifts blue-grey as the eye's\n"
                              "rods take over.");
        ImGui::Separator();
        ImGui::SliderFloat("Bloom", &post_.bloomIntensity, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zero skips the whole bloom pyramid, not just its weight");
    } else ImGui::TextDisabled("Select an actor in the World Outliner");
    ImGui::End();

}

#endif

#if AVER_MODULE_SCENE
#if AVER_WITH_IMGUI
// A pure-ImGui debug window. Guarded because uiActive() is a RUNTIME test and cannot make the
// ImGui:: names exist for the compiler -- see the mouse-capture block in onUpdate for the same trap.
void SandboxApp::buildChunkStreamingPanel() {
    if (!streaming_.enabled()) return;
    const world::StreamStats& s = streaming_.stats();
    // Anchored to the viewport, not the window's corner: a fixed (12, 60) from the window origin
    // used to overlap the mode panel's tool buttons once a docked panel appeared on the left.
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
    ImGui::TextDisabled("%s", streaming_.settings().worldDir.c_str());
    ImGui::End();
}

#endif
#endif

} // namespace aver
