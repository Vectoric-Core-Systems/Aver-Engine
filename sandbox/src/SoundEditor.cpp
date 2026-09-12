// The Aver Sound (.ocsnd) editor tab. See the header for the three-line SandboxApp hook, for why
// the structural edits are free functions, and for why this is a list rather than a canvas.

#include "SoundEditor.hpp"
#include "EditorKeybinds.hpp"

#include "EditorIcons.hpp"
#include "aver/core/Log.hpp"
#include "aver/sound/Synth.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

// AVER_WITH_AUDIO_ABI is defined by sandbox/CMakeLists.txt when Aver.Audio.Abi is linked into
// this binary. GUARDED SEPARATELY FROM AVER_WITH_IMGUI ON PURPOSE, even though today the two are on
// together: they mean different things, and conflating them would make "this build has no mixer"
// read as "this build has no window". tests/editor's target defines neither, and a build configured
// without the audio seam still compiles the whole tab -- it just cannot make a sound.
#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif

namespace aver::editor {
namespace {

bool inRange(const fmt::OcSoundData& d, i32 index) {
    return index >= 0 && static_cast<usize>(index) < d.nodes.size();
}

// Links in a canonical order: by consumer, then by which input, then by source. Nothing in the
// FORMAT requires this -- OcSoundData::valid() only cares that every link runs low to high -- but
// two things want it. A saved file should not differ byte for byte because of the order somebody
// happened to click in, and the evaluator SUMS what arrives at an input, so with more than one
// source the float addition order is part of the result; fixing the order fixes the render.
void sortLinks(fmt::OcSoundData& d) {
    std::sort(d.links.begin(), d.links.end(),
              [](const fmt::OcSoundLink& a, const fmt::OcSoundLink& b) {
                  if (a.toNode != b.toNode) return a.toNode < b.toNode;
                  if (a.toInput != b.toInput) return a.toInput < b.toInput;
                  return a.fromNode < b.fromNode;
              });
}

// A topological order of the graph as old index -> new index, or an EMPTY vector when the graph
// contains a cycle.
//
// SMALLEST READY INDEX FIRST, which is what makes this stable: on a graph that already satisfies
// "sources before consumers" it returns the identity permutation and nothing moves. So calling it
// after every link is not a graph that reshuffles itself under the author's hands -- it only ever
// moves the nodes that HAD to move, and only when a link needed them to.
//
// The selection scan is O(n^2). Deliberate: a sound graph is tens of nodes, and a priority queue
// here would be more code than the loop it replaced for no measurable difference.
std::vector<i32> topoRemap(const fmt::OcSoundData& d) {
    const i32 n = static_cast<i32>(d.nodes.size());
    std::vector<i32> indeg(static_cast<usize>(n), 0);
    for (const fmt::OcSoundLink& l : d.links)
        if (static_cast<i32>(l.toNode) < n) ++indeg[l.toNode];

    std::vector<bool> done(static_cast<usize>(n), false);
    std::vector<i32> order;
    order.reserve(static_cast<usize>(n));

    for (i32 step = 0; step < n; ++step) {
        i32 pick = -1;
        for (i32 i = 0; i < n; ++i)
            if (!done[static_cast<usize>(i)] && indeg[static_cast<usize>(i)] == 0) { pick = i; break; }
        // Nothing is ready and nodes remain: every survivor is waiting on another survivor, which
        // is a cycle. A SELF-LINK LANDS HERE TOO -- it gives its own node an in-degree that nothing
        // can ever decrement -- so it needs no separate check.
        if (pick < 0) return {};
        done[static_cast<usize>(pick)] = true;
        order.push_back(pick);
        for (const fmt::OcSoundLink& l : d.links)
            if (static_cast<i32>(l.fromNode) == pick && static_cast<i32>(l.toNode) < n)
                --indeg[l.toNode];
    }

    std::vector<i32> newOf(static_cast<usize>(n), 0);
    for (i32 i = 0; i < n; ++i) newOf[static_cast<usize>(order[static_cast<usize>(i)])] = i;
    return newOf;
}

void applyRemap(fmt::OcSoundData& d, const std::vector<i32>& newOf) {
    std::vector<fmt::OcSoundNode> out(d.nodes.size());
    for (usize i = 0; i < d.nodes.size(); ++i)
        out[static_cast<usize>(newOf[i])] = d.nodes[i];
    d.nodes = std::move(out);
    for (fmt::OcSoundLink& l : d.links) {
        l.fromNode = static_cast<u32>(newOf[l.fromNode]);
        l.toNode   = static_cast<u32>(newOf[l.toNode]);
    }
    if (d.outputNode < d.nodes.size()) d.outputNode = static_cast<u32>(newOf[d.outputNode]);
    sortLinks(d);
}

} // namespace

const char* snKindName(fmt::OcSoundNodeKind kind) {
    switch (kind) {
        case fmt::OcSoundNodeKind::Sine:     return "Sine";
        case fmt::OcSoundNodeKind::Saw:      return "Saw";
        case fmt::OcSoundNodeKind::Square:   return "Square";
        case fmt::OcSoundNodeKind::Noise:    return "Noise";
        case fmt::OcSoundNodeKind::Const:    return "Const";
        case fmt::OcSoundNodeKind::Gain:     return "Gain";
        case fmt::OcSoundNodeKind::LowPass:  return "Low pass";
        case fmt::OcSoundNodeKind::Adsr:     return "ADSR";
        case fmt::OcSoundNodeKind::Mix:      return "Mix";
        case fmt::OcSoundNodeKind::Multiply: return "Multiply";
    }
    return "?";
}

const char* snParamLabel(fmt::OcSoundNodeKind kind, u32 slot) {
    switch (kind) {
        case fmt::OcSoundNodeKind::Sine:
        case fmt::OcSoundNodeKind::Saw:
            return slot == 0 ? "Frequency (Hz)" : nullptr;
        case fmt::OcSoundNodeKind::Square:
            if (slot == 0) return "Frequency (Hz)";
            if (slot == 1) return "Duty (0..1)";
            return nullptr;
        case fmt::OcSoundNodeKind::Noise:
            return nullptr;   // driven by the render seed, not by a param
        case fmt::OcSoundNodeKind::Const:
            return slot == 0 ? "Value" : nullptr;
        case fmt::OcSoundNodeKind::Gain:
            return slot == 0 ? "Gain" : nullptr;
        case fmt::OcSoundNodeKind::LowPass:
            return slot == 0 ? "Cutoff (Hz)" : nullptr;
        case fmt::OcSoundNodeKind::Adsr:
            if (slot == 0) return "Attack (s)";
            if (slot == 1) return "Decay (s)";
            if (slot == 2) return "Sustain (0..1)";
            if (slot == 3) return "Release (s)";
            return nullptr;
        case fmt::OcSoundNodeKind::Mix:
        case fmt::OcSoundNodeKind::Multiply:
            return nullptr;   // both operands are inputs, not params
    }
    return nullptr;
}

const char* snInputLabel(fmt::OcSoundNodeKind kind, u32 input) {
    switch (kind) {
        case fmt::OcSoundNodeKind::Gain:
        case fmt::OcSoundNodeKind::LowPass:
        case fmt::OcSoundNodeKind::Adsr:
            return input == 0 ? "In" : nullptr;
        case fmt::OcSoundNodeKind::Mix:
        case fmt::OcSoundNodeKind::Multiply:
            if (input == 0) return "A";
            if (input == 1) return "B";
            return nullptr;
        default:
            return nullptr;
    }
}

void snDefaultParams(fmt::OcSoundNodeKind kind, f32 params[4]) {
    params[0] = params[1] = params[2] = params[3] = 0.0f;
    switch (kind) {
        case fmt::OcSoundNodeKind::Sine:
        case fmt::OcSoundNodeKind::Saw:
            params[0] = 440.0f;                 // concert A, so a fresh oscillator is recognisable
            break;
        case fmt::OcSoundNodeKind::Square:
            params[0] = 440.0f;
            params[1] = 0.5f;                   // the evaluator reads 0 as 0.5 anyway; say it
            break;
        case fmt::OcSoundNodeKind::Const:
            params[0] = 1.0f;                   // unity, so feeding it through a Multiply is a no-op
            break;
        case fmt::OcSoundNodeKind::Gain:
            params[0] = 0.5f;                   // NOT 1: a new Gain should be audibly a gain
            break;
        case fmt::OcSoundNodeKind::LowPass:
            params[0] = 2000.0f;                // takes the edge off a saw without silencing it
            break;
        case fmt::OcSoundNodeKind::Adsr:
            params[0] = 0.01f;                  // a click-free attack
            params[1] = 0.10f;
            params[2] = 0.70f;
            params[3] = 0.20f;
            break;
        case fmt::OcSoundNodeKind::Noise:
        case fmt::OcSoundNodeKind::Mix:
        case fmt::OcSoundNodeKind::Multiply:
            break;                              // nothing to configure
    }
}

i32 snAddNode(fmt::OcSoundData& d, fmt::OcSoundNodeKind kind) {
    fmt::OcSoundNode fresh;
    fresh.kind = kind;
    snDefaultParams(kind, fresh.params);
    d.nodes.push_back(fresh);
    return static_cast<i32>(d.nodes.size()) - 1;
}

i32 snDeleteNode(fmt::OcSoundData& d, i32 index) {
    if (!inRange(d, index)) return -1;
    if (d.nodes.size() <= 1) return -1;   // valid() requires a non-empty graph

    d.nodes.erase(d.nodes.begin() + index);

    std::vector<fmt::OcSoundLink> keep;
    keep.reserve(d.links.size());
    for (const fmt::OcSoundLink& l : d.links) {
        if (static_cast<i32>(l.fromNode) == index || static_cast<i32>(l.toNode) == index) continue;
        fmt::OcSoundLink m = l;
        if (static_cast<i32>(m.fromNode) > index) --m.fromNode;
        if (static_cast<i32>(m.toNode) > index)   --m.toNode;
        keep.push_back(m);
    }
    d.links = std::move(keep);

    // Deleting the output leaves the graph without one, and valid() would reject an out-of-range
    // index. The END rather than the start: see the header for why.
    if (static_cast<i32>(d.outputNode) == index)      d.outputNode = static_cast<u32>(d.nodes.size() - 1);
    else if (static_cast<i32>(d.outputNode) > index)  --d.outputNode;

    sortLinks(d);
    return index < static_cast<i32>(d.nodes.size()) ? index : static_cast<i32>(d.nodes.size()) - 1;
}

void snSetKind(fmt::OcSoundData& d, i32 index, fmt::OcSoundNodeKind kind) {
    if (!inRange(d, index)) return;
    fmt::OcSoundNode& n = d.nodes[static_cast<usize>(index)];
    n.kind = kind;
    snDefaultParams(kind, n.params);

    const u32 inputs = fmt::ocSoundInputCount(kind);
    std::vector<fmt::OcSoundLink> keep;
    keep.reserve(d.links.size());
    for (const fmt::OcSoundLink& l : d.links) {
        // An input the new kind does not have. Dropping the link is what keeps the graph saveable;
        // leaving it would make valid() false and the tab would refuse to write, with the reason
        // several edits behind wherever the author currently is.
        if (static_cast<i32>(l.toNode) == index && l.toInput >= inputs) continue;
        keep.push_back(l);
    }
    d.links = std::move(keep);
}

std::vector<i32> snSourcesOf(const fmt::OcSoundData& d, i32 to, u32 toInput) {
    std::vector<i32> out;
    for (const fmt::OcSoundLink& l : d.links)
        if (static_cast<i32>(l.toNode) == to && l.toInput == toInput)
            out.push_back(static_cast<i32>(l.fromNode));
    return out;
}

i32 snAddLink(fmt::OcSoundData& d, i32 from, i32 to, u32 toInput) {
    if (!inRange(d, from) || !inRange(d, to)) return -1;
    if (from == to) return -1;
    if (toInput >= fmt::ocSoundInputCount(d.nodes[static_cast<usize>(to)].kind)) return -1;
    for (const fmt::OcSoundLink& l : d.links)
        if (static_cast<i32>(l.fromNode) == from && static_cast<i32>(l.toNode) == to &&
            l.toInput == toInput)
            return -1;   // already connected; a second copy would double the signal

    fmt::OcSoundLink add;
    add.fromNode = static_cast<u32>(from);
    add.toNode   = static_cast<u32>(to);
    add.toInput  = toInput;
    d.links.push_back(add);

    const std::vector<i32> newOf = topoRemap(d);
    if (newOf.empty()) {
        // A CYCLE. Put the graph back exactly as it was -- a refused edit must leave no trace, or
        // the author is left with a link they cannot see and a graph that will not save.
        d.links.pop_back();
        return -1;
    }
    applyRemap(d, newOf);
    return newOf[static_cast<usize>(to)];
}

bool snRemoveLink(fmt::OcSoundData& d, i32 from, i32 to, u32 toInput) {
    const auto it = std::find_if(d.links.begin(), d.links.end(),
        [&](const fmt::OcSoundLink& l) {
            return static_cast<i32>(l.fromNode) == from && static_cast<i32>(l.toNode) == to &&
                   l.toInput == toInput;
        });
    if (it == d.links.end()) return false;
    d.links.erase(it);
    // No re-sort: REMOVING an edge can never break an ordering that already held.
    return true;
}

bool snSetOutput(fmt::OcSoundData& d, i32 index) {
    if (!inRange(d, index)) return false;
    d.outputNode = static_cast<u32>(index);
    return true;
}

fmt::OcSoundData snStarterGraph() {
    fmt::OcSoundData d;
    snAddNode(d, fmt::OcSoundNodeKind::Saw);
    d.nodes[0].params[0] = 220.0f;              // an octave below the default A, so it reads as a tone
    snAddNode(d, fmt::OcSoundNodeKind::LowPass);
    snAddNode(d, fmt::OcSoundNodeKind::Adsr);
    snAddLink(d, 0, 1, 0);
    snAddLink(d, 1, 2, 0);
    d.outputNode  = 2;
    d.durationSec = 0.6f;
    return d;
}

// ================================================================================== the tab =======

SoundEditor::SoundEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

SoundEditor::~SoundEditor() { stopPreview(); }

void SoundEditor::loadFromDisk() {
    std::string why;
    fmt::OcSoundData loadedGraph;
    if (!fmt::loadOcSound(path_, loadedGraph, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    graph_ = std::move(loadedGraph);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    // THE OUTPUT, NOT NODE 0. Node 0 is always a source -- the ordering invariant guarantees it --
    // so opening a graph on node 0 always lands on a bare oscillator with no inputs to look at. The
    // output is the node whose panel actually describes the sound, and it is where an author
    // reading somebody else's graph starts.
    selected_ = static_cast<i32>(graph_.outputNode);
    history_.clear();
    previewPcm_.clear();
    previewError_.clear();
}

std::string SoundEditor::title() const {
    // No manual dirty marker: the host passes ImGuiWindowFlags_UnsavedDocument for every editor
    // whose dirty() is true (AssetEditor.cpp), so a '*' here would double it up.
    return std::filesystem::path(path_).filename().string() + "###snd:" + path_;
}

bool SoundEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!fmt::saveOcSound(path_, graph_, why)) return false;
    dirty_ = false;
    return true;
}

void SoundEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS -- BtEditor's own reasoning: reloading here would discard what the
    // author typed because something else touched the file, which is the one outcome an editor must
    // never produce. A clean tab reloads, which is what this callback is actually useful for.
    if (dirty_) {
        AVER_WARN("[SoundEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them",
                  path_);
        return;
    }
    loadFromDisk();
}

