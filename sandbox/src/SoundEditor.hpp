#pragma once
// The Aver Sound editor tab: a .ocsnd opened as an asset, shown as a node list beside a parameter
// panel, with a rendered waveform and an audible preview underneath.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching BtEditor.hpp and GraphEditor.hpp before it:
//   #include "SoundEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeSoundEditor);   // APPENDED -- order is precedence
// and nothing else.
//
// A LIST, NOT A CANVAS, and that is a deliberate difference from GraphEditor's .ocgraph tab even
// though both edit a DAG. A .ocsnd graph is a handful of nodes deep and one or two wide -- an
// oscillator, an envelope, a filter -- so a canvas would spend its whole budget on pan/zoom/layout
// state that the format does not even store (there are no node positions in OcSoundData). The list
// shows the same information with none of that, and it costs no persisted layout the format would
// then have to grow a chunk for.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcSound.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// ---- structural edits, as FREE FUNCTIONS -------------------------------------------------------
//
// NOT SoundEditor members, for BtEditor.hpp's reason exactly: draw() is behind `#if AVER_WITH_IMGUI`
// and cannot run headless, but these can, and tests/editor's own target compiles this .cpp with that
// macro undefined to reach them.
//
// EVERY ONE PRESERVES "SOURCES BEFORE CONSUMERS" (OcSoundData::valid(), modules/formats/src/
// OcSound.cpp) BY CONSTRUCTION rather than by each edit separately remembering to. Where an edit
// could break that ordering -- which is only ever snAddLink -- the whole node array is re-sorted
// topologically instead of the edit being refused, so the author is never told "you cannot connect
// these two" for a reason that is really about array order. INDICES THEREFORE MOVE across such a
// call; the functions that can move them say what they return so a caller can keep its selection.

// The display name of a node kind, for a list row or a combo box.
const char* snKindName(fmt::OcSoundNodeKind kind);

// What params[slot] MEANS for this kind, or nullptr when the kind does not read that slot. The
// panel hides what it cannot name: an unused slot shown as "params[3]" invites someone to set it
// and then wonder why nothing happened.
const char* snParamLabel(fmt::OcSoundNodeKind kind, u32 slot);

// What input `input` means for this kind, or nullptr when the kind has no such input.
const char* snInputLabel(fmt::OcSoundNodeKind kind, u32 input);

// The params a freshly created node of this kind starts with. NOT ZEROES: a Sine at 0 Hz and a Gain
// of 0 are both silent, and a node that does nothing when you add it reads as broken rather than as
// unconfigured.
void snDefaultParams(fmt::OcSoundNodeKind kind, f32 params[4]);

// Appends a node of `kind` with that kind's defaults. Appended LAST, which is the only position
// that is always legal: the highest index can consume anything and feeds nothing yet. Returns its
// index, which is always nodes.size() - 1.
i32 snAddNode(fmt::OcSoundData& d, fmt::OcSoundNodeKind kind);

// Removes `index` and every link touching it. Returns the index to select afterwards; -1 when
// `index` is out of range or is the last node left (valid() requires a non-empty graph, so a graph
// cannot be emptied from the UI). When the deleted node WAS the output, the output moves to the
// highest remaining index rather than to 0 -- an output is conventionally the terminal node, and
// silently making the first oscillator the output would be a louder surprise than picking the end.
i32 snDeleteNode(fmt::OcSoundData& d, i32 index);

// Changes `index`'s kind. Two things follow and both are deliberate:
//
//   PARAMS ARE RESET to the new kind's defaults. They are NOT carried over, because a param slot
//   means something different for every kind -- a LowPass cutoff of 2000 becoming an Adsr attack of
//   2000 seconds is a graph that renders pure silence, from a number the author never typed. Undo
//   covers the rarer case where somebody wanted to keep a value.
//
//   INCOMING LINKS TO INPUTS THE NEW KIND DOES NOT HAVE ARE DROPPED, because valid() rejects a link
//   to an input that does not exist and the graph would silently refuse to save otherwise.
void snSetKind(fmt::OcSoundData& d, i32 index, fmt::OcSoundNodeKind kind);

// Every node feeding `toInput` of `to`, in link order. A LIST, NOT ONE SOURCE: the evaluator sums
// what arrives at an input (in0[l.toNode] += v, modules/sound/src/Synth.cpp), so two oscillators
// into one input is a legal graph that means something, and an editor that showed only the first
// would quietly discard the second on the next edit.
std::vector<i32> snSourcesOf(const fmt::OcSoundData& d, i32 to, u32 toInput);

