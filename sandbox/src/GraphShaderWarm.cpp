// A level's new material graphs compile in the background instead of on the render thread.
//
// A material graph is compiled into every scene shader (MaterialGraphRegistry.hpp), so a level that brings a new one used to
// recompile all of Voxi's scene pipelines inside one frame: a minute or two of a window Windows calls "not responding". Here the
// editor holds that rebuild (VoxiRenderer::holdGraphRebuild), starts a second copy of itself in --warm-shaders --graphs mode
// (ShaderWarmRun.cpp) that registers the same graphs under the same ids and compiles every variant into the shared blob cache, and
// releases the hold when that run ends; the rebuild then reads the cache. Meanwhile the scene is not drawn (pipelines that lack a
// graph must not draw its materials) and the editor stays usable, with a notification saying why.
//
// The run reports the fingerprint of the shader text its graphs compose to. When it differs from this process's (a graph edited
// in memory and not saved, a file that no longer loads), the hold is released at once and the rebuild compiles as it always did.
// Only the interactive editor on D3D12 does this: the blob cache is the D3D12 backend's, and bounded runs (--frames) keep the
// synchronous rebuild so they stay deterministic.
#include "SandboxApp.hpp"

#if AVER_MODULE_VOXI && AVER_MODULE_PBR
#include "aver/core/Log.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <fstream>
#endif

namespace aver {

void SandboxApp::tickGraphShaderWarm(Engine& e) {
#if defined(_WIN32) && AVER_MODULE_VOXI && AVER_MODULE_PBR
    graphWarmup_.poll();
    const auto release = [this](const char* why) {
        voxiRenderer_.holdGraphRebuild(false);
        graphWarmHeld_ = false;
        AVER_INFO("[ShaderWarm] material graphs: {}; the scene pipelines rebuild now", why);
    };
    pbr::MaterialGraphRegistry& reg = pbr::materialGraphs();
    const bool can = rset_.voxiAttached_ && e.window() != nullptr && maxFrames_ == 0 && e.device() != nullptr &&
                     e.device()->backend() == rhi::Backend::D3D12;
    if (!can || reg.count() == 0 || !voxiRenderer_.graphRebuildPending()) {
        if (graphWarmHeld_) {
            graphWarmup_.cancel();
            release("nothing left to wait for");
        }
        return;
    }

    const u64 rev = reg.revision();
    if (!graphWarmHeld_ || graphWarmRev_ != rev) {
        // A new set of graphs (or a newer one while a run was going): one run for what the registry holds now.
        graphWarmup_.cancel();
        const std::string dir = userDataDir();
        std::string list;
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(dir) / "ShaderCache", ec);
            list = (std::filesystem::path(dir) / "ShaderCache" / "warm_graphs.txt").string();
            std::ofstream f(list, std::ios::binary | std::ios::trunc);
            for (const auto& [id, key] : reg.keys()) f << id << ' ' << key << '\n';
            if (!f) list.clear();
        }
        if (list.empty()) {
            if (graphWarmHeld_) release("the graph list could not be written");
            return;
        }
        graphWarmup_.start(projSess_.project_.manifestPath, list);
        if (graphWarmup_.finished()) {   // could not start: compile here, as before
            if (graphWarmHeld_) release("the background compile could not start");
            return;
        }
        graphWarmRev_ = rev;
        graphWarmHeld_ = true;
        graphWarmT0_ = std::chrono::steady_clock::now();
        voxiRenderer_.holdGraphRebuild(true);
        AVER_INFO("[ShaderWarm] {} material graph(s) changed: the scene waits for a background compile instead of compiling "
                  "on the render thread", reg.count());
        return;
    }

    unsigned long long fp = 0;
    if (graphWarmup_.graphFingerprint(fp) && fp != reg.fingerprint()) {
        graphWarmup_.cancel();
        release("the background run composed different shader text (a graph not saved?)");
    } else if (graphWarmup_.finished()) {
        release("the background compile ended");
    } else if (std::chrono::steady_clock::now() - graphWarmT0_ > std::chrono::minutes(10)) {
        graphWarmup_.cancel();
        release("the background compile ran past 10 minutes");
    }
#else
    (void)e;
#endif
}

}  // namespace aver