void SoundEditor::pushUndo() {
    history_.push(graph_);
}

void SoundEditor::undo() {
    if (!history_.undo(graph_)) return;
    dirty_ = true;
    // The selection is an INDEX, and undo can shrink the array under it.
    if (!inRange(graph_, selected_)) selected_ = 0;
}

void SoundEditor::redo() {
    if (!history_.redo(graph_)) return;
    dirty_ = true;
    if (!inRange(graph_, selected_)) selected_ = 0;
}

void SoundEditor::addNode(fmt::OcSoundNodeKind kind) {
    if (!loaded_) return;
    pushUndo();
    selected_ = snAddNode(graph_, kind);
    dirty_ = true;
}

void SoundEditor::deleteSelected() {
    if (!loaded_) return;
    pushUndo();
    const i32 next = snDeleteNode(graph_, selected_);
    if (next < 0) { history_.cancelPush(); return; }
    selected_ = next;
    dirty_ = true;
}

void SoundEditor::setSelectedKind(fmt::OcSoundNodeKind kind) {
    if (!loaded_ || !inRange(graph_, selected_)) return;
    if (graph_.nodes[static_cast<usize>(selected_)].kind == kind) return;
    pushUndo();
    snSetKind(graph_, selected_, kind);
    dirty_ = true;
}

