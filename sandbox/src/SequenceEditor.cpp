// The level sequence editor behind the Animate mode (SequenceEditor.hpp).
#include "SequenceEditor.hpp"

#if AVER_MODULE_SCENE
#include "aver/core/Log.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/scene/World.hpp"

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace aver::editor {

using fmt::OcSeqInterp;
using fmt::OcSeqKey;
using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

namespace {

constexpr f64 kSameKeySec = 1e-3;   // a key this close to the playhead is the same key

i32 targetOf(scene::Entity e) { return static_cast<i32>(static_cast<u32>(e)); }
scene::Entity entityOfTarget(i32 t) { return static_cast<scene::Entity>(static_cast<u32>(t)); }

#if AVER_WITH_IMGUI
ImU32 kindColor(OcSeqTrackKind k) {
    return k == OcSeqTrackKind::Transform ? IM_COL32(242, 150, 60, 255)
         : k == OcSeqTrackKind::Camera    ? IM_COL32(110, 180, 255, 255)
                                          : IM_COL32(240, 220, 90, 255);
}
#endif

} // namespace

// ---- level lifecycle ---------------------------------------------------------------------------

void SequenceEditor::reset() {
    model_ = fmt::OcSequence{};
    model_.name = "Main";
    player_ = game::SequencePlayer{};
    bases_.clear();
    playRunning_ = false;
    evalDirty_ = true;
    editTime_ = 0;
    selTrack_ = -1;
    selKey_ = -1;
    dragKey_ = dragRuler_ = false;
    status_.clear();
    syncPlayer();
}

void SequenceEditor::load(const std::vector<fmt::OcSequence>& seqs,
                          const std::vector<scene::Entity>& placementEntities) {
    reset();
    if (seqs.empty()) return;
    if (seqs.size() > 1)
        AVER_WARN("[Sequence] the level has {} sequences; the editor keeps the first and drops the rest",
                  (u32)seqs.size());
    scene::World& w = scene::World::instance();
    model_ = seqs.front();
    if (model_.name.empty()) model_.name = "Main";
    u32 dropped = 0;
    bool haveCamera = false;
    std::vector<OcSeqTrack> kept;
    for (OcSeqTrack& tr : model_.tracks) {
        if (tr.kind == OcSeqTrackKind::Camera) {
            if (haveCamera) { ++dropped; continue; }   // one camera track
            haveCamera = true;
            tr.target = -1;
        } else {
            const bool inRange = tr.target >= 0 && static_cast<usize>(tr.target) < placementEntities.size();
            const scene::Entity e = inRange ? placementEntities[static_cast<usize>(tr.target)] : scene::kInvalidEntity;
            if (e == scene::kInvalidEntity || !w.valid(e)) { ++dropped; continue; }
            tr.target = targetOf(e);
        }
        std::stable_sort(tr.keys.begin(), tr.keys.end(),
                         [](const OcSeqKey& a, const OcSeqKey& b) { return a.t < b.t; });
        kept.push_back(std::move(tr));
    }
    model_.tracks = std::move(kept);
    syncPlayer();
    AVER_INFO("[Sequence] loaded '{}': {} track(s), {:.1f} s{}", model_.name, (u32)model_.tracks.size(),
              model_.length, dropped ? " (some tracks named missing placements and were dropped)" : "");
}

void SequenceEditor::save(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf) const {
    w.sequences.clear();
    fmt::OcSequence out = model_;
    out.tracks.clear();
    for (const OcSeqTrack& tr : model_.tracks) {
        OcSeqTrack c = tr;
        if (tr.kind != OcSeqTrackKind::Camera) {
            const auto it = slotOf.find(static_cast<u32>(tr.target));
            if (it == slotOf.end()) continue;
            c.target = it->second;
        } else {
            c.target = -1;
        }
        out.tracks.push_back(std::move(c));
    }
    if (out.tracks.empty()) return;
    w.sequences.push_back(std::move(out));
}

// ---- mode --------------------------------------------------------------------------------------

void SequenceEditor::enter() {
    active_ = true;
    evalDirty_ = true;
    syncPlayer();
}

void SequenceEditor::leave() {
    restoreBases();
    active_ = false;
    dragKey_ = dragRuler_ = false;
    player_.pause();
}

// ---- frame -------------------------------------------------------------------------------------

void SequenceEditor::tick(const SequenceTickCtx& c) {
    playActiveLast_ = c.playActive;
    if (playRunning_) {
        if (!c.playActive) { endPlay(); return; }   // the session never started or ended without a Stop
        if (!c.playPaused) { player_.advance(c.dt); evalDirty_ = true; }
        if (evalDirty_) evaluateNow();
        return;
    }
    if (!active_ || c.playActive) return;
    if (player_.playing()) { player_.advance(c.dt); evalDirty_ = true; }
    if (evalDirty_) { captureMissingBases(); evaluateNow(); }
}

void SequenceEditor::evaluateNow() {
    player_.evaluate(scene::World::instance());
    evalDirty_ = false;
}

void SequenceEditor::captureBase(scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    bases_.emplace(static_cast<u32>(e), w.localTransform(e));
}

void SequenceEditor::captureMissingBases() {
    for (const scene::Entity e : player_.transformEntities()) captureBase(e);
}

void SequenceEditor::restoreBases() {
    if (bases_.empty()) return;
    scene::World& w = scene::World::instance();
    for (const auto& kv : bases_) {
        const scene::Entity e = static_cast<scene::Entity>(kv.first);
        if (w.valid(e)) w.setLocalTransform(e, kv.second);
    }
    bases_.clear();
    evalDirty_ = true;
}

// ---- Play --------------------------------------------------------------------------------------

void SequenceEditor::beginPlay(game::PlayMobility& mobility) {
    playRunning_ = false;
    player_.pause();
    if (!model_.autoplay || model_.tracks.empty()) return;
    syncPlayer();
    if (player_.empty()) return;
    editTime_ = player_.time();
    player_.setTime(0);
    player_.play();
    mobility.seedMovable(player_.boundEntities());
    playRunning_ = true;
    evalDirty_ = true;
}

void SequenceEditor::endPlay() {
    if (!playRunning_) return;
    playRunning_ = false;
    player_.pause();
    player_.setTime(editTime_);
    evalDirty_ = true;
}

bool SequenceEditor::drivesTransform(scene::Entity e) const {
    for (const OcSeqTrack& tr : model_.tracks)
        if (tr.kind == OcSeqTrackKind::Transform && tr.target == targetOf(e)) return true;
    return false;
}

// ---- view --------------------------------------------------------------------------------------

bool SequenceEditor::viewPose(bool playEjected, game::SeqCameraPose& out) const {
    if (playRunning_) return !playEjected && model_.camera && player_.camera(out);
    if (active_ && pilot_ && !playActiveLast_) return player_.camera(out);
    return false;
}

bool SequenceEditor::emissiveScale(scene::Entity e, f32 out[3]) const {
    return previewing() && player_.emissiveScale(e, out);
}

// ---- model -------------------------------------------------------------------------------------

// The player gets a copy of the model with each target rewritten to an index into a dense entity
// list; a track whose entity is gone is left out of the copy, not out of the model.
void SequenceEditor::syncPlayer() {
    scene::World& w = scene::World::instance();
    std::vector<scene::Entity> dense;
    std::unordered_map<u32, i32> index;
    fmt::OcSequence seq = model_;
    seq.tracks.clear();
    for (const OcSeqTrack& tr : model_.tracks) {
        OcSeqTrack c = tr;
        if (tr.kind != OcSeqTrackKind::Camera) {
            const scene::Entity e = entityOfTarget(tr.target);
            if (!w.valid(e)) continue;
            auto it = index.find(static_cast<u32>(e));
            if (it == index.end()) {
                it = index.emplace(static_cast<u32>(e), static_cast<i32>(dense.size())).first;
                dense.push_back(e);
            }
            c.target = it->second;
        }
        seq.tracks.push_back(std::move(c));
    }
    player_.setSequence(seq);
    player_.bind(dense);
    player_.setTime(player_.time());   // re-wrapped against the (possibly new) length
}

void SequenceEditor::edited() {
    syncPlayer();
    evalDirty_ = true;
    if (host_ && host_->markLevelDirty) host_->markLevelDirty();
}

int SequenceEditor::findTrack(OcSeqTrackKind kind, scene::Entity e) const {
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        if (tr.kind == kind && (kind == OcSeqTrackKind::Camera || tr.target == targetOf(e)))
            return static_cast<int>(i);
    }
    return -1;
}

