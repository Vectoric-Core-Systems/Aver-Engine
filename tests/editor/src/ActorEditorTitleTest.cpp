// ActorEditorTitleTest -- the actor tab's title never carries a manual dirty marker.
//
// THE DEFECT. ActorEditor::title() used to be `dirty_ ? title_ + " *" : title_`, but
// AssetEditorHost::draw (AssetEditor.cpp) already passes ImGuiWindowFlags_UnsavedDocument to
// ImGui::Begin whenever an editor's dirty() is true, so ImGui draws its OWN dirty dot on the tab.
// The manual " *" doubled it. SoundEditor::title() and BtEditor::title() already document this
// exact rule with a comment and omit the suffix; ActorEditor did not.
//
// WHY THIS TESTS actorTabTitle, NOT ActorEditor and its private dirty_ flag DIRECTLY. ActorEditor is
// a file-local class defined entirely inside ActorEditor.cpp (in its anonymous namespace) and
// reachable only through the AssetEditor interface makeActorEditor() returns -- there is no header
// declaring the class itself, unlike BtEditor/GraphEditor/SoundEditor. Its dirty_ flag has no public
// setter: every `dirty_ = true` in that file sits inside draw()'s own `#if AVER_WITH_IMGUI` body,
// set only by live mouse-drag gizmo interaction, which no headless caller can simulate. Forcing that
// flag true just for this test would mean either exposing a private member across the AssetEditor
// interface for every editor type, or giving ActorEditor a public test-only setter that does not
// exist for any other reason -- both a larger change than "drop the suffix" calls for.
//
// actorTabTitle (declared in ActorEditor.hpp, alongside the class that calls it) is the actual
// function ActorEditor::title() calls, extracted for exactly this reason -- the same
// EditorEulerTest/DropPlacementTest/AssetRefScanTest precedent of pulling placement-adjacent
// arithmetic into a small Core-only header specifically so a headless suite can reach it. Since the
// function ignores its `dirty` parameter BY DESIGN, asserting both values here proves "regardless of
// dirty()" against the real code path title() runs, not merely against a comment.
#include "ActorEditor.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("ActorEditorTitleTest");

    const std::string base = "Guard.Designer.cs";

    AVER_INFO("the bare title comes back unchanged when the tab is clean");
    check(actorTabTitle(base, false) == base, "actorTabTitle(base, false) == base");

    AVER_INFO("and REGARDLESS OF DIRTY -- no manual '*' is ever appended");
    check(actorTabTitle(base, true) == base, "actorTabTitle(base, true) == base, not base + \" *\"");
    check(actorTabTitle(base, true).find('*') == std::string::npos,
          "the dirty title carries no '*' at all -- the host's own UnsavedDocument flag is the ONLY marker");

    AVER_INFO(g_failures ? "ActorEditorTitleTest: {} FAILURES" : "ActorEditorTitleTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
