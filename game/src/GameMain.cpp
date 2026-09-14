// AverGame.exe -- the standalone game runtime. The second aver::Application in the tree, and the
// first one that is not an editor.
//
// Deliberately tiny. Everything is in Aver.Runtime.Game so that the editor can eventually CALL the
// same code rather than keep its own copy of it; an executable that accumulates logic is how
// Sandbox.exe became impossible to ship a game out of.
#include "aver/game/GameApp.hpp"
#include "aver/runtime/EntryPoint.hpp"

// AverSR (3.3 C, contract C2-12): THE ONLY TRANSLATION UNIT IN AverGame.exe ALLOWED TO INCLUDE
// aver/sr/* OR NAME sr::Quality. game/CMakeLists.txt links Aver.Render.Sr to the AverGame EXECUTABLE
// target alone, never to Aver.Runtime.Game (see that file's own `if(TARGET Aver.Render.Sr)` block),
// so GameApp.hpp/.cpp cannot see these headers even if something in them wanted to -- the module
// boundary render.voxi and runtime.game must respect (Scalability.hpp's own header comment:
// "render.voxi must never include render.sr") is therefore enforced by the link graph, not merely by
// convention. GameApp learns none of this: it is handed only a plain function pointer
// (GameApp::AverSrInstaller) whose signature never names sr::anything.
//
// AVER_MODULE_VOXI IS ALSO REQUIRED FOR THE CROSS-CHECK BELOW, and deliberately narrows the guard
// past what 3.2's own text says ("under AVER_MODULE_SR"): the ladder numbering being checked
// (aver::voxi::ladder::kAverSrOff..kAverSrPerformance) is declared in a render.voxi header, reachable
// only when Aver.Render.Voxi is linked (modules/runtime.game/CMakeLists.txt's own `if(TARGET
// Aver.Render.Voxi)` block) -- an AVER_MODULE_SR=1, AVER_MODULE_VOXI=0 tree is not a configuration
// this engine ships (AverSR scales what Voxi renders; there is no other renderer), but nesting the
// guard is what keeps that hypothetical tree CONFIGURING at all rather than failing to find the
// header. installAverSr itself, a few lines down, is guarded on AVER_MODULE_SR alone, matching
// GameApp::onInit's own AVER_MODULE_VOXI-gated call site -- so in that hypothetical tree the function
// would simply never be reachable, which is a harmless dead definition, not a compile error.
#if AVER_MODULE_SR
#  include "aver/sr/AverSrQuality.hpp"
#  include "aver/sr/AverSrSpatial.hpp"
#  if AVER_MODULE_VOXI
#    include "aver/voxi/QualityLadder.hpp"
// Cross-checked here, not assumed: GameApp::onInit resolves an AverSR level as a plain u32 through
// aver::voxi::resolveAverSrLevel (the ladder's own kAverSrOff..kAverSrPerformance numbering), and
// installAverSr below is the ONLY place that u32 is ever reinterpreted as an aver::sr::Quality. A
// silent renumbering on either side -- the ladder or sr::Quality -- would install the WRONG quality
// level under the right printed name, and nothing else in this tree would notice. Mirrors
// SandboxApp.cpp's own guard beside its aver/sr includes (3.2), same four static_asserts.
static_assert(static_cast<aver::u32>(aver::sr::Quality::Off) == aver::voxi::ladder::kAverSrOff,
              "AverSR numbering drifted: sr::Quality::Off no longer matches ladder::kAverSrOff");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Quality) == aver::voxi::ladder::kAverSrQuality,
              "AverSR numbering drifted: sr::Quality::Quality no longer matches ladder::kAverSrQuality");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Balanced) == aver::voxi::ladder::kAverSrBalanced,
              "AverSR numbering drifted: sr::Quality::Balanced no longer matches ladder::kAverSrBalanced");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Performance) == aver::voxi::ladder::kAverSrPerformance,
              "AverSR numbering drifted: sr::Quality::Performance no longer matches ladder::kAverSrPerformance");
#  endif
#endif

#include <memory>

namespace aver {

#if AVER_MODULE_SR
namespace {

// game::GameApp::AverSrInstaller (3.3 C): builds a concrete SpatialUpscaler against `dev`'s resource
// factory and reports the render scale for `level` -- the one function this executable hands GameApp
// so it can install AverSR without ever naming sr::anything itself. `out` is GameApp's own
// averSrUpscaler_ member, handed back by reference so ownership stays with GameApp exactly the way
// SandboxApp.cpp's own averSrUpscaler_ owns its upscaler; see GameApp::onShutdown for the matching
// teardown order this mirrors (setUpscaler(nullptr) precedes the unique_ptr's reset).
//
// LEVEL IS A PLAIN u32 -- the quality ladder's own kAverSrOff..kAverSrPerformance numbering, not an
// sr::Quality -- because GameApp::AverSrInstaller's signature cannot name a type only THIS
// translation unit can see. The static_asserts above are what makes reinterpreting it as one safe;
// resolveAverSrLevel (Scalability.hpp) already clamps it to 0..3 before GameApp ever calls this.
//
// FALSE ONLY WHEN THERE IS NO RESOURCE FACTORY (a headless/mock device) -- SandboxApp.cpp's own
// ensureAverSrUpscaler guards the identical case the same way. `out` is left untouched on failure, so
// a transient miss does not discard an upscaler a previous, successful call already built.
bool installAverSr(rhi::IDevice& dev, u32 level, std::unique_ptr<rhi::IUpscaler>& out, f32& renderScale) {
    rhi::IResourceFactory* res = dev.resources();
    if (!res) return false;
    if (!out) out = std::make_unique<sr::SpatialUpscaler>(*res);
    renderScale = sr::renderScaleFor(static_cast<sr::Quality>(level));
    return true;
}

} // namespace
#endif

Application* createApplication(int argc, char** argv) {
    auto* app = new game::GameApp(game::parseArgs(argc, argv));
#if AVER_MODULE_SR
    // The only setAverSrInstaller call in the tree for this host. Left uncalled -- averSrInstaller_
    // stays null -- in a build with Aver.Render.Sr absent; GameApp::onInit's own comment says why that
    // has to be a legal, non-fatal state rather than an assumption this file gets to make.
    app->setAverSrInstaller(&installAverSr);
#endif
    return app;
}

} // namespace aver