fmt::OcSeqKey* SequenceEditor::selectedKey() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) return nullptr;
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    if (selKey_ < 0 || static_cast<usize>(selKey_) >= tr.keys.size()) return nullptr;
    return &tr.keys[static_cast<usize>(selKey_)];
}

void SequenceEditor::clampSelection() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) { selTrack_ = -1; selKey_ = -1; return; }
    if (selKey_ >= static_cast<int>(model_.tracks[static_cast<usize>(selTrack_)].keys.size())) selKey_ = -1;
}

// The key's values from the live scene: the actor's transform, the editor camera, or (for a new
// material key) a neutral 1,1,1 x 1.
void SequenceEditor::fillKey(const OcSeqTrack& tr, OcSeqKey& key) const {
    switch (tr.kind) {
        case OcSeqTrackKind::Transform: {
            scene::World& w = scene::World::instance();
            const scene::Entity e = entityOfTarget(tr.target);
            if (w.valid(e)) game::seqKeyFromTransform(w.localTransform(e), key);
            break;
        }
        case OcSeqTrackKind::Camera:
            if (host_) game::seqKeyFromCamera(host_->editorCamera, key);
            break;
        case OcSeqTrackKind::Material:
            key.v[0] = key.v[1] = key.v[2] = key.v[3] = 1.0;
            break;
    }
}

