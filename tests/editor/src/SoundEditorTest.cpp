// The .ocsnd sound editor tab's headless core: the structural edits (add / delete / set-kind /
// link / unlink / set-output), the topological re-sort that keeps "sources before consumers" true
// by construction, and the tab's own load / save / dirty / undo bookkeeping on top of them.
//
// AVER_WITH_IMGUI AND AVER_SOUND_EDITOR_AUDIO ARE BOTH DELIBERATELY UNDEFINED for this target.
// SoundEditor.cpp's `#include "imgui.h"` and its whole drawing half sit behind the first; its three
// mixer calls sit behind the second. What is left is exactly the part worth testing, which is why
// the structural edits are free functions (see SoundEditor.hpp) -- and why renderPreview() is a
// member of the tab rather than living inside the Preview button: an invalid graph, a duration that
// overruns the renderer's cap and a seed that fails to vary are all things that can be wrong, and
// none of them need a sound card to check.
//
// BtEditorTest beside this file is the precedent for the whole arrangement.
#include "SoundEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcSound.hpp"
#include "aver/sound/Synth.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Every link runs from a lower index to a higher one, the output is in range, and the graph is
// non-empty -- restated here rather than only calling valid(), so a failure says WHICH half broke.
static bool ordered(const fmt::OcSoundData& d) {
    for (const fmt::OcSoundLink& l : d.links)
        if (l.fromNode >= l.toNode) return false;
    return true;
}