// Connects `from`'s output to `toInput` of `to`, RE-SORTING the graph when that is what it takes to
// keep sources before consumers. Returns the new index of `to` -- the node whose panel the author
// is in when they add an input -- so a caller can keep its selection across the re-sort.
//
// -1 when either index is out of range, when `to` has no such input, when the link already exists
// (it would double the signal, which is a mistake far more often than an intent), or when it would
// make a CYCLE. A cycle is the one case that is genuinely refused rather than re-sorted: no ordering
// of the array can satisfy a node that feeds itself, directly or round a loop.
i32 snAddLink(fmt::OcSoundData& d, i32 from, i32 to, u32 toInput);

// Removes that exact link. False when it was not there.
bool snRemoveLink(fmt::OcSoundData& d, i32 from, i32 to, u32 toInput);

// Makes `index` the graph's output. False when out of range.
bool snSetOutput(fmt::OcSoundData& d, i32 index);

// A graph worth hearing the moment it is created: a 220 Hz saw through a low pass through an ADSR.
// Used by the editor's New Sound Graph, and by tests as a fixture. Deliberately NOT a bare sine --
// a starter asset should show what the format is FOR (a source, a shaper and an envelope in a
// chain), not the smallest thing that validates.
fmt::OcSoundData snStarterGraph();

// ---- the tab ------------------------------------------------------------------------------------

// Declared in the header rather than hidden behind the factory for BtEditor's reason: tests/editor
// constructs the tab directly to exercise load / save / dirty / undo with no ImGui and no window.
class SoundEditor final : public AssetEditor {
public:
    explicit SoundEditor(std::string path);
    ~SoundEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test, for the same reason the edits above are free functions.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcSoundData& graph() const { return graph_; }
    i32 selected() const { return selected_; }
    void select(i32 index) { selected_ = index; }

    // Snapshot undo, through the shared SnapshotUndo<State> template (SnapshotUndo.hpp) now --
    // GraphEditor/BtEditor migrated to the same template in the same change; see that header for why
    // a whole-graph copy per edit is cheaper here than the bookkeeping an undoable-command layer
    // would need.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // The structural edits as the TAB performs them: push undo, apply, remap the selection, mark
    // dirty. A test drives these to check the tab's bookkeeping, not just the free functions'.
    void addNode(fmt::OcSoundNodeKind kind);
    void deleteSelected();
    void setSelectedKind(fmt::OcSoundNodeKind kind);
    void linkInto(i32 from, i32 to, u32 toInput);
    void unlink(i32 from, i32 to, u32 toInput);
    void makeSelectedOutput();
    // How long a render of this graph runs for. An edit like any other -- it is a property of
    // the SOUND, carried in the asset, so it belongs on the undo stack beside the node edits.
    void setDuration(f32 seconds);
    void markDirty() { dirty_ = true; }

    // RENDERS THE GRAPH TO PCM, into previewPcm(). Headless and therefore TESTED -- this is the
    // half of "preview" that can actually be wrong (an invalid graph, a duration that overruns the
    // renderer's frame cap, a seed that fails to vary). Handing the buffer to the mixer is three
    // ABI calls and sits in the drawing half, beside the button that triggers it.
    bool renderPreview(std::string* why = nullptr);
    const std::vector<f32>& previewPcm() const { return previewPcm_; }
    u32 previewSeed() const { return previewSeed_; }
    // Each render advances the seed, so pressing Preview twice on a graph containing Noise gives
    // two different renders -- which is the whole argument for a procedural sound over a .wav, and
    // it should be audible from the editor rather than only from a running game.
    void bumpPreviewSeed() { previewSeed_ = previewSeed_ * 1664525u + 1013904223u; }

private:
    void loadFromDisk();
    void stopPreview();

    std::string path_;
    fmt::OcSoundData graph_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 selected_ = 0;

    SnapshotUndo<fmt::OcSoundData> history_;

    std::vector<f32> previewPcm_;
    std::string previewError_;
    u32 previewSeed_ = 1;
    i32 previewSound_ = 0;   // aver_audio_load_pcm handle, 0 when none
    i32 previewVoice_ = 0;   // aver_audio_play handle, 0 when none

#if AVER_WITH_IMGUI
    void drawNodeList();
    void drawDetails();
    void drawTransport();
    void playPreview();
#endif
};

// Creates a sound editor for a .ocsnd, else nullptr.
std::unique_ptr<AssetEditor> makeSoundEditor(const std::string& path);

} // namespace aver::editor
