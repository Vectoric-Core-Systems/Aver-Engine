// Editor: World Settings > Level Streaming > Build HLOD (WAVE2_UE W2 step 5, editor half). Saves the level, then bakes the per-cell proxies
// (fmt::bakeHlod, the same code as `AverAssetC hlod`) on a background thread so a big level does not freeze the editor. Only cells whose content
// changed are rebuilt. The bake writes the proxies and the .ocstream HLOD records; the level's own STREAM `hlod=` distance is applied here when it
// finishes, and the level is left for the user to save. Nothing here draws a proxy: that is the renderer side of W2 step 6.

#include "SandboxApp.hpp"
#include "HlodBuildJob.hpp"

#if AVER_MODULE_SCENE
#  include "aver/formats/OcHlodBake.hpp"
#  include "aver/formats/OcStream.hpp"
#  if AVER_MODULE_TRIFACTOR
#    include "aver/trifactor/ClusterBuilder.hpp"
#  endif

#  include "aver/rhi/PrimitiveMeshes.hpp"

#  include <filesystem>
#  include <memory>

namespace aver {
namespace {

// The engine's generated primitives (GameContent::registerBuiltins) as mesh data. They are names, not files, so the bake's own .ocmesh loader
// skipped them -- and every new project's floor and walls are the built-in cube. Unit size, as the scene draws them; placement scale applies.
bool builtinMeshData(const std::string& asset, fmt::OcMeshData& out) {
    std::vector<rhi::MeshVertex> v;
    std::vector<u32> idx;
    if (asset == "Meshes/cube.ocmesh") appendBox(v, idx, 0, 0, 0, 1.0f);
    else if (asset == "Meshes/sphere.ocmesh") appendSphere(v, idx, 1.0f, 24, 48);
    else if (const rhi::primitives::BuiltinPrimitive* p = rhi::primitives::findBuiltinPrimitive(asset)) p->build(v, idx);
    else return false;
    if (v.empty() || idx.empty()) return false;
    out = fmt::OcMeshData{};
    out.positions.reserve(v.size() * 3);
    out.normals.reserve(v.size() * 3);
    out.uvs.reserve(v.size() * 2);
    for (const rhi::MeshVertex& m : v) {
        out.positions.insert(out.positions.end(), {m.px, m.py, m.pz});
        out.normals.insert(out.normals.end(), {m.nx, m.ny, m.nz});
        out.uvs.insert(out.uvs.end(), {m.u, m.v});
    }
    out.indices = std::move(idx);
    fmt::OcMeshSubmesh s;
    s.indexCount = static_cast<u32>(out.indices.size());
    s.vertexCount = static_cast<u32>(v.size());
    out.submeshes.push_back(s);
    out.materialSlots.push_back(std::string());   // the placement's material names it
    out.computeBounds();
    return true;
}

} // namespace

void SandboxApp::startHlodBuild() {
    const std::string blocked = editor::hlodBlockedReason(lvl_.levelHeader_.stream, projSess_.project_.valid() && !projSess_.levelPath_.empty());
    if (!blocked.empty()) { lvl_.hlodReport_ = blocked; return; }
    if (!saveLevel(projSess_.levelPath_)) { lvl_.hlodReport_ = "The level could not be saved."; return; }
    markLevelSaved();

    auto w = std::make_shared<fmt::OcWorldData>();
    std::string why;
    if (!fmt::loadOcworld(projSess_.levelPath_, *w, &why)) { lvl_.hlodReport_ = "Failed: " + why; return; }
    if (!w->stream.enabled) { lvl_.hlodReport_ = "Failed: the saved level does not stream."; return; }
    {   // the stream data places each placement in its cell
        fmt::OcStreamData sd;
        if (!w->stream.dataPath.empty() && fmt::loadOcStream((std::filesystem::path(projSess_.project_.dir) / w->stream.dataPath).string(), sd))
            fmt::applyOcStreamBounds(*w, sd);
    }
    const std::string stem = std::filesystem::path(projSess_.levelPath_).stem().string();
    const std::string contentDir = projSess_.project_.contentDir(), projectDir = projSess_.project_.dir;
    const bool force = lvl_.hlodForce_;
    lvl_.hlodReport_ = "Building...";
    hlodJob_.start([w, stem, contentDir, projectDir, force](const std::atomic<bool>& cancel, std::atomic<u32>& done, std::atomic<u32>& total) {
        editor::HlodJobResult res;
        fmt::OcHlodOptions opt;
        opt.force = force;
        opt.meshLoader = [contentDir](const std::string& asset, fmt::OcMeshData& out) {
            return fmt::loadOcMesh((std::filesystem::path(contentDir) / asset).string(), out) || builtinMeshData(asset, out);
        };
        opt.cancel = [&cancel]() { return cancel.load(); };
        opt.progress = [&done, &total](u32 d, u32 t) { done = d; total = t; };
#  if AVER_MODULE_TRIFACTOR
        opt.simplify = [](fmt::OcMeshData& m, u32 budget) {
            u32 got = 0;
            std::string sWhy;
            trifactor::simplifyToBudget(m, budget, &got, &sWhy);
            return got > 0;
        };
#  endif
        res.ok = fmt::bakeHlod(*w, stem, contentDir, projectDir, opt, res.report, &res.why);
        res.hlodCm = w->stream.hlodCm;
        return res;
    });
}

// Once per frame: takes a finished bake's result.
void SandboxApp::pollHlodBuild() {
    editor::HlodJobResult res;
    if (!hlodJob_.poll(res)) return;
    if (!res.ok) { lvl_.hlodReport_ = "Failed: " + res.why; return; }
    if (lvl_.levelHeader_.stream.hlodCm <= 0.0f && res.hlodCm > 0.0f) {
        lvl_.levelHeader_.stream.hlodCm = res.hlodCm;
        markLevelUnsaved();
    }
    lvl_.hlodReport_ = editor::hlodSummary(res.report);
    for (const std::string& m : res.report.warnings) AVER_WARN("[HLOD] {}", m);
    AVER_INFO("[HLOD] {}: {}", projSess_.levelPath_, lvl_.hlodReport_);
}

// Drawn inside World Settings > Level Streaming, under Cooked chunks.
void SandboxApp::buildHlodSettings() {
    ImGui::Separator();
    ImGui::TextDisabled("HLOD");
    fmt::OcStreamSettings& st = lvl_.levelHeader_.stream;
    f32 hlodM = st.hlodCm / 100.0f;
    ImGui::SetNextItemWidth(200.0f * viewState_.dpi_);
    if (ImGui::SliderFloat("HLOD distance (m)", &hlodM, 0.0f, 5000.0f, hlodM > 0.0f ? "%.0f" : "off")) {
        st.hlodCm = hlodM * 100.0f;
        markLevelUnsaved();
    }
    uiReg_.track("worldSettings.hlodDistance");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cells past the load distance, out to this one, show a merged low-poly proxy.\n"
                          "0 = no HLOD ring. When it is 0, Build HLOD uses three load distances.");
    const bool running = hlodJob_.running();
    const std::string blocked = editor::hlodBlockedReason(st, projSess_.project_.valid() && !projSess_.levelPath_.empty());
    ImGui::BeginDisabled(running || !blocked.empty());
    if (ImGui::Button("Build HLOD")) startHlodBuild();
    ImGui::EndDisabled();
    uiReg_.track("worldSettings.buildHlod");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(blocked.empty()
            ? "Saves the level, then merges and simplifies each streaming cell's static objects into one proxy mesh\n"
              "under Binaries/Streaming/<level>/hlod. Only cells whose content changed are rebuilt.\n"
              "Objects that move, are hidden, or are in a layer the game can switch are left out."
            : blocked.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Rebuild all", &lvl_.hlodForce_);
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) hlodJob_.cancel();
        ImGui::ProgressBar(hlodJob_.fraction(), ImVec2(-FLT_MIN, 0.0f), (std::to_string(hlodJob_.done()) + " / " + std::to_string(hlodJob_.total()) + " cells").c_str());
    }
    if (!lvl_.hlodReport_.empty()) ImGui::TextWrapped("%s", lvl_.hlodReport_.c_str());
}

}  // namespace aver

#endif