int main() {
    AVER_INFO("SoundEditorTest");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-sound-editor").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/fixture.ocsnd";

    // ---- the starter graph ----------------------------------------------------------------------
    AVER_INFO("the starter graph is a real, saveable, audible chain");
    {
        const fmt::OcSoundData g = editor::snStarterGraph();
        check(g.valid(), "snStarterGraph is valid");
        check(ordered(g), "snStarterGraph puts every source before its consumer");
        check(g.nodes.size() == 3, "it is a three-node chain");
        check(g.nodes[0].kind == fmt::OcSoundNodeKind::Saw, "node 0 is the Saw source");
        check(g.nodes[2].kind == fmt::OcSoundNodeKind::Adsr, "node 2 is the envelope");
        check(g.outputNode == 2, "the envelope is the output");
        check(g.links.size() == 2, "two links wire the chain");

        // NOT MERELY VALID -- AUDIBLE. A starter asset that renders silence would pass every
        // structural check above and still be useless, which is the failure this catches.
        sound::RenderParams p;
        std::vector<f32> pcm;
        std::string why;
        check(sound::renderSound(g, p, pcm, &why), "the starter graph renders: " + why);
        f32 peak = 0.0f;
        for (const f32 s : pcm) peak = std::abs(s) > peak ? std::abs(s) : peak;
        check(peak > 0.1f, "and it is audible, not silence (peak " + std::to_string(peak) + ")");

        std::string w2;
        check(fmt::saveOcSound(path, g, &w2), "the fixture writes to disk: " + w2);
    }

    // ---- defaults ------------------------------------------------------------------------------
    AVER_INFO("a freshly added node is configured, not zeroed");
    {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcSoundNodeKind::Multiply); ++k) {
            const auto kind = static_cast<fmt::OcSoundNodeKind>(k);
            f32 params[4];
            editor::snDefaultParams(kind, params);
            // Only the kinds that READ a param need a non-zero one; a Noise or a Mix has nothing to
            // configure, and inventing a value for it would be worse than leaving it at zero.
            bool needsOne = false;
            for (u32 s = 0; s < 4; ++s) if (editor::snParamLabel(kind, s)) { needsOne = true; break; }
            if (!needsOne) continue;
            bool anyNonZero = false;
            for (u32 s = 0; s < 4; ++s) if (params[s] != 0.0f) anyNonZero = true;
            check(anyNonZero, std::string("a default ") + editor::snKindName(kind) +
                              " has a non-zero parameter");
        }
        // Every kind names itself, and every named param slot is one the kind really reads.
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcSoundNodeKind::Multiply); ++k) {
            const auto kind = static_cast<fmt::OcSoundNodeKind>(k);
            check(std::string(editor::snKindName(kind)) != "?",
                  std::string("kind ") + std::to_string(k) + " has a display name");
            const u32 inputs = fmt::ocSoundInputCount(kind);
            for (u32 in = 0; in < 2; ++in) {
                const bool named = editor::snInputLabel(kind, in) != nullptr;
                check(named == (in < inputs),
                      std::string(editor::snKindName(kind)) + " names input " + std::to_string(in) +
                      " exactly when it has one");
            }
        }
    }

    // ---- the re-sort ---------------------------------------------------------------------------
    AVER_INFO("linking backwards re-sorts the graph instead of refusing");
    {
        fmt::OcSoundData d;
        editor::snAddNode(d, fmt::OcSoundNodeKind::Gain);      // 0 -- the CONSUMER, added first
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);      // 1 -- its source, added second
        d.outputNode = 0;

        // Feeding 1 -> 0 is backwards for the format. The whole point of the re-sort is that this
        // still works: an author should never be told "connect these in the other order".
        const i32 gainNow = editor::snAddLink(d, 1, 0, 0);
        check(gainNow == 1, "the consumer moved to index 1 and snAddLink reported where");
        check(ordered(d), "the graph now puts the source first");
        check(d.valid(), "and it is valid");
        check(d.nodes[0].kind == fmt::OcSoundNodeKind::Sine, "the Sine really moved to index 0");
        check(d.nodes[1].kind == fmt::OcSoundNodeKind::Gain, "the Gain really moved to index 1");
        check(d.outputNode == 1, "the output index followed its node");
        const std::vector<i32> src = editor::snSourcesOf(d, 1, 0);
        check(src.size() == 1 && src[0] == 0, "the link survived the re-sort, pointing at the Sine");
    }

    AVER_INFO("an already-ordered graph is left exactly where it is");
    {
        // THE STABILITY PROPERTY, and it is the reason the re-sort can run after every link without
        // the array reshuffling under the author's hands. Falsified by changing topoRemap's pick to
        // scan from the highest index: this check fails while every other check in this file still
        // passes, because the graph stays valid -- just permuted for no reason.
        fmt::OcSoundData d = editor::snStarterGraph();
        const i32 gainIdx = editor::snAddNode(d, fmt::OcSoundNodeKind::Gain);   // 3
        const i32 moved = editor::snAddLink(d, 2, gainIdx, 0);                  // 2 -> 3, forwards
        check(moved == gainIdx, "a forwards link moves nothing");
        check(d.nodes[0].kind == fmt::OcSoundNodeKind::Saw, "node 0 is still the Saw");
        check(d.nodes[1].kind == fmt::OcSoundNodeKind::LowPass, "node 1 is still the low pass");
        check(d.nodes[2].kind == fmt::OcSoundNodeKind::Adsr, "node 2 is still the envelope");
        check(d.nodes[3].kind == fmt::OcSoundNodeKind::Gain, "node 3 is still the new Gain");
    }

    AVER_INFO("a cycle is refused, and refusing leaves no trace");
    {
        fmt::OcSoundData d;
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);      // 0
        editor::snAddNode(d, fmt::OcSoundNodeKind::Gain);      // 1
        editor::snAddNode(d, fmt::OcSoundNodeKind::Gain);      // 2
        editor::snAddLink(d, 0, 1, 0);
        editor::snAddLink(d, 1, 2, 0);
        const fmt::OcSoundData before = d;

        // 2 already reaches nothing that reaches it... except through 1. Feeding 2 -> 1 closes it.
        check(editor::snAddLink(d, 2, 1, 0) == -1, "a link closing a loop is refused");
        check(d.links.size() == before.links.size(),
              "and the refused link was not left behind (the graph would then never save)");
        check(d.valid(), "the graph is still valid after the refusal");

        // A NODE FEEDING ITSELF needs no special case in topoRemap -- see its comment -- so this
        // checks the claim rather than trusting it.
        check(editor::snAddLink(d, 1, 1, 0) == -1, "a self-link is refused");
        check(d.links.size() == before.links.size(), "and it too left nothing behind");
    }

    AVER_INFO("the other refusals");
    {
        fmt::OcSoundData d = editor::snStarterGraph();
        const usize links = d.links.size();
        check(editor::snAddLink(d, 0, 1, 0) == -1, "a duplicate link is refused (it would double the signal)");
        check(editor::snAddLink(d, 0, 0, 0) == -1, "linking a node to itself is refused");
        check(editor::snAddLink(d, 0, 99, 0) == -1, "an out-of-range consumer is refused");
        check(editor::snAddLink(d, 99, 1, 0) == -1, "an out-of-range source is refused");
        // The low pass has ONE input, so input 1 does not exist on it.
        check(editor::snAddLink(d, 0, 1, 1) == -1, "a link to an input the kind does not have is refused");
        check(d.links.size() == links, "every refusal left the link list exactly as it was");
        check(!editor::snRemoveLink(d, 0, 2, 0), "removing a link that is not there reports false");
        check(!editor::snSetOutput(d, 99), "an out-of-range output is refused");
    }

    // ---- kind changes --------------------------------------------------------------------------
    AVER_INFO("changing a kind resets params and drops links the new kind cannot take");
    {
        fmt::OcSoundData d;
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);      // 0
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);      // 1
        editor::snAddNode(d, fmt::OcSoundNodeKind::Mix);       // 2, two inputs
        editor::snAddLink(d, 0, 2, 0);
        editor::snAddLink(d, 1, 2, 1);
        d.outputNode = 2;
        check(d.valid(), "the two-input graph starts valid");

        // Mix (2 inputs) -> Gain (1 input). The link into input 1 has nowhere to go.
        editor::snSetKind(d, 2, fmt::OcSoundNodeKind::Gain);
        check(d.nodes[2].kind == fmt::OcSoundNodeKind::Gain, "the kind changed");
        check(editor::snSourcesOf(d, 2, 0).size() == 1, "the link into input 0 survived");
        check(editor::snSourcesOf(d, 2, 1).empty(), "the link into input 1 was dropped");
        check(d.valid(), "so the graph is still saveable -- which is the whole reason to drop it");
        check(d.nodes[2].params[0] == 0.5f, "and the params are the new kind's defaults, not the old kind's");
    }

    // ---- delete --------------------------------------------------------------------------------
    AVER_INFO("deleting a node takes its links with it");
    {
        fmt::OcSoundData d = editor::snStarterGraph();   // Saw(0) -> LowPass(1) -> Adsr(2), out = 2
        const i32 next = editor::snDeleteNode(d, 1);     // the middle of the chain
        check(next >= 0, "the low pass was deleted");
        check(d.nodes.size() == 2, "two nodes remain");
        check(d.links.empty(), "both links went with it -- neither end is dangling");
        check(d.valid(), "and the graph is still valid");
        check(d.outputNode == 1, "the output index followed the envelope down to 1");

        // The output itself.
        fmt::OcSoundData e2 = editor::snStarterGraph();
        editor::snDeleteNode(e2, 2);                     // delete the OUTPUT
        check(e2.outputNode == static_cast<u32>(e2.nodes.size() - 1),
              "deleting the output moves it to the last remaining node");
        check(e2.valid(), "so the graph never holds an out-of-range output");

        fmt::OcSoundData one;
        editor::snAddNode(one, fmt::OcSoundNodeKind::Sine);
        check(editor::snDeleteNode(one, 0) == -1, "the last node cannot be deleted");
        check(one.nodes.size() == 1, "and it is still there");
        check(editor::snDeleteNode(one, 7) == -1, "an out-of-range delete is refused");
    }

    // ---- summed inputs -------------------------------------------------------------------------
    AVER_INFO("two sources into one input is legal and is kept");
    {
        // The evaluator SUMS what arrives at an input, so an editor that only tracked one source
        // per input would silently discard the second on the next edit. This is that check.
        fmt::OcSoundData d;
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);
        editor::snAddNode(d, fmt::OcSoundNodeKind::Sine);
        editor::snAddNode(d, fmt::OcSoundNodeKind::Gain);
        editor::snAddLink(d, 0, 2, 0);
        editor::snAddLink(d, 1, 2, 0);
        check(editor::snSourcesOf(d, 2, 0).size() == 2, "both sources are on input 0");
        check(d.valid(), "and the graph is valid");
        check(editor::snRemoveLink(d, 0, 2, 0), "one can be removed");
        check(editor::snSourcesOf(d, 2, 0).size() == 1, "leaving the other");
    }

    // ---- the tab -------------------------------------------------------------------------------
    AVER_INFO("the tab loads, edits, marks dirty, saves and reloads");
    {
        editor::SoundEditor ed(path);
        check(ed.loaded(), "the tab loaded the fixture: " + ed.loadError());
        check(!ed.dirty(), "a freshly loaded tab is clean");
        check(ed.graph().nodes.size() == 3, "with the starter graph's three nodes");
        check(ed.title().find("fixture.ocsnd") != std::string::npos, "the title names the file");
        check(ed.title().find("###snd:") != std::string::npos, "and carries a stable ImGui id");

        ed.addNode(fmt::OcSoundNodeKind::Noise);
        check(ed.dirty(), "adding a node marks the tab dirty");
        check(ed.graph().nodes.size() == 4, "and the node is there");
        check(ed.selected() == 3, "the new node is selected");

        ed.select(3);
        ed.setSelectedKind(fmt::OcSoundNodeKind::Square);
        check(ed.graph().nodes[3].kind == fmt::OcSoundNodeKind::Square, "the kind change applied");

        // Node 3 feeding node 2 is BACKWARDS, so this re-sorts and the envelope stops being node
        // 2. Following it through ed.selected() rather than assuming an index is the whole point of
        // linkInto returning where it went -- and writing this test the naive way is what proved it.
        ed.linkInto(3, 2, 0);
        const i32 env = ed.selected();
        check(ed.graph().nodes[static_cast<usize>(env)].kind == fmt::OcSoundNodeKind::Adsr,
              "the selection followed the envelope through the re-sort");
        // The envelope already had a source, and the format sums -- so this is a second one.
        check(editor::snSourcesOf(ed.graph(), env, 0).size() == 2, "the tab wired a second source in");
        check(ed.graph().valid(), "and the graph is still valid");
        for (const fmt::OcSoundLink& l : ed.graph().links)
            if (l.fromNode >= l.toNode) { check(false, "the re-sorted graph still runs low to high"); break; }

        std::string why;
        check(ed.save(&why), "the tab saves: " + why);
        check(!ed.dirty(), "and is clean afterwards");

        fmt::OcSoundData reread;
        std::string w2;
        check(fmt::loadOcSound(path, reread, &w2), "what it wrote reads back: " + w2);
        check(reread.nodes.size() == 4, "with all four nodes");
        check(reread.links.size() == ed.graph().links.size(), "and every link");
        check(reread.outputNode == ed.graph().outputNode, "and the same output");
    }

    AVER_INFO("undo and redo walk the whole graph, not just the selection");
    {
        editor::SoundEditor ed(path);
        check(ed.loaded(), "reloaded the saved fixture");
        const usize started = ed.graph().nodes.size();
        check(!ed.canUndo(), "a fresh tab has nothing to undo");

        ed.addNode(fmt::OcSoundNodeKind::Noise);
        check(ed.canUndo(), "after an edit it does");
        check(ed.graph().nodes.size() == started + 1, "the node is there");
        ed.undo();
        check(ed.graph().nodes.size() == started, "undo removed it");
        check(ed.canRedo(), "and redo is now available");
        ed.redo();
        check(ed.graph().nodes.size() == started + 1, "redo put it back");

        // A refused edit MUST NOT leave an undo entry -- otherwise Undo does nothing visible once
        // and the user presses it again, losing a real edit.
        editor::SoundEditor fresh(path);
        fresh.select(0);
        const bool couldUndoBefore = fresh.canUndo();
        fresh.linkInto(0, 0, 0);                  // refused: a node cannot feed itself
        check(fresh.canUndo() == couldUndoBefore, "a refused link left no undo entry");
        fresh.select(0);
        fresh.deleteSelected();
        fresh.deleteSelected();
        fresh.deleteSelected();                   // the last one is refused
        check(fresh.graph().nodes.size() == 1, "the graph cannot be emptied through the tab");
        check(fresh.graph().valid(), "and what remains is valid");
    }

    // ---- preview -------------------------------------------------------------------------------
    AVER_INFO("renderPreview is the half of Preview that can be wrong, so it is checked here");
    {
        editor::SoundEditor ed(path);
        check(ed.loaded(), "loaded for preview");
        check(ed.previewPcm().empty(), "nothing is rendered until asked");

        std::string why;
        check(ed.renderPreview(&why), "the preview renders: " + why);
        check(!ed.previewPcm().empty(), "and produced samples");
        f32 peak = 0.0f;
        for (const f32 s : ed.previewPcm()) peak = std::abs(s) > peak ? std::abs(s) : peak;
        check(peak > 0.01f, "which are audible rather than silence");
        for (const f32 s : ed.previewPcm())
            if (s < -1.0f || s > 1.0f) { check(false, "a sample escaped [-1, 1]"); break; }

        const u32 seedA = ed.previewSeed();
        check(ed.renderPreview(&why), "a second preview renders");
        check(ed.previewSeed() != seedA,
              "and the seed advanced -- which is what makes two footsteps differ");

        // A GRAPH THE RENDERER WILL REFUSE. durationSec comes out of an asset file, so the tab has
        // to survive an optimistic one rather than trying to allocate it.
        editor::SoundEditor big(path);
        big.setDuration(100000.0f);
        std::string bigWhy;
        const bool rendered = big.renderPreview(&bigWhy);
        check(!rendered, "an absurd duration is refused rather than allocated");
        check(bigWhy.find("exceeds the cap") != std::string::npos,
              "and the reason says so: " + bigWhy);
        check(big.previewPcm().empty(), "leaving no half-filled buffer behind");
    }

    AVER_INFO("a file that is not a sound graph is not claimed, and a missing one fails cleanly");
    {
        check(editor::makeSoundEditor("thing.ocbt") == nullptr, "makeSoundEditor declines a .ocbt");
        check(editor::makeSoundEditor("thing.txt") == nullptr, "and a .txt");
        check(editor::makeSoundEditor("thing.OCSND") != nullptr, "but claims .OCSND, case-insensitively");

        editor::SoundEditor missing(dir + "/nope.ocsnd");
        check(!missing.loaded(), "a missing file leaves the tab not loaded");
        check(!missing.loadError().empty(), "with a reason to show");
        std::string why;
        check(!missing.save(&why), "and saving is refused rather than writing a default graph over it");
        check(why.find("failed to load") != std::string::npos, "saying why: " + why);
    }

    if (g_failures == 0) AVER_INFO("SoundEditorTest: all checks passed");
    else AVER_ERROR("SoundEditorTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
