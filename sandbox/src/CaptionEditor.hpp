#pragma once
// The Captions editor tab: a .occaptions opened as an asset -- its speakers, cues, timed lines, the sound patterns
// that raise a cue, a pattern tester and a Preview button. See aver/formats/OcCaptions.hpp for the format and
// docs/ACCESSIBILITY.md section 2.9 for the tab.
//
// THE HOOK INTO SandboxApp.cpp IS TWO LINES, InputSchemeEditor's pattern:
//   #include "CaptionEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeCaptionEditor);   // APPENDED -- order is precedence
// Headless-compilable (AVER_WITH_IMGUI undefined) so tests/editor drives load, save, undo and every setter.
//
// MODELLED ON InputSchemeEditor: SnapshotUndo<State> with State the whole record, and a TEXT-MERGED save
// (fmt::writeOcCaptions(data_, originalText_)) so a comment an author left in the file survives.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcCaptions.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// What the tab needs from SandboxApp. Installed once (setCaptionEditorHooks), read by every open tab.
struct CaptionEditorHooks {
    // The project's audio files, content-relative with '/' separators: feeds the pattern tester and "Add cue from
    // sound". Empty list or no hook: the tester reports 0.
    std::function<std::vector<std::string>()> soundPaths;
    // Shows the cue in the Game UI preview and fires it through the engine for an open Play session.
    std::function<bool(const std::string& cueId)> previewCue;
    // A localization reference resolved to its current text, for the preview line under each text field.
    std::function<std::string(const std::string& raw)> resolveText;
};
void setCaptionEditorHooks(CaptionEditorHooks hooks);

class CaptionEditor final : public AssetEditor {
public:
    explicit CaptionEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcCaptionData& data() const { return data_; }
    void markDirty() { dirty_ = true; }

    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- speakers ----
    void addSpeaker();                                  // "speaker", "speaker1", ... the first free id
    void removeSpeaker(usize index);                    // cues naming it keep the id (a missing speaker shows its id)
    bool renameSpeaker(usize index, std::string id);    // repoints every cue and line; false for an empty or taken id
    void setSpeakerName(usize index, std::string nameRef);
    void setSpeakerColour(usize index, u32 rgba);       // 0xRRGGBBAA

    // ---- cues ----
    // Appends an empty-looking cue with one line, selects it. Returns its index.
    usize addCue();
    // A cue whose sound pattern is `contentRelativePath` and whose id is the file stem (made unique); a path under a
    // VO / Voice / Dialogue folder starts as dialogue. Returns its index.
    usize addCueFromSound(const std::string& contentRelativePath);
    void removeCue(usize index);
    bool renameCue(usize index, std::string id);        // false for an empty or taken id
    usize duplicateCue(usize index);
    void setCueKind(usize index, fmt::OcCaptionKind kind);
    void setCueSpeaker(usize index, std::string speakerId);
    void setCuePriority(usize index, i32 priority);
    void setCueRadius(usize index, f32 cm);
    void setCueCooldown(usize index, f32 seconds);
    void setCueHold(usize index, f32 seconds);          // < 0 = the kind's default
    void setCueTag(usize index, std::string textRef);
    void addCueSound(usize cue, std::string pattern);
    void setCueSound(usize cue, usize i, std::string pattern);
    void removeCueSound(usize cue, usize i);

    // ---- lines ----
    void addLine(usize cue);                            // after the last line, starting where it ends
    void removeLine(usize cue, usize line);
    void setLineTimes(usize cue, usize line, f32 start, f32 dur);
    void setLineText(usize cue, usize line, std::string textRef);
    void setLineSpeaker(usize cue, usize line, std::string speakerId);

    // ---- the pattern tester ----
    // How many of the project's sounds (CaptionEditorHooks::soundPaths) the pattern matches.
    usize matchCount(const std::string& pattern) const;
    std::vector<std::string> matches(const std::string& pattern, usize limit = 16) const;

    usize selectedCue() const { return selected_; }
    void selectCue(usize index) { if (index < data_.cues.size()) selected_ = index; }

private:
    void loadFromDisk();
    bool cueOk(usize i) const { return loaded_ && i < data_.cues.size(); }
    std::string uniqueCueId(const std::string& base) const;

    std::string path_;
    fmt::OcCaptionData data_;
    std::string originalText_;   // writeOcCaptions' `existing` merge argument
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    usize selected_ = 0;
    SnapshotUndo<fmt::OcCaptionData> history_;

#if AVER_WITH_IMGUI
    void drawSpeakers();
    void drawCueList();
    void drawCueDetails();
    char filter_[64] = {};
    char testPattern_[160] = {};
    bool dragUndone_ = false;
#endif
};

// Creates a Captions editor for a .occaptions, else nullptr.
std::unique_ptr<AssetEditor> makeCaptionEditor(const std::string& path);

// The starter file the Content Browser's "New Captions" writes: one speaker and one dialogue cue, immediately
// meaningful rather than an empty file.
inline fmt::OcCaptionData captionStarterData() {
    fmt::OcCaptionData d;
    fmt::OcCaptionSpeaker s;
    s.id = "narrator";
    s.name = "@Cap:spk.narrator=Narrator";
    s.rgba = 0x7DD3FCFFu;
    d.speakers.push_back(s);
    fmt::OcCaptionCue c;
    c.id = "intro_01";
    c.kind = fmt::OcCaptionKind::Dialogue;
    c.speaker = "narrator";
    c.sounds.push_back("Audio/VO/intro_01.ocaudio");
    fmt::OcCaptionLine l;
    l.start = 0.0f;
    l.dur = 0.0f;
    l.text = "@Cap:intro_01_a=Welcome.";
    c.lines.push_back(l);
    d.cues.push_back(c);
    return d;
}

} // namespace aver::editor
