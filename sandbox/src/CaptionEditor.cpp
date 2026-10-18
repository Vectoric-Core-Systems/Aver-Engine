// The .occaptions editor tab. See the header for the two-line SandboxApp hook and why the save merges onto the file.
#include "CaptionEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "EditorKeybinds.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {
namespace {
CaptionEditorHooks g_hooks;

std::string lowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string stemOf(const std::string& path) {
    const usize slash = path.find_last_of("/\\");
    std::string f = slash == std::string::npos ? path : path.substr(slash + 1);
    const usize dot = f.rfind('.');
    if (dot != std::string::npos && dot > 0) f.resize(dot);
    return f;
}

bool looksLikeVoice(const std::string& path) {
    const std::string p = "/" + lowerStr(path);
    return p.find("/vo/") != std::string::npos || p.find("/voice") != std::string::npos || p.find("/dialog") != std::string::npos ||
           p.find("/speech/") != std::string::npos;
}

// A cue id is one bare word in the file: the ends are trimmed, anything in the middle that would split or comment it becomes '_'.
std::string cleanId(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    usize a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    s.erase(0, a);
    for (char& c : s) if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '#') c = '_';
    return s;
}
} // namespace

void setCaptionEditorHooks(CaptionEditorHooks hooks) { g_hooks = std::move(hooks); }

CaptionEditor::CaptionEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void CaptionEditor::loadFromDisk() {
    std::string text;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "could not read " + path_;
        return;
    }
    fmt::OcCaptionData loaded;
    std::string err;
    std::vector<std::string> diag;
    if (!fmt::parseOcCaptions(text, loaded, &diag, &err)) {
        loaded_ = false;
        loadError_ = err.empty() ? "failed to parse .occaptions" : err;
        return;
    }
    for (const std::string& m : diag) AVER_WARN("[CaptionEditor] {}: {}", path_, m);
    originalText_ = std::move(text);
    data_ = std::move(loaded);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    if (selected_ >= data_.cues.size()) selected_ = 0;
}

std::string CaptionEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "###captions:" + path_;
}

bool CaptionEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    const std::string text = fmt::writeOcCaptions(data_, originalText_);
    if (!writeFileTextAtomic(path_, text)) {
        if (why) *why = "could not write " + path_;
        return false;
    }
    originalText_ = text;
    dirty_ = false;
    return true;
}

void CaptionEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[CaptionEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

// ---- undo -----------------------------------------------------------------------------------------

void CaptionEditor::pushUndo() { history_.push(data_); }

void CaptionEditor::undo() {
    if (!history_.undo(data_)) return;
    if (selected_ >= data_.cues.size()) selected_ = data_.cues.empty() ? 0 : data_.cues.size() - 1;
    dirty_ = true;
}

void CaptionEditor::redo() {
    if (!history_.redo(data_)) return;
    if (selected_ >= data_.cues.size()) selected_ = data_.cues.empty() ? 0 : data_.cues.size() - 1;
    dirty_ = true;
}

// ---- speakers -------------------------------------------------------------------------------------

void CaptionEditor::addSpeaker() {
    if (!loaded_) return;
    pushUndo();
    std::string id = "speaker";
    for (int n = 1;; ++n) {
        bool clash = false;
        for (const auto& s : data_.speakers) if (s.id == id) { clash = true; break; }
        if (!clash) break;
        id = "speaker" + std::to_string(n);
    }
    fmt::OcCaptionSpeaker s;
    s.id = id;
    s.name = id;
    data_.speakers.push_back(std::move(s));
    dirty_ = true;
}

void CaptionEditor::removeSpeaker(usize index) {
    if (!loaded_ || index >= data_.speakers.size()) return;
    pushUndo();
    data_.speakers.erase(data_.speakers.begin() + static_cast<isize>(index));
    dirty_ = true;
}

bool CaptionEditor::renameSpeaker(usize index, std::string id) {
    if (!loaded_ || index >= data_.speakers.size()) return false;
    id = cleanId(std::move(id));
    if (id.empty()) return false;
    for (usize i = 0; i < data_.speakers.size(); ++i) if (i != index && data_.speakers[i].id == id) return false;
    const std::string old = data_.speakers[index].id;
    if (old == id) return true;
    pushUndo();
    data_.speakers[index].id = id;
    for (fmt::OcCaptionCue& c : data_.cues) {
        if (c.speaker == old) c.speaker = id;
        for (fmt::OcCaptionLine& l : c.lines) if (l.speaker == old) l.speaker = id;
    }
    dirty_ = true;
    return true;
}

void CaptionEditor::setSpeakerName(usize index, std::string nameRef) {
    if (!loaded_ || index >= data_.speakers.size() || data_.speakers[index].name == nameRef) return;
    pushUndo();
    data_.speakers[index].name = std::move(nameRef);
    dirty_ = true;
}

void CaptionEditor::setSpeakerColour(usize index, u32 rgba) {
    if (!loaded_ || index >= data_.speakers.size() || data_.speakers[index].rgba == rgba) return;
    pushUndo();
    data_.speakers[index].rgba = rgba;
    dirty_ = true;
}

// ---- cues -----------------------------------------------------------------------------------------

std::string CaptionEditor::uniqueCueId(const std::string& baseIn) const {
    std::string base = cleanId(baseIn.empty() ? std::string("cue") : baseIn);
    std::string id = base;
    for (int n = 1;; ++n) {
        bool clash = false;
        for (const auto& c : data_.cues) if (c.id == id) { clash = true; break; }
        if (!clash) return id;
        id = base + "_" + std::to_string(n);
    }
}

usize CaptionEditor::addCue() {
    if (!loaded_) return 0;
    pushUndo();
    fmt::OcCaptionCue c;
    c.id = uniqueCueId("cue");
    fmt::OcCaptionLine l;
    l.text = "@Cap:" + c.id + "=Caption text";
    c.lines.push_back(std::move(l));
    data_.cues.push_back(std::move(c));
    selected_ = data_.cues.size() - 1;
    dirty_ = true;
    return selected_;
}

usize CaptionEditor::addCueFromSound(const std::string& path) {
    if (!loaded_) return 0;
    pushUndo();
    fmt::OcCaptionCue c;
    c.id = uniqueCueId(stemOf(path));
    c.sounds.push_back(path);
    c.kind = looksLikeVoice(path) ? fmt::OcCaptionKind::Dialogue : fmt::OcCaptionKind::Sound;
    fmt::OcCaptionLine l;
    l.text = c.kind == fmt::OcCaptionKind::Dialogue ? "@Cap:" + c.id + "=Spoken text"
                                                    : "@Cap:" + c.id + "=[" + stemOf(path) + "]";
    c.lines.push_back(std::move(l));
    data_.cues.push_back(std::move(c));
    selected_ = data_.cues.size() - 1;
    dirty_ = true;
    return selected_;
}

void CaptionEditor::removeCue(usize index) {
    if (!cueOk(index)) return;
    pushUndo();
    data_.cues.erase(data_.cues.begin() + static_cast<isize>(index));
    if (selected_ >= data_.cues.size()) selected_ = data_.cues.empty() ? 0 : data_.cues.size() - 1;
    dirty_ = true;
}

bool CaptionEditor::renameCue(usize index, std::string id) {
    if (!cueOk(index)) return false;
    id = cleanId(std::move(id));
    if (id.empty()) return false;
    for (usize i = 0; i < data_.cues.size(); ++i) if (i != index && data_.cues[i].id == id) return false;
    if (data_.cues[index].id == id) return true;
    pushUndo();
    data_.cues[index].id = id;
    dirty_ = true;
    return true;
}

usize CaptionEditor::duplicateCue(usize index) {
    if (!cueOk(index)) return 0;
    pushUndo();
    fmt::OcCaptionCue c = data_.cues[index];
    c.id = uniqueCueId(c.id);
    data_.cues.insert(data_.cues.begin() + static_cast<isize>(index) + 1, std::move(c));
    selected_ = index + 1;
    dirty_ = true;
    return selected_;
}

#define AVER_CAP_SET(name, field, type)                                         \
    void CaptionEditor::name(usize index, type v) {                            \
        if (!cueOk(index) || data_.cues[index].field == v) return;             \
        pushUndo();                                                             \
        data_.cues[index].field = std::move(v);                                 \
        dirty_ = true;                                                          \
    }
AVER_CAP_SET(setCueKind, kind, fmt::OcCaptionKind)
AVER_CAP_SET(setCueSpeaker, speaker, std::string)
AVER_CAP_SET(setCuePriority, priority, i32)
AVER_CAP_SET(setCueRadius, radius, f32)
AVER_CAP_SET(setCueCooldown, cooldown, f32)
AVER_CAP_SET(setCueHold, hold, f32)
AVER_CAP_SET(setCueTag, tag, std::string)
#undef AVER_CAP_SET

void CaptionEditor::addCueSound(usize cue, std::string pattern) {
    if (!cueOk(cue)) return;
    pushUndo();
    data_.cues[cue].sounds.push_back(std::move(pattern));
    dirty_ = true;
}

void CaptionEditor::setCueSound(usize cue, usize i, std::string pattern) {
    if (!cueOk(cue) || i >= data_.cues[cue].sounds.size() || data_.cues[cue].sounds[i] == pattern) return;
    pushUndo();
    data_.cues[cue].sounds[i] = std::move(pattern);
    dirty_ = true;
}

void CaptionEditor::removeCueSound(usize cue, usize i) {
    if (!cueOk(cue) || i >= data_.cues[cue].sounds.size()) return;
    pushUndo();
    data_.cues[cue].sounds.erase(data_.cues[cue].sounds.begin() + static_cast<isize>(i));
    dirty_ = true;
}

// ---- lines ----------------------------------------------------------------------------------------

void CaptionEditor::addLine(usize cue) {
    if (!cueOk(cue)) return;
    pushUndo();
    fmt::OcCaptionCue& c = data_.cues[cue];
    fmt::OcCaptionLine l;
    if (!c.lines.empty()) l.start = c.lines.back().start + (c.lines.back().dur > 0 ? c.lines.back().dur : 1.0f);
    l.text = "@Cap:" + c.id + "_" + std::string(1, static_cast<char>('a' + std::min<usize>(c.lines.size(), 25))) + "=Caption text";
    c.lines.push_back(std::move(l));
    dirty_ = true;
}

void CaptionEditor::removeLine(usize cue, usize line) {
    if (!cueOk(cue) || line >= data_.cues[cue].lines.size()) return;
    pushUndo();
    data_.cues[cue].lines.erase(data_.cues[cue].lines.begin() + static_cast<isize>(line));
    dirty_ = true;
}

void CaptionEditor::setLineTimes(usize cue, usize line, f32 start, f32 dur) {
    if (!cueOk(cue) || line >= data_.cues[cue].lines.size()) return;
    fmt::OcCaptionLine& l = data_.cues[cue].lines[line];
    start = std::max(0.0f, start);
    dur = std::max(0.0f, dur);
    if (l.start == start && l.dur == dur) return;
    pushUndo();
    l.start = start;
    l.dur = dur;
    dirty_ = true;
}

void CaptionEditor::setLineText(usize cue, usize line, std::string textRef) {
    if (!cueOk(cue) || line >= data_.cues[cue].lines.size() || data_.cues[cue].lines[line].text == textRef) return;
    pushUndo();
    data_.cues[cue].lines[line].text = std::move(textRef);
    dirty_ = true;
}

void CaptionEditor::setLineSpeaker(usize cue, usize line, std::string speakerId) {
    if (!cueOk(cue) || line >= data_.cues[cue].lines.size() || data_.cues[cue].lines[line].speaker == speakerId) return;
    pushUndo();
    data_.cues[cue].lines[line].speaker = std::move(speakerId);
    dirty_ = true;
}

// ---- the pattern tester -----------------------------------------------------------------------------

usize CaptionEditor::matchCount(const std::string& pattern) const {
    if (!g_hooks.soundPaths || pattern.empty()) return 0;
    usize n = 0;
    for (const std::string& p : g_hooks.soundPaths()) if (fmt::captionPatternMatches(pattern, p)) ++n;
    return n;
}

std::vector<std::string> CaptionEditor::matches(const std::string& pattern, usize limit) const {
    std::vector<std::string> out;
    if (!g_hooks.soundPaths || pattern.empty()) return out;
    for (const std::string& p : g_hooks.soundPaths()) {
        if (!fmt::captionPatternMatches(pattern, p)) continue;
        out.push_back(p);
        if (out.size() >= limit) break;
    }
    return out;
}

// ---- the window -------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

namespace {
ImVec4 colourOf(u32 rgba) {
    return ImVec4(static_cast<float>((rgba >> 24) & 255) / 255.0f, static_cast<float>((rgba >> 16) & 255) / 255.0f,
                  static_cast<float>((rgba >> 8) & 255) / 255.0f, static_cast<float>(rgba & 255) / 255.0f);
}
u32 rgbaOf(const ImVec4& c) {
    const auto b = [](float v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (b(c.x) << 24) | (b(c.y) << 16) | (b(c.z) << 8) | b(c.w);
}
const char* const kKindNames[] = {"Dialogue", "Sound", "Music"};

// A text field that commits on deactivation; returns true on commit.
bool commitText(const char* id, std::string& value, float width) {
    char buf[320];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, buf, sizeof buf);
    if (ImGui::IsItemDeactivatedAfterEdit()) { value = buf; return true; }
    return false;
}
} // namespace

void CaptionEditor::drawSpeakers() {
    if (!ImGui::CollapsingHeader("Speakers", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::BeginTable("capSpeakers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Name (text or reference)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < data_.speakers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string id = data_.speakers[i].id;
            if (commitText("##id", id, -1)) renameSpeaker(i, id);
            ImGui::TableNextColumn();
            std::string nm = data_.speakers[i].name;
            if (commitText("##name", nm, -1)) setSpeakerName(i, nm);
            if (g_hooks.resolveText && !data_.speakers[i].name.empty())
                ImGui::TextDisabled("%s", g_hooks.resolveText(data_.speakers[i].name).c_str());
            ImGui::TableNextColumn();
            ImVec4 col = colourOf(data_.speakers[i].rgba);
            if (ImGui::ColorEdit4("##col", &col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                if (ImGui::IsItemActivated() || !dragUndone_) { pushUndo(); dragUndone_ = true; }
                data_.speakers[i].rgba = rgbaOf(col);
                dirty_ = true;
            }
            if (ImGui::IsItemDeactivated()) dragUndone_ = false;
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_DELETE)) removeSpeaker(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_ADD " Add Speaker")) addSpeaker();
}

void CaptionEditor::drawCueList() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##capFilter", "Filter cues", filter_, sizeof filter_);
    const std::string f = lowerStr(filter_);
    ImGui::BeginChild("capCueList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f), true);
    for (usize i = 0; i < data_.cues.size(); ++i) {
        const fmt::OcCaptionCue& c = data_.cues[i];
        if (!f.empty() && lowerStr(c.id).find(f) == std::string::npos) continue;
        const std::string label = std::string("[") + kKindNames[static_cast<int>(c.kind)][0] + "] " + c.id + "###cue" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), i == selected_)) selected_ = i;
    }
    ImGui::EndChild();
    if (ImGui::Button(ICON_ADD " Add Cue")) addCue();
    ImGui::SameLine();
    if (ImGui::Button("From Sound...")) ImGui::OpenPopup("capFromSound");
    if (ImGui::BeginPopup("capFromSound")) {
        const std::vector<std::string> sounds = g_hooks.soundPaths ? g_hooks.soundPaths() : std::vector<std::string>();
        if (sounds.empty()) ImGui::TextDisabled("No audio assets in this project.");
        ImGui::BeginChild("capSoundPick", ImVec2(420, 260));
        for (const std::string& s : sounds)
            if (ImGui::Selectable(s.c_str())) { addCueFromSound(s); ImGui::CloseCurrentPopup(); }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
}