void SoundEditor::linkInto(i32 from, i32 to, u32 toInput) {
    if (!loaded_) return;
    pushUndo();
    const i32 moved = snAddLink(graph_, from, to, toInput);
    if (moved < 0) { history_.cancelPush(); return; }
    // The re-sort can have moved `to`; follow it, so the panel the author is looking at stays on
    // the node they were wiring rather than jumping to whatever landed at the old index.
    selected_ = moved;
    dirty_ = true;
}

void SoundEditor::unlink(i32 from, i32 to, u32 toInput) {
    if (!loaded_) return;
    pushUndo();
    if (!snRemoveLink(graph_, from, to, toInput)) { history_.cancelPush(); return; }
    dirty_ = true;
}

void SoundEditor::setDuration(f32 seconds) {
    if (!loaded_ || graph_.durationSec == seconds) return;
    pushUndo();
    graph_.durationSec = seconds;
    dirty_ = true;
}

void SoundEditor::makeSelectedOutput() {
    if (!loaded_ || !inRange(graph_, selected_)) return;
    if (static_cast<i32>(graph_.outputNode) == selected_) return;
    pushUndo();
    snSetOutput(graph_, selected_);
    dirty_ = true;
}

bool SoundEditor::renderPreview(std::string* why) {
    previewError_.clear();
    if (!loaded_) {
        previewError_ = "the file failed to load";
        if (why) *why = previewError_;
        return false;
    }
    sound::RenderParams p;
    // A FIXED 48 kHz, not the device's rate. The mixer resamples by the ratio of the sound's own
    // rate to its output rate (Mixer.cpp), so a buffer that declares 48000 plays at the right pitch
    // on a 44.1 kHz device -- and rendering at a fixed rate keeps this function reachable from a
    // headless test, which asking the audio ABI for a rate would not.
    p.sampleRate = 48000;
    p.seed = previewSeed_;
    std::string w;
    if (!sound::renderSound(graph_, p, previewPcm_, &w)) {
        previewPcm_.clear();
        previewError_ = w;
        if (why) *why = w;
        return false;
    }
    bumpPreviewSeed();
    return true;
}