std::string SequenceEditor::trackLabel(const OcSeqTrack& tr) const {
    if (tr.kind == OcSeqTrackKind::Camera) return "Camera";
    const scene::Entity e = entityOfTarget(tr.target);
    std::string name;
    if (!scene::World::instance().valid(e)) name = "<deleted>";
    else if (host_ && host_->label) name = host_->label(e);
    else name = "Entity " + std::to_string(static_cast<u32>(e));
    return tr.kind == OcSeqTrackKind::Material ? name + " (emissive)" : name;
}

// Insert a key at t, keeping the keys sorted; returns its index.
static int insertKeySorted(OcSeqTrack& tr, const OcSeqKey& k) {
    const auto it = std::upper_bound(tr.keys.begin(), tr.keys.end(), k.t,
                                     [](f64 t, const OcSeqKey& a) { return t < a.t; });
    return static_cast<int>(tr.keys.insert(it, k) - tr.keys.begin());
}

void SequenceEditor::addTrack(OcSeqTrackKind kind, scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (kind != OcSeqTrackKind::Camera && !w.valid(e)) return;
    const int existing = findTrack(kind, e);
    if (existing >= 0) { selTrack_ = existing; selKey_ = -1; return; }
    OcSeqTrack tr;
    tr.kind = kind;
    tr.target = kind == OcSeqTrackKind::Camera ? -1 : targetOf(e);
    // The base is the actor as authored, taken before this track can pose it.
    if (kind == OcSeqTrackKind::Transform) captureBase(e);
    // Seeded with a key at the playhead so the track does something at once.
    OcSeqKey k;
    k.t = std::clamp(player_.time(), 0.0, model_.length);
    fillKey(tr, k);
    tr.keys.push_back(k);
    model_.tracks.push_back(std::move(tr));
    selTrack_ = static_cast<int>(model_.tracks.size()) - 1;
    selKey_ = 0;
    edited();
}

void SequenceEditor::removeTrack(int index) {
    if (index < 0 || static_cast<usize>(index) >= model_.tracks.size()) return;
    const OcSeqTrack gone = model_.tracks[static_cast<usize>(index)];
    model_.tracks.erase(model_.tracks.begin() + index);
    // An actor with no transform track left goes back to where it was authored.
    if (gone.kind == OcSeqTrackKind::Transform && !drivesTransform(entityOfTarget(gone.target))) {
        const auto it = bases_.find(static_cast<u32>(gone.target));
        if (it != bases_.end()) {
            scene::World& w = scene::World::instance();
            const scene::Entity e = entityOfTarget(gone.target);
            if (w.valid(e)) w.setLocalTransform(e, it->second);
            bases_.erase(it);
        }
    }
    if (selTrack_ == index) { selTrack_ = -1; selKey_ = -1; }
    else if (selTrack_ > index) --selTrack_;
    edited();
}