void CaptionEditor::drawCueDetails() {
    if (!cueOk(selected_)) { ImGui::TextDisabled("No cue selected."); return; }
    const usize ci = selected_;
    fmt::OcCaptionCue& c = data_.cues[ci];

    std::string id = c.id;
    ImGui::TextUnformatted("Id");
    ImGui::SameLine(110);
    if (commitText("##cueId", id, 220)) renameCue(ci, id);
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) { duplicateCue(ci); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_DELETE " Remove")) { removeCue(ci); return; }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_hooks.previewCue);
    if (ImGui::SmallButton(ICON_PLAY " Preview") && g_hooks.previewCue) g_hooks.previewCue(c.id);
    ImGui::EndDisabled();

    int kind = static_cast<int>(c.kind);
    ImGui::TextUnformatted("Kind");
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("##kind", &kind, "Dialogue\0Sound\0Music\0")) setCueKind(ci, static_cast<fmt::OcCaptionKind>(kind));

    ImGui::TextUnformatted("Speaker");
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##speaker", c.speaker.empty() ? "(none)" : c.speaker.c_str())) {
        if (ImGui::Selectable("(none)", c.speaker.empty())) setCueSpeaker(ci, std::string());
        for (const auto& s : data_.speakers) if (ImGui::Selectable(s.id.c_str(), s.id == c.speaker)) setCueSpeaker(ci, s.id);
        ImGui::EndCombo();
    }

    // Numbers drag with one undo step per drag, like the other editors.
    const auto dragF = [&](const char* label, const char* tip, f32& v, float speed, float lo, float hi) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(110);
        ImGui::SetNextItemWidth(120);
        const bool ch = ImGui::DragFloat((std::string("##") + label).c_str(), &v, speed, lo, hi, "%.2f");
        if (ImGui::IsItemActivated()) pushUndo();
        if (ch) dirty_ = true;
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    ImGui::TextUnformatted("Priority");
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(120);
    int prio = c.priority;
    const bool pch = ImGui::DragInt("##prio", &prio, 0.2f, -100, 100);
    if (ImGui::IsItemActivated()) pushUndo();
    if (pch) { c.priority = prio; dirty_ = true; }
    dragF("Radius", "cm; 0 = always shown. Positional sounds farther than this from the listener are skipped.", c.radius, 10.0f, 0.0f, 100000.0f);
    dragF("Cooldown", "seconds before the same cue can show again", c.cooldown, 0.05f, 0.0f, 60.0f);
    {
        bool useDefault = c.hold < 0;
        ImGui::TextUnformatted("Hold");
        ImGui::SameLine(110);
        if (ImGui::Checkbox("default##hold", &useDefault)) setCueHold(ci, useDefault ? -1.0f : fmt::captionCueHold(c));
        if (c.hold >= 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            const bool hch = ImGui::DragFloat("##holdv", &c.hold, 0.05f, 0.0f, 30.0f, "%.2f s");
            if (ImGui::IsItemActivated()) pushUndo();
            if (hch) dirty_ = true;
        }
    }

    std::string tag = c.tag;
    ImGui::TextUnformatted("Tag");
    ImGui::SameLine(110);
    if (commitText("##tag", tag, 320)) setCueTag(ci, tag);
    if (g_hooks.resolveText && !c.tag.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", g_hooks.resolveText(c.tag).c_str()); }

    ImGui::SeparatorText("Sounds that raise this cue");
    for (usize i = 0; i < c.sounds.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::string p = c.sounds[i];
        if (commitText("##pat", p, 360)) setCueSound(ci, i, p);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu match(es)", matchCount(c.sounds[i]));
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_DELETE)) { removeCueSound(ci, i); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(ICON_ADD " Add pattern")) addCueSound(ci, "Audio/*.ocaudio");
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##tester", "Test a pattern against this project's sounds", testPattern_, sizeof testPattern_);
    if (testPattern_[0]) {
        ImGui::SameLine();
        ImGui::Text("matches %zu sound(s)", matchCount(testPattern_));
        if (ImGui::IsItemHovered()) {
            const std::vector<std::string> m = matches(testPattern_);
            ImGui::BeginTooltip();
            for (const std::string& s : m) ImGui::TextUnformatted(s.c_str());
            if (m.empty()) ImGui::TextDisabled("none");
            ImGui::EndTooltip();
        }
    }

    ImGui::SeparatorText("Lines");
    if (ImGui::BeginTable("capLines", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Dur", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Text (text or reference)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Speaker", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < c.lines.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            fmt::OcCaptionLine& l = c.lines[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            bool ch = ImGui::DragFloat("##start", &l.start, 0.02f, 0.0f, 600.0f, "%.2f");
            if (ImGui::IsItemActivated()) pushUndo();
            if (ch) { l.start = std::max(0.0f, l.start); dirty_ = true; }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ch = ImGui::DragFloat("##dur", &l.dur, 0.02f, 0.0f, 600.0f, l.dur > 0 ? "%.2f" : "voice");
            if (ImGui::IsItemActivated()) pushUndo();
            if (ch) { l.dur = std::max(0.0f, l.dur); dirty_ = true; }
            ImGui::TableNextColumn();
            std::string t = l.text;
            if (commitText("##text", t, -1)) setLineText(ci, i, t);
            if (g_hooks.resolveText && !l.text.empty()) ImGui::TextDisabled("%s", g_hooks.resolveText(l.text).c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##lspk", l.speaker.empty() ? "(cue)" : l.speaker.c_str())) {
                if (ImGui::Selectable("(cue)", l.speaker.empty())) setLineSpeaker(ci, i, std::string());
                for (const auto& s : data_.speakers) if (ImGui::Selectable(s.id.c_str(), s.id == l.speaker)) setLineSpeaker(ci, i, s.id);
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            const bool del = ImGui::SmallButton(ICON_DELETE);
            ImGui::PopID();
            if (del) { removeLine(ci, i); break; }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_ADD " Add Line")) addLine(ci);
    if (c.lines.empty() && c.tag.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "A cue with no line and no tag is skipped when the file loads.");
}

void CaptionEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[CaptionEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
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
    ImGui::Separator();

    drawSpeakers();
    ImGui::Dummy(ImVec2(0, 6.0f));
    const float listW = std::min(260.0f, ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::BeginChild("capLeft", ImVec2(listW, 0), false);
    drawCueList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("capRight", ImVec2(0, 0), false);
    drawCueDetails();
    ImGui::EndChild();
}

#else   // AVER_WITH_IMGUI

// The headless build (tests/editor leaves AVER_WITH_IMGUI undefined) still gets load/save/dirty/undo and every setter.
void CaptionEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeCaptionEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".occaptions") return nullptr;
    return std::make_unique<CaptionEditor>(path);
}

} // namespace aver::editor