void SoundEditor::stopPreview() {
#if AVER_WITH_AUDIO_ABI
    if (previewVoice_ != 0) { aver_audio_stop(previewVoice_); previewVoice_ = 0; }
    if (previewSound_ != 0) {
        aver_audio_unload(previewSound_);
        // aver_audio_load_pcm is NOT path-cached (see its own comment), so every preview is a new
        // entry that this tab owns and must release. collect() is what actually frees it, and the
        // editor's frame loop does not call it -- so this tab calls it for its own sounds, which is
        // the whole reason a preview handle is tracked rather than fired and forgotten.
        aver_audio_collect();
        previewSound_ = 0;
    }
#endif
}

#if AVER_WITH_IMGUI

void SoundEditor::playPreview() {
    std::string why;
    if (!renderPreview(&why)) {
        AVER_ERROR("[SoundEditor] preview failed for '{}': {}", path_, why);
        return;
    }
#if AVER_WITH_AUDIO_ABI
    stopPreview();
    // Idempotent, and 0 when the machine has no output device -- which the ABI states is not an
    // error. The editor has never opened the device for anything else, so this is where it happens.
    if (aver_audio_init() == 0) {
        previewError_ = "no output device -- the waveform below is still the real render";
        return;
    }
    previewSound_ = aver_audio_load_pcm(previewPcm_.data(), static_cast<i32>(previewPcm_.size()),
                                        1, 48000);
    if (previewSound_ == 0) { previewError_ = "the mixer would not take the buffer"; return; }
    // The UI bus: an editor preview is chrome, so it follows whatever the author has that bus set
    // to rather than competing with a game's SFX mix.
    previewVoice_ = aver_audio_play(previewSound_, 1.0f, 1.0f, 0, AVER_AUDIO_BUS_UI);
#else
    previewError_ = "this build has no audio seam -- the waveform below is still the real render";
#endif
}