void SequenceEditor::addKeyAtPlayhead() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) {
        status_ = "Select a track to key";
        return;
    }
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    const f64 t = player_.time();
    int idx = -1;
    for (usize i = 0; i < tr.keys.size(); ++i)
        if (std::fabs(tr.keys[i].t - t) < kSameKeySec) { idx = static_cast<int>(i); break; }
    if (idx >= 0) {
        // Re-keying: new pose, same time and interpolation. A material key keeps its colour; the
        // panel edits that.
        if (tr.kind != OcSeqTrackKind::Material) fillKey(tr, tr.keys[static_cast<usize>(idx)]);
    } else {
        OcSeqKey k;
        k.t = t;
        fillKey(tr, k);
        idx = insertKeySorted(tr, k);
    }
    selKey_ = idx;
    char buf[160];
    std::snprintf(buf, sizeof buf, "Key at %.2f s on %s", t, trackLabel(tr).c_str());
    status_ = buf;
    edited();
}

void SequenceEditor::removeSelectedKey() {
    if (!selectedKey()) return;
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    tr.keys.erase(tr.keys.begin() + selKey_);
    selKey_ = -1;
    edited();
}

// Retimes a key; the list is re-sorted, so the key's index can change. Returns the new index.
int SequenceEditor::moveKey(OcSeqTrack& tr, int index, f64 t) {
    OcSeqKey k = tr.keys[static_cast<usize>(index)];
    k.t = std::clamp(t, 0.0, model_.length);
    tr.keys.erase(tr.keys.begin() + index);
    return insertKeySorted(tr, k);
}

// ---- UI ----------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

