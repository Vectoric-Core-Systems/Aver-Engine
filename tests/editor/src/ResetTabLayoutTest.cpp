// ResetTabLayoutTest -- "Reset Tab Layout" reaches the FOCUSED editor, not merely the first or any
// open one.
//
// THE DEFECT THIS REPRODUCES. SandboxApp.cpp gated the menu item on `assetTabActive = !levelVisible_
// && assetEditors_.anyOpen()` -- one bool, no per-editor-type branch -- and then unconditionally
// called editor::resetActorEditorLayout(). So opening a Sound tab and a Graph tab together, with the
// Graph tab in front, and clicking "Reset Tab Layout" reset the ACTOR editor's layout: not the
// Sound tab's, not the focused Graph tab's, always the Actor editor's, regardless of whether one was
// even open. AssetEditorHost::resetFocusedLayout() is the fix: it calls resetLayout() on whichever
// editor find(focusedPath_) names.
//
// NO IMGUI, NO WINDOW. AssetEditorHost::draw() sets focusedPath_ from ImGui::IsWindowFocused() once
// per tab every real frame; setFocusedPathForTest() stands in for that so this proves the DISPATCH
// logic without a live frame -- see its own comment in AssetEditor.hpp.
//
// TWO FAKE EDITORS, not one, because a dispatch bug that always reaches editor A passes a test with
// only editor A in it. AssetEditorFactory is a plain function pointer (no captured state, matching
// every real factory's own shape -- see makeActorEditor/makeBtEditor/makeGraphEditor/makeSoundEditor),
// so each fake is told apart by its own file extension rather than by a captured id.
#include "AssetEditor.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>
#include <memory>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

struct FakeEditorA final : AssetEditor {
    explicit FakeEditorA(std::string p) : path_(std::move(p)) {}
    const std::string& path() const override { return path_; }
    std::string title() const override { return "FakeA"; }
    void draw(Engine&) override {}
    void resetLayout() override { ++resetCount; }
    std::string path_;
    int resetCount = 0;
};

struct FakeEditorB final : AssetEditor {
    explicit FakeEditorB(std::string p) : path_(std::move(p)) {}
    const std::string& path() const override { return path_; }
    std::string title() const override { return "FakeB"; }
    void draw(Engine&) override {}
    void resetLayout() override { ++resetCount; }
    std::string path_;
    int resetCount = 0;
};

std::unique_ptr<AssetEditor> makeFakeA(const std::string& path) {
    if (std::filesystem::path(path).extension() != ".fakea") return nullptr;
    return std::make_unique<FakeEditorA>(path);
}

std::unique_ptr<AssetEditor> makeFakeB(const std::string& path) {
    if (std::filesystem::path(path).extension() != ".fakeb") return nullptr;
    return std::make_unique<FakeEditorB>(path);
}

} // namespace

int main() {
    AVER_INFO("ResetTabLayoutTest");

    AssetEditorHost host;
    host.registerFactory(&makeFakeA);
    host.registerFactory(&makeFakeB);

    check(host.open("tabA.fakea"), "tab A opens through its factory");
    check(host.open("tabB.fakeb"), "tab B opens through its factory");
    check(host.count() == 2, "both tabs are tracked");

    auto* a = static_cast<FakeEditorA*>(host.find("tabA.fakea"));
    auto* b = static_cast<FakeEditorB*>(host.find("tabB.fakeb"));
    check(a != nullptr && b != nullptr, "both tabs are reachable by path");

    AVER_INFO("dispatch reaches the FOCUSED editor, and leaves every other open one alone");
    host.setFocusedPathForTest("tabB.fakeb");
    host.resetFocusedLayout();
    check(a->resetCount == 0, "A's layout was left alone while B was focused");
    check(b->resetCount == 1, "B's layout was reset -- B was the focused tab");

    host.setFocusedPathForTest("tabA.fakea");
    host.resetFocusedLayout();
    check(a->resetCount == 1, "A's layout resets once IT becomes the focused tab");
    check(b->resetCount == 1, "and B is untouched this time -- not the reverse of the old bug");

    AVER_INFO("no open tab named by focus (e.g. focus lost, or the level view was in front) resets nothing");
    host.setFocusedPathForTest("");
    host.resetFocusedLayout();
    check(a->resetCount == 1 && b->resetCount == 1, "neither fake editor moved");

    host.setFocusedPathForTest("no/such/tab.fakea");
    host.resetFocusedLayout();
    check(a->resetCount == 1 && b->resetCount == 1, "and naming a path nothing has open is also a no-op");

    AVER_INFO(g_failures ? "ResetTabLayoutTest: {} FAILURES" : "ResetTabLayoutTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