void SoundEditor::drawNodeList() {
    for (i32 i = 0; i < static_cast<i32>(graph_.nodes.size()); ++i) {
        const fmt::OcSoundNode& n = graph_.nodes[static_cast<usize>(i)];
        const bool isOutput = static_cast<i32>(graph_.outputNode) == i;
        std::string label = "#" + std::to_string(i) + "  " + snKindName(n.kind);
        // The output is the one node whose role is not visible from its kind, so it says so.
        if (isOutput) label += "   " ICON_VOLUME " out";
        ImGui::PushID(i);
        if (ImGui::Selectable(label.c_str(), selected_ == i)) selected_ = i;
        ImGui::PopID();
    }
}

void SoundEditor::drawDetails() {
    if (!inRange(graph_, selected_)) { ImGui::TextUnformatted("Nothing selected"); return; }
    fmt::OcSoundNode& node = graph_.nodes[static_cast<usize>(selected_)];

    if (ImGui::BeginCombo("Kind", snKindName(node.kind))) {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcSoundNodeKind::Multiply); ++k) {
            const auto kind = static_cast<fmt::OcSoundNodeKind>(k);
            if (ImGui::Selectable(snKindName(kind), kind == node.kind)) setSelectedKind(kind);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changing the kind resets this node's parameters -- a slot means\n"
                          "something different for every kind. Undo restores them.");

    // ---- inputs -------------------------------------------------------------------------------
    const u32 inputs = fmt::ocSoundInputCount(node.kind);
    if (inputs > 0) {
        ImGui::SeparatorText("Inputs");
        for (u32 in = 0; in < inputs; ++in) {
            ImGui::PushID(static_cast<int>(100 + in));
            const char* name = snInputLabel(node.kind, in);
            ImGui::Text("%s", name ? name : "In");

            const std::vector<i32> sources = snSourcesOf(graph_, selected_, in);
            for (const i32 src : sources) {
                ImGui::PushID(src);
                ImGui::BulletText("#%d %s", src,
                                  snKindName(graph_.nodes[static_cast<usize>(src)].kind));
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_CLOSE "##unlink")) unlink(src, selected_, in);
                ImGui::PopID();
            }
            if (sources.empty()) ImGui::TextDisabled("   (unconnected -- reads 0)");
            // MORE THAN ONE SOURCE IS LEGAL AND MEANS SOMETHING: the evaluator sums them. Said out
            // loud here because a graph that quietly got twice as loud is otherwise a mystery.
            if (sources.size() > 1) ImGui::TextDisabled("   (%d sources, summed)",
                                                        static_cast<int>(sources.size()));

            if (ImGui::BeginCombo("##connect", "Connect...")) {
                for (i32 i = 0; i < static_cast<i32>(graph_.nodes.size()); ++i) {
                    if (i == selected_) continue;
                    const std::string label = "#" + std::to_string(i) + "  " +
                                              snKindName(graph_.nodes[static_cast<usize>(i)].kind);
                    if (ImGui::Selectable(label.c_str())) linkInto(i, selected_, in);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }

    // ---- params -------------------------------------------------------------------------------
    bool anyParam = false;
    for (u32 s = 0; s < 4; ++s) if (snParamLabel(node.kind, s)) { anyParam = true; break; }
    if (anyParam) {
        ImGui::SeparatorText("Parameters");
        for (u32 s = 0; s < 4; ++s) {
            const char* label = snParamLabel(node.kind, s);
            if (!label) continue;   // hidden rather than shown as a dead slot -- see the header
            const f32 before = node.params[s];
            ImGui::PushID(static_cast<int>(200 + s));
            // Hz values want a coarser drag than a 0..1 sustain does.
            const bool isHz = std::string(label).find("Hz") != std::string::npos;
            ImGui::DragFloat(label, &node.params[s], isHz ? 1.0f : 0.01f, 0.0f, 0.0f, "%.3f");
            if (ImGui::IsItemDeactivatedAfterEdit() && node.params[s] != before) {
                // ONE DRAG IS ONE UNDO ENTRY, not one per frame -- GraphEditor's own
                // IsItemDeactivatedAfterEdit convention. The drag writes straight into the node
                // every frame; only the release rewinds, records, and re-applies.
                const f32 after = node.params[s];
                node.params[s] = before;
                pushUndo();
                node.params[s] = after;
                dirty_ = true;
            }
            ImGui::PopID();
        }
    } else {
        ImGui::SeparatorText("Parameters");
        ImGui::TextDisabled("This kind has none.");
    }

    ImGui::SeparatorText("Role");
    const bool isOutput = static_cast<i32>(graph_.outputNode) == selected_;
    ImGui::BeginDisabled(isOutput);
    if (ImGui::Button(isOutput ? ICON_VOLUME " This is the output"
                               : ICON_VOLUME " Make this the output")) makeSelectedOutput();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(#%u today)", graph_.outputNode);

    ImGui::BeginDisabled(graph_.nodes.size() <= 1);
    if (ImGui::Button(ICON_DELETE " Delete node")) deleteSelected();
    ImGui::EndDisabled();
    if (graph_.nodes.size() <= 1 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A sound graph must keep at least one node.");
}

void SoundEditor::drawTransport() {
    if (ImGui::Button(ICON_PLAY " Preview")) playPreview();
    ImGui::SameLine();
    ImGui::BeginDisabled(previewVoice_ == 0 && previewSound_ == 0);
    if (ImGui::Button(ICON_STOP " Stop")) stopPreview();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const f32 beforeDur = graph_.durationSec;
    ImGui::DragFloat("Duration (s)", &graph_.durationSec, 0.01f, 0.0f, 0.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit() && graph_.durationSec != beforeDur) {
        // ONE DRAG IS ONE UNDO ENTRY: the drag writes straight into the graph every frame, so the
        // release rewinds to where it started and re-applies through the setter, which is the one
        // place that knows a duration edit is undoable.
        const f32 after = graph_.durationSec;
        graph_.durationSec = beforeDur;
        setDuration(after);
    }

    if (!previewError_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_WARNING " %s", previewError_.c_str());
    }

    if (!previewPcm_.empty()) {
        // DOWNSAMPLED FOR THE PLOT, taking the peak of each bucket rather than every Nth sample. A
        // stride would alias a 440 Hz tone into whatever beat frequency it happens to make with the
        // stride and draw a shape the sound does not have; peaks draw the envelope, which is what
        // somebody looking at a waveform is actually reading.
        constexpr int kPlotPoints = 512;
        static std::vector<f32> plot;
        plot.assign(kPlotPoints, 0.0f);
        const usize per = previewPcm_.size() / kPlotPoints + 1;
        for (int i = 0; i < kPlotPoints; ++i) {
            const usize begin = static_cast<usize>(i) * per;
            f32 peak = 0.0f;
            for (usize s = begin; s < begin + per && s < previewPcm_.size(); ++s)
                if (std::abs(previewPcm_[s]) > std::abs(peak)) peak = previewPcm_[s];
            plot[static_cast<usize>(i)] = peak;
        }
        char overlay[96];
        std::snprintf(overlay, sizeof overlay, "%zu frames @ 48 kHz  (seed %u)",
                      previewPcm_.size(), previewSeed_);
        ImGui::PlotLines("##wave", plot.data(), kPlotPoints, 0, overlay, -1.0f, 1.0f,
                         ImVec2(-1.0f, ImGui::GetTextLineHeight() * 5.0f));
    } else {
        ImGui::TextDisabled("Press Preview to render and hear this graph.");
    }
}

void SoundEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[SoundEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", ICON_ADD " Add node")) {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcSoundNodeKind::Multiply); ++k) {
            const auto kind = static_cast<fmt::OcSoundNodeKind>(k);
            if (ImGui::Selectable(snKindName(kind))) addNode(kind);
        }
        ImGui::EndCombo();
    }

    // A graph can be edited into a state that will not save (an output pointing nowhere after an
    // undo, say). Said HERE, continuously, rather than only when Save is pressed -- an editor that
    // reports a structural problem at the moment you try to leave is reporting it too late.
    if (!graph_.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_WARNING " this graph will not save");
    }

    ImGui::Separator();

    // THE TRANSPORT'S HEIGHT IS MEASURED, NOT GUESSED. A raw pixel reserve here was wrong the
    // moment it met a 300% display: the button row and the waveform both scale with the font, the
    // constant did not, and the bottom row was clipped off the window. draw() carries no dpi (see
    // the header), so the scale is taken from the font metrics, which already have it.
    const f32 rowH    = ImGui::GetFrameHeightWithSpacing();
    const f32 plotH   = ImGui::GetTextLineHeight() * 5.0f;
    const f32 reserve = rowH + plotH + ImGui::GetStyle().ItemSpacing.y * 4.0f;
    const f32 listW = ImGui::GetContentRegionAvail().x * 0.32f;
    // Never squeezes the panes to nothing on a short window -- the transport scrolls away instead.
    const f32 paneH = std::max(ImGui::GetContentRegionAvail().y - reserve, rowH * 3.0f);
    if (ImGui::BeginChild("##sndlist", ImVec2(listW, paneH), true)) drawNodeList();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##snddetails", ImVec2(0, paneH), true)) drawDetails();
    ImGui::EndChild();

    ImGui::Separator();
    drawTransport();
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's target, which deliberately leaves AVER_WITH_IMGUI
// undefined -- see tests/editor/CMakeLists.txt) still gets load / save / undo / edits and
// renderPreview(); only the window is absent. GraphEditor.cpp and BtEditor.cpp do exactly this.
void SoundEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeSoundEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocsnd") return nullptr;
    return std::make_unique<SoundEditor>(path);
}

} // namespace aver::editor