void SequenceEditor::drawModePanel(SequenceHost& host) {
    host_ = &host;
    const std::vector<scene::Entity>& sel = host.selection;

    ImGui::TextDisabled("SEQUENCE");
    f32 len = static_cast<f32>(model_.length);
    ImGui::TextUnformatted("Length");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##seqLen", &len, 0.1f, 0.5f, 3600.0f, "%.1f s")) {
        model_.length = std::fmax(0.5f, len);
        edited();
    }
    bool b = model_.loop;
    if (ImGui::Checkbox("Loop", &b)) { model_.loop = b; edited(); }
    b = model_.autoplay;
    if (ImGui::Checkbox("Autoplay in Play", &b)) { model_.autoplay = b; edited(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts from 0 when Play starts, and with the packaged game");
    b = model_.camera;
    if (ImGui::Checkbox("Use sequence camera in Play", &b)) { model_.camera = b; edited(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The camera track drives the view while the sequence plays");

    ImGui::Spacing();
    ImGui::TextDisabled("TRACKS");
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::Button("+ Transform track (selected actor)", ImVec2(-1, 0)))
        for (const scene::Entity e : sel) addTrack(OcSeqTrackKind::Transform, e);
    if (ImGui::Button("+ Emissive track (selected actor)", ImVec2(-1, 0)))
        for (const scene::Entity e : sel) addTrack(OcSeqTrackKind::Material, e);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity) >= 0);
    if (ImGui::Button("+ Camera track", ImVec2(-1, 0))) addTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);
    ImGui::EndDisabled();

    int removeIdx = -1;
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kindColor(tr.kind)));
        ImGui::TextUnformatted(tr.kind == OcSeqTrackKind::Transform ? "T"
                             : tr.kind == OcSeqTrackKind::Camera ? "C" : "E");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const f32 xW = ImGui::GetFrameHeight();
        const std::string label = trackLabel(tr) + "  (" + std::to_string(tr.keys.size()) + ")";
        if (ImGui::Selectable(label.c_str(), selTrack_ == static_cast<int>(i), 0,
                              ImVec2(ImGui::GetContentRegionAvail().x - xW - 4.0f, 0))) {
            selTrack_ = static_cast<int>(i);
            selKey_ = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIdx = static_cast<int>(i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this track");
        ImGui::PopID();
    }
    if (removeIdx >= 0) removeTrack(removeIdx);
    if (model_.tracks.empty())
        ImGui::TextWrapped("Select an actor, then add a track. K keys the selected track at the playhead.");

    // The selected key.
    if (OcSeqKey* key = selectedKey()) {
        OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
        ImGui::Spacing();
        ImGui::TextDisabled("KEY");
        f32 t = static_cast<f32>(key->t);
        ImGui::TextUnformatted("Time");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##keyT", &t, 0.01f, 0.0f, static_cast<f32>(model_.length), "%.2f s")) {
            selKey_ = moveKey(tr, selKey_, t);
            edited();
        }
        key = selectedKey();
        if (key) {
            int interp = static_cast<int>(key->interp);
            ImGui::TextUnformatted("Interpolation to next key");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##keyInterp", &interp, "Smooth\0Linear\0Step\0")) {
                key->interp = static_cast<OcSeqInterp>(interp);
                edited();
            }
            if (tr.kind == OcSeqTrackKind::Material) {
                f32 col[3] = {static_cast<f32>(key->v[0]), static_cast<f32>(key->v[1]), static_cast<f32>(key->v[2])};
                ImGui::TextUnformatted("Emissive colour");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::ColorEdit3("##keyCol", col, ImGuiColorEditFlags_Float)) {
                    key->v[0] = col[0]; key->v[1] = col[1]; key->v[2] = col[2];
                    edited();
                }
                f32 inten = static_cast<f32>(key->v[3]);
                ImGui::TextUnformatted("Intensity");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##keyInt", &inten, 0.05f, 0.0f, 1000.0f, "%.2f")) {
                    key->v[3] = std::fmax(0.0f, inten);
                    edited();
                }
            } else if (tr.kind == OcSeqTrackKind::Transform) {
                ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
                ImGui::TextDisabled("rot %.0f %.0f %.0f", key->v[3], key->v[4], key->v[5]);
                ImGui::TextDisabled("scl %.2f %.2f %.2f", key->v[6], key->v[7], key->v[8]);
                if (ImGui::Button("Set from actor now", ImVec2(-1, 0))) { fillKey(tr, *key); edited(); }
            } else {
                ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
                ImGui::TextDisabled("yaw %.1f  pitch %.1f", key->v[3], key->v[4]);
                if (ImGui::Button("Set from viewport camera", ImVec2(-1, 0))) { fillKey(tr, *key); edited(); }
            }
            if (ImGui::Button("Delete key", ImVec2(-1, 0))) removeSelectedKey();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("VIEW");
    ImGui::Checkbox("Pilot camera in viewport", &pilot_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The viewport shows the sequence camera at the playhead.\n"
                          "Untick it to fly the editor camera, then press K to key that pose.");
    game::SeqCameraPose pose;
    ImGui::BeginDisabled(!player_.camera(pose));
    if (ImGui::Button("Fly editor camera to playhead", ImVec2(-1, 0)) && host.setEditorCamera)
        host.setEditorCamera(pose);
    ImGui::EndDisabled();
    if (!status_.empty()) { ImGui::Spacing(); ImGui::TextDisabled("%s", status_.c_str()); }
    host_ = nullptr;
}

