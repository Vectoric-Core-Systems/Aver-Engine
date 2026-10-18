// The .occaptions editor tab's headless core: load / save with the file's comments kept, dirty and undo/redo, the
// speaker / cue / line setters (and their refusals), adding a cue from a sound path, the pattern tester, speaker
// colours, and renaming cues and speakers.
//
// AVER_WITH_IMGUI IS DELIBERATELY UNDEFINED for this target (InputSchemeEditorTest's arrangement): the drawing half of
// CaptionEditor.cpp sits behind that macro, so what runs is exactly the part that needs no window.
#include "CaptionEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcCaptions.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

static std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const char* kFixture =
    "OCCAPTIONS 1\n"
    "# hand-written note that must survive a save\n"
    "SPEAKER guard name \"@Cap:spk.guard=Guard\" colour #7DD3FC\n"
    "CUE intro_01 sound \"Audio/VO/intro_01.ocaudio\" kind dialogue speaker guard priority 8\n"
    "  LINE 0.0 2.4 \"@Cap:intro_01_a=Welcome.\"\n"
    "CUE boom sound \"Audio/SFX/explosion_*.ocaudio\" kind sound tag \"[explosion]\"\n";

int main() {
    AVER_INFO("CaptionEditorTest");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-captions-editor").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/Test.occaptions";
    writeText(path, kFixture);

    using editor::CaptionEditor;
    editor::CaptionEditorHooks hooks;
    hooks.soundPaths = [] {
        return std::vector<std::string>{"Audio/SFX/explosion_01.ocaudio", "Audio/SFX/explosion_02.ocaudio", "Audio/SFX/door.ocaudio",
                                        "Audio/VO/intro_01.ocaudio", "Audio/VO/outro_02.ocaudio", "Audio/Music/theme.ocaudio"};
    };
    editor::setCaptionEditorHooks(hooks);

    // ---- load ----
    {
        CaptionEditor ed(path);
        check(ed.loaded() && ed.data().cues.size() == 2 && ed.data().speakers.size() == 1, "the fixture loads: two cues, one speaker");
        check(!ed.dirty() && !ed.canUndo(), "freshly loaded: clean, nothing to undo");
        check(ed.title().find("Test.occaptions") != std::string::npos, "the title names the file");
        CaptionEditor missing(dir + "/nope.occaptions");
        check(!missing.loaded() && !missing.loadError().empty(), "a missing file reports why");
        check(editor::makeCaptionEditor(path) != nullptr && editor::makeCaptionEditor(dir + "/x.ocinput") == nullptr, "the factory takes .occaptions only");
    }

    // ---- edits, undo, save with comments ----
    {
        CaptionEditor ed(path);
        ed.setCuePriority(0, 9);
        check(ed.dirty() && ed.data().cues[0].priority == 9 && ed.canUndo(), "an edit dirties the tab and pushes undo");
        ed.setCuePriority(0, 9);
        check(ed.data().cues[0].priority == 9, "setting the same value again is harmless");
        ed.undo();
        check(ed.data().cues[0].priority == 8 && ed.canRedo(), "undo restores it");
        ed.redo();
        check(ed.data().cues[0].priority == 9, "redo reapplies it");

        ed.setLineText(0, 0, "@Cap:intro_01_a=Welcome, friend.");
        ed.setLineTimes(0, 0, 0.5f, 3.0f);
        ed.setCueKind(1, fmt::OcCaptionKind::Music);
        ed.setCueRadius(1, 1500.0f);
        ed.setCueCooldown(1, 2.5f);
        ed.setCueHold(1, 2.0f);
        ed.setCueSpeaker(1, "guard");
        ed.setLineSpeaker(0, 0, "guard");
        check(ed.data().cues[0].lines[0].start == 0.5f && ed.data().cues[1].kind == fmt::OcCaptionKind::Music && ed.data().cues[1].radius == 1500.0f &&
                  ed.data().cues[1].cooldown == 2.5f && ed.data().cues[1].hold == 2.0f && ed.data().cues[1].speaker == "guard",
              "the cue and line setters take their values");
        ed.setLineTimes(0, 0, -3.0f, -1.0f);
        check(ed.data().cues[0].lines[0].start == 0.0f && ed.data().cues[0].lines[0].dur == 0.0f, "negative times clamp to 0");
        ed.setCueTag(1, "[bang]");

        std::string why;
        check(ed.save(&why) && !ed.dirty(), "save writes and clears the dirty flag");
        const std::string text = readText(path);
        check(text.find("# hand-written note that must survive a save") != std::string::npos, "the file's own comment survives the save");
        fmt::OcCaptionData back;
        check(fmt::parseOcCaptions(text, back) && back.cues.size() == 2 && back.cues[1].tag == "[bang]" && back.cues[0].priority == 9, "and the edits are on disk");
        // Undo is gone after a reload but a later edit still merges against the saved text.
        ed.addLine(0);
        check(ed.save(&why) && readText(path).find("# hand-written note") != std::string::npos, "a second save merges against what is now on disk");
    }

    // ---- structure: cues, sounds, lines ----
    {
        writeText(path, kFixture);
        CaptionEditor ed(path);
        const usize n = ed.addCue();
        check(n == 2 && ed.data().cues.size() == 3 && ed.data().cues[2].lines.size() == 1 && ed.selectedCue() == 2, "Add Cue appends one with a line, and selects it");
        check(ed.data().cues[2].id == "cue" && ed.addCue() == 3 && ed.data().cues[3].id == "cue_1", "ids are made unique");
        check(ed.renameCue(2, "alarm") && ed.data().cues[2].id == "alarm", "a cue renames");
        check(!ed.renameCue(2, "boom") && !ed.renameCue(2, "") && !ed.renameCue(2, "   ") , "a taken, empty or blank id is refused");
        check(ed.renameCue(2, "two words") && ed.data().cues[2].id == "two_words", "spaces in an id become underscores (the file is whitespace separated)");

        const usize dup = ed.duplicateCue(0);
        check(dup == 1 && ed.data().cues.size() == 5 && ed.data().cues[1].id != ed.data().cues[0].id && ed.data().cues[1].lines.size() == 1, "duplicate copies a cue just after the original under a new id");
        ed.removeCue(1);
        check(ed.data().cues.size() == 4, "remove drops a cue");

        ed.addCueSound(0, "Audio/VO/extra.ocaudio");
        ed.setCueSound(0, 1, "Audio/VO/extra2.ocaudio");
        check(ed.data().cues[0].sounds.size() == 2 && ed.data().cues[0].sounds[1] == "Audio/VO/extra2.ocaudio", "sound patterns add and edit");
        ed.removeCueSound(0, 1);
        check(ed.data().cues[0].sounds.size() == 1, "and remove");

        ed.addLine(0);
        check(ed.data().cues[0].lines.size() == 2 && ed.data().cues[0].lines[1].start > 0.0f, "Add Line starts where the last one ends");
        ed.removeLine(0, 1);
        check(ed.data().cues[0].lines.size() == 1, "remove line");

        // Out-of-range indices are harmless.
        ed.removeCue(99);
        ed.setCuePriority(99, 1);
        ed.setLineText(0, 99, "x");
        ed.removeLine(99, 0);
        check(ed.data().cues.size() == 4, "out-of-range indices do nothing");
    }

    // ---- add cue from a sound path ----
    {
        writeText(path, kFixture);
        CaptionEditor ed(path);
        const usize v = ed.addCueFromSound("Audio/VO/outro_02.ocaudio");
        const fmt::OcCaptionCue& c = ed.data().cues[v];
        check(c.id == "outro_02" && c.kind == fmt::OcCaptionKind::Dialogue && c.sounds.size() == 1 && c.sounds[0] == "Audio/VO/outro_02.ocaudio" && c.lines.size() == 1,
              "a voice file becomes a dialogue cue named after the file, with its path as the pattern and one line");
        const usize s = ed.addCueFromSound("Audio/SFX/door.ocaudio");
        check(ed.data().cues[s].kind == fmt::OcCaptionKind::Sound && ed.data().cues[s].lines[0].text.find("[door]") != std::string::npos, "an effect becomes a sound cue captioned [name]");
        const usize again = ed.addCueFromSound("Audio/SFX/door.ocaudio");
        check(ed.data().cues[again].id == "door_1", "the same file twice gets a unique id");
        std::string why;
        ed.save(&why);
        fmt::OcCaptionData back;
        check(fmt::parseOcCaptions(readText(path), back, nullptr) && back.cues.size() == 5, "all of them survive a save and reload");
    }

    // ---- the pattern tester ----
    {
        writeText(path, kFixture);
        CaptionEditor ed(path);
        check(ed.matchCount("Audio/SFX/explosion_*.ocaudio") == 2, "explosion_* matches two project sounds");
        check(ed.matchCount("explosion_01.ocaudio") == 1 && ed.matchCount("Audio/*/*.ocaudio") == 6, "a bare file name and a two-segment wildcard");
        check(ed.matchCount("nothing_here.ocaudio") == 0 && ed.matchCount("") == 0, "no match, and an empty pattern matches nothing");
        const std::vector<std::string> m = ed.matches("Audio/VO/*.ocaudio");
        check(m.size() == 2 && m[0] == "Audio/VO/intro_01.ocaudio", "matches() lists them");
        check(ed.matches("Audio/*/*.ocaudio", 3).size() == 3, "and respects the limit");
    }

    // ---- speakers ----
    {
        writeText(path, kFixture);
        CaptionEditor ed(path);
        ed.setSpeakerColour(0, 0x112233FFu);
        check(ed.data().speakers[0].rgba == 0x112233FFu && ed.dirty(), "a speaker colour edits");
        ed.undo();
        check(ed.data().speakers[0].rgba == 0x7DD3FCFFu, "and undoes");
        ed.addSpeaker();
        check(ed.data().speakers.size() == 2 && ed.data().speakers[1].id == "speaker", "Add Speaker makes a unique id");
        ed.addSpeaker();
        check(ed.data().speakers[2].id == "speaker1", "and a next one");
        check(ed.renameSpeaker(0, "sentry") && ed.data().speakers[0].id == "sentry" && ed.data().cues[0].speaker == "sentry", "renaming a speaker repoints the cues that use it");
        ed.setLineSpeaker(0, 0, "sentry");
        ed.renameSpeaker(0, "watch");
        check(ed.data().cues[0].lines[0].speaker == "watch", "and their lines");
        check(!ed.renameSpeaker(1, "watch") && !ed.renameSpeaker(1, ""), "a taken or empty speaker id is refused");
        ed.setSpeakerName(0, "@Cap:spk.watch=Watchman");
        check(ed.data().speakers[0].name == "@Cap:spk.watch=Watchman", "the name reference edits");
        ed.removeSpeaker(1);
        check(ed.data().speakers.size() == 2, "remove speaker");
    }

    // ---- reload from disk ----
    {
        writeText(path, kFixture);
        CaptionEditor ed(path);
        writeText(path, "OCCAPTIONS 1\nCUE only tag \"x\"\n");
        ed.onFileChanged();
        check(ed.data().cues.size() == 1 && ed.data().cues[0].id == "only", "a clean tab reloads when the file changes");
        ed.setCuePriority(0, 4);
        writeText(path, kFixture);
        ed.onFileChanged();
        check(ed.data().cues.size() == 1 && ed.data().cues[0].priority == 4, "a dirty tab keeps its edits");
    }

    // ---- the starter file ----
    {
        const fmt::OcCaptionData starter = editor::captionStarterData();
        const std::string text = fmt::writeOcCaptions(starter);
        fmt::OcCaptionData back;
        std::vector<std::string> diag;
        check(fmt::parseOcCaptions(text, back, &diag) && diag.empty() && back.cues.size() == 1 && back.speakers.size() == 1 && back.cues[0].lines.size() == 1, "the starter file parses cleanly");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO(g_failures == 0 ? "CaptionEditorTest: PASS" : "CaptionEditorTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