void SequenceEditor::drawTimeline(SequenceHost& host) {
    host_ = &host;
    const f32 ui = ImGui::GetFontSize() / 16.0f;
    const f32 winH = std::fmax(190.0f * ui, host.vpH * 0.3f);
    ImGui::SetNextWindowPos(ImVec2(host.vpX + 8.0f, host.vpY + host.vpH - winH - 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::fmax(360.0f * ui, host.vpW - 16.0f), winH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        host_ = nullptr;
        return;
    }
    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGuiIO& io = ImGui::GetIO();

    // ---- transport ----
    const f64 len = std::fmax(model_.length, 0.01);
    ImGui::BeginDisabled(playActiveLast_);
    if (ImGui::Button("|<")) { player_.pause(); player_.setTime(0); evalDirty_ = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("To start");
    ImGui::SameLine();
    auto togglePlay = [&]() {
        if (player_.playing()) player_.pause();
        else {
            if (!model_.loop && player_.time() >= len - 1e-6) player_.setTime(0);
            player_.play();
        }
    };
    if (ImGui::Button(player_.playing() ? "Pause" : "Play ")) togglePlay();
    ImGui::SameLine();
    bool loop = model_.loop;
    if (ImGui::Checkbox("Loop", &loop)) { model_.loop = loop; edited(); }
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2f s", player_.time(), model_.length);
    ImGui::SameLine();
    if (ImGui::Button("Key")) addKeyAtPlayhead();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key the selected track at the playhead  (K)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (playActiveLast_) ImGui::TextDisabled("Play is running");
    else if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // ---- hotkeys ----
    if (!playActiveLast_ && (winFocused || host.levelFocused) && !io.WantTextInput && !io.KeyCtrl &&
        !io.KeyAlt && !io.MouseDown[1]) {
        if (ImGui::IsKeyPressed(ImGuiKey_K, false)) addKeyAtPlayhead();
        if (winFocused && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) removeSelectedKey();
        if (winFocused && ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
    }

    // ---- ruler ----
    const f32 nameW = 170.0f * ui;
    const f32 rowH = ImGui::GetTextLineHeightWithSpacing() + 4.0f * ui;
    const f32 laneW = std::fmax(80.0f * ui, ImGui::GetContentRegionAvail().x - nameW - ImGui::GetStyle().ScrollbarSize - 6.0f * ui);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d track(s)", static_cast<int>(model_.tracks.size()));
    ImGui::SameLine(nameW);
    ImGui::InvisibleButton("##ruler", ImVec2(laneW, rowH));
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    auto laneX = [&](f64 t) { return r0.x + static_cast<f32>(t / len) * (r1.x - r0.x); };
    dl->AddRectFilled(r0, r1, IM_COL32(36, 36, 42, 255));
    {
        static const f64 kSteps[] = {0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300};
        f64 step = kSteps[std::size(kSteps) - 1];
        for (const f64 s : kSteps)
            if (static_cast<f32>(s / len) * (r1.x - r0.x) >= 70.0f * ui) { step = s; break; }
        for (f64 t = 0; t <= len + 1e-6; t += step) {
            const f32 x = laneX(t);
            dl->AddLine(ImVec2(x, r1.y - 7.0f * ui), ImVec2(x, r1.y), IM_COL32(150, 150, 160, 255));
            char tb[24];
            std::snprintf(tb, sizeof tb, "%g", t);
            dl->AddText(ImVec2(x + 3.0f * ui, r0.y), IM_COL32(170, 170, 178, 255), tb);
        }
    }
    if (ImGui::IsItemActivated()) dragRuler_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragRuler_ = false;
    if (dragRuler_ && !playActiveLast_) {
        const f64 u = std::clamp(static_cast<f64>((io.MousePos.x - r0.x) / std::fmax(1.0f, r1.x - r0.x)), 0.0, 1.0);
        player_.pause();
        player_.setTime(model_.loop ? std::min(u * len, len * 0.9999) : u * len);
        evalDirty_ = true;
    }
    {
        const f32 x = laneX(player_.time());
        const ImVec2 tri[3] = {ImVec2(x - 5.0f * ui, r0.y), ImVec2(x + 5.0f * ui, r0.y), ImVec2(x, r0.y + 9.0f * ui)};
        dl->AddConvexPolyFilled(tri, 3, IM_COL32(255, 190, 70, 255));
    }

    // ---- track lanes ----
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool laneChild = ImGui::BeginChild("##seqLanes", ImVec2(0, 0), false);
    ImGui::PopStyleVar();
    int removeIdx = -1;
    if (laneChild) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        if (model_.tracks.empty())
            ImGui::TextDisabled("No tracks. Select an actor and add one from the Mode panel.");
        const f32 hitR = 8.0f * ui;
        for (usize i = 0; i < model_.tracks.size(); ++i) {
            OcSeqTrack& tr = model_.tracks[i];
            const bool trackSel = selTrack_ == static_cast<int>(i);
            ImGui::PushID(static_cast<int>(i));
            const std::string name = trackLabel(tr);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kindColor(tr.kind)));
            if (ImGui::Selectable(name.c_str(), trackSel, 0, ImVec2(nameW - 6.0f * ui, rowH))) {
                selTrack_ = static_cast<int>(i);
                selKey_ = -1;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(nameW);
            ImGui::InvisibleButton("##lane", ImVec2(laneW, rowH));
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
            const f32 cy = (p0.y + p1.y) * 0.5f;
            auto lx = [&](f64 t) { return p0.x + static_cast<f32>(t / len) * (p1.x - p0.x); };

            cdl->AddRectFilled(p0, p1, trackSel ? IM_COL32(46, 40, 36, 255) : IM_COL32(30, 30, 34, 255));
            if (tr.keys.size() >= 2)
                cdl->AddLine(ImVec2(lx(tr.keys.front().t), cy), ImVec2(lx(tr.keys.back().t), cy),
                             IM_COL32(120, 120, 130, 140), 2.0f);
            const ImU32 col = kindColor(tr.kind);
            for (usize k = 0; k < tr.keys.size(); ++k) {
                const f32 x = lx(tr.keys[k].t);
                const bool ks = trackSel && selKey_ == static_cast<int>(k);
                const f32 r = (ks ? 6.0f : 4.5f) * ui;
                if (tr.keys[k].interp == OcSeqInterp::Step) {
                    cdl->AddRectFilled(ImVec2(x - r * 0.8f, cy - r * 0.8f), ImVec2(x + r * 0.8f, cy + r * 0.8f),
                                       ks ? IM_COL32(255, 255, 255, 255) : col);
                } else {
                    const ImVec2 d[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
                    cdl->AddConvexPolyFilled(d, 4, ks ? IM_COL32(255, 255, 255, 255) : col);
                    cdl->AddPolyline(d, 4, IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, 1.0f);
                }
            }
            if (trackSel) cdl->AddRect(p0, p1, IM_COL32(255, 220, 140, 150));
            const f32 phx = lx(player_.time());
            cdl->AddLine(ImVec2(phx, p0.y), ImVec2(phx, p1.y), IM_COL32(255, 190, 70, 170), 1.0f);

            // Press decides once what it landed on; the drag then follows that key.
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                int hit = -1;
                f32 best = hitR;
                for (usize k = 0; k < tr.keys.size(); ++k) {
                    const f32 d = std::fabs(io.MousePos.x - lx(tr.keys[k].t));
                    if (d <= best) { best = d; hit = static_cast<int>(k); }
                }
                selTrack_ = static_cast<int>(i);
                selKey_ = hit;
                dragKey_ = hit >= 0;
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { selTrack_ = static_cast<int>(i); }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragKey_ = false;
            if (dragKey_ && active && trackSel && selKey_ >= 0 && static_cast<usize>(selKey_) < tr.keys.size()) {
                const f64 u = std::clamp(static_cast<f64>((io.MousePos.x - p0.x) / std::fmax(1.0f, p1.x - p0.x)), 0.0, 1.0);
                const f64 nt = std::round(u * len * 100.0) / 100.0;
                if (std::fabs(nt - tr.keys[static_cast<usize>(selKey_)].t) > 1e-9) {
                    selKey_ = moveKey(tr, selKey_, nt);
                    edited();
                }
            }

            if (ImGui::BeginPopupContextItem("##laneCtx")) {
                if (ImGui::MenuItem("Key at playhead")) { selTrack_ = static_cast<int>(i); addKeyAtPlayhead(); }
                if (ImGui::MenuItem("Delete selected key", nullptr, false, trackSel && selectedKey() != nullptr))
                    removeSelectedKey();
                if (ImGui::MenuItem("Remove track")) removeIdx = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (removeIdx >= 0) removeTrack(removeIdx);
    clampSelection();
    ImGui::End();
    host_ = nullptr;
}

#else  // !AVER_WITH_IMGUI

void SequenceEditor::drawModePanel(SequenceHost&) {}
void SequenceEditor::drawTimeline(SequenceHost&) {}

#endif

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
