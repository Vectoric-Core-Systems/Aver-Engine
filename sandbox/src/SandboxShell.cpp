// Editor UI: menu bar/toolbar, prompts, log/console, drawer/notifications, reference/profiler/nav panels.

#include <cstdlib>
#include "SandboxApp.hpp"
#include "aver/core/CpuTiming.hpp"

namespace aver {
// Restarts profiler stats when Play starts/stops (called from startPlay/stopPlay in SandboxPlay.cpp).
void SandboxApp::resetPlayProfile(bool playStarting) {
    if (prefsDevice_) prefsDevice_->resetGpuTiming();
    if (playStarting) playProf_.resetPhases();
}

// Guarded on AVER_WITH_IMGUI (declarations in SandboxApp.hpp).
#if AVER_WITH_IMGUI
// ---- References panel ----
// Finds files that name the asset. On-demand scan; text formats only (misses runtime references).
void SandboxApp::buildReferencesPanel() {
    if (!showReferences_) return;
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 320.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("References", &showReferences_)) { ImGui::End(); return; }

    if (!refPanelScanned_) {
        ImGui::TextWrapped("Right-click an asset in the Content Browser and choose "
                           "\"Find References\" to see what names it.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", cbRelativeToContent(refPanelAsset_).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) refPanelResults_ = cbFindReferencesTo(refPanelAsset_);
    uiReg_.track("references.rescan");
    ImGui::Separator();

    if (refPanelResults_.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Nothing found that names this asset.");
    } else {
        ImGui::Text("%zu file(s) name it:", refPanelResults_.size());
        ImGui::Separator();
        for (const std::string& rel : refPanelResults_) {
            // Double-click opens the file.
            if (ImGui::Selectable(rel.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const std::string abs = project_.contentDir() + "\\" + rel;
                cbSelectedFile_ = abs;
                assetEditors_.open(abs);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Scanned .ocworld/.ocmap/.ocmat/.ocgraph/.ocproject for this path.");
    ImGui::TextDisabled("Cannot see: a path built in C# at runtime, or a reference stored only as");
    ImGui::TextDisabled("a hashed id. This is \"found N\", not \"there are exactly N\".");
    ImGui::End();
}

// ---- Neural Visualiser (Window > Neural Visualiser) ----
// NeuraFI: frame interpolation overlay. NeuRaC: radiance cache (painted via rcDebugColour).
void SandboxApp::buildNeuralVisualiserPanel(Engine& e) {
    if (!showNeuralViz_) return;
    ImGui::SetNextWindowSize(ImVec2(470.0f * dpi_, 430.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Neural Visualiser", &showNeuralViz_)) { ImGui::End(); return; }
    rhi::IDevice* dev = e.device();

    // ---- NeuraFI ----
    ImGui::SeparatorText("NeuraFI (frame interpolation)");
    const bool fiOn = dev && dev->frameInterpolation();
    if (!fiOn) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f),
                           "Frame interpolation is off. Turn it on in Editor Preferences > Display\n"
                           "(\"Frame interpolation in viewport while editing\"); Play follows the project.");
    } else if (!dev->frameInterpolated()) {
        ImGui::TextDisabled("Frame interpolation is paused this frame (see the log for why).");
    }
    static const char* kFiViews[] = {"Off", "Sources (which real frame)", "Confidence",
                                     "Path bend (curved vs straight)", "Network share of the bend"};
    int fv = neurafiVizMode_ < 0 ? 0 : (neurafiVizMode_ > 4 ? 4 : neurafiVizMode_);
    if (ImGui::Combo("View##neurafi", &fv, kFiViews, 5)) neurafiVizMode_ = fv;
    uiReg_.track("neuralViz.neurafiView");
    switch (fv) {
        case 1: ImGui::TextDisabled("Orange: from the newer real frame. Blue: from the older one.\n"
                                    "Dim: low confidence. Magenta: a hole neither frame could fill\n"
                                    "(filled from its neighbours)."); break;
        case 2: ImGui::TextDisabled("How sure the blend is: blue (unsure) to red (sure). Magenta: a hole."); break;
        case 3: ImGui::TextDisabled("How far the curved path moves the in-between point off a straight\n"
                                    "line, blue (none) to red (full scale). Needs Quadratic or Learned."); break;
        case 4: ImGui::TextDisabled("How much of that bend the network added, blue (none) to red (full\n"
                                    "scale). All blue while the quadratic stands in."); break;
        default: break;
    }
    ImGui::BeginDisabled(fv == 0);
    ImGui::SliderFloat("Opacity##neurafi", &neurafiVizOpacity_, 0.05f, 1.0f, "%.2f");
    uiReg_.track("neuralViz.neurafiOpacity");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(fv != 3 && fv != 4);
    ImGui::SliderFloat("Full scale (px)##neurafi", &neurafiVizScalePx_, 0.05f, 4.0f, "%.2f px",
                       ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The bend, in pixels at the in-between frame, shown at full red.");
    ImGui::EndDisabled();
    ImGui::Checkbox("Show interpolated frames only", &neurafiShowGeneratedOnly_);
    uiReg_.track("neuralViz.generatedOnly");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Both images of every frame show the GENERATED one, so its errors can be\n"
                          "seen on their own (the real frames are hidden).");
    if (frameInterpolator_) {
        const neurafi::NeuraFI::TrainingStatus st = frameInterpolator_->trainingStatus();
        ImGui::TextDisabled("Path: %s", st.networkInUse ? "learned (network in use)"
                                        : frameInterpolator_->trajectory() == neurafi::Trajectory::Linear
                                            ? "straight lines" : "quadratic");
        if (st.evaluated)
            ImGui::TextDisabled("Error (smoothed): straight %.3f px, quadratic %.3f px, learned %.3f px",
                                st.errLinear, st.errAnalytic, st.errNetwork);
    }

    // ---- NeuRaC ----
    ImGui::SeparatorText("NeuRaC (radiance cache)");
#if AVER_MODULE_VOXI
    if (!voxiRenderer_.neuracLive()) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f),
                           "The cache is not running. It needs ray-driven GI (staged, D3D12) with\n"
                           "Project Settings > Rendering > ReSTIR visibility rays: Cached (NeuRaC).");
    }
    static const char* kRcViews[] = {"Off", "Cached light", "Coverage", "Cascade", "Cell state"};
    int rv = neuracViewMode_ < 0 ? 0 : (neuracViewMode_ > 4 ? 4 : neuracViewMode_);
    if (ImGui::Combo("View##neurac", &rv, kRcViews, 5)) neuracViewMode_ = rv;
    uiReg_.track("neuralViz.neuracView");
    switch (rv) {
        case 1: ImGui::TextDisabled("The light the cache holds at each visible surface, on its own.\n"
                                    "Black: nothing cached there yet."); break;
        case 2: ImGui::TextDisabled("Green: the cache supplies the light. Red: the fallback does."); break;
        case 3: ImGui::TextDisabled("Finest cascade holding the point: cyan 25 cm cells, yellow 100 cm,\n"
                                    "orange 400 cm. Dark red: outside all three."); break;
        case 4: ImGui::TextDisabled("Green: how many frames the cell has averaged. Red: how long since it\n"
                                    "was last updated. Violet: empty or stale."); break;
        default: break;
    }
    ImGui::BeginDisabled(rv == 0);
    ImGui::Checkbox("Cell grid", &neuracViewGrid_);
    uiReg_.track("neuralViz.neuracGrid");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Replaces the lit image while on (not an overlay).");
#else
    ImGui::TextDisabled("This build has no Voxi renderer.");
#endif
    ImGui::End();
}

// ---- Profiler panel (Window > GPU Profiler) ----
// Frame time, CPU phases (Play), scene walk, Voxi accel-struct loop, and per-pass GPU table.
void SandboxApp::buildProfilerPanel(Engine& e) {
    if (!showProfiler_) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 540.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Profiler", &showProfiler_)) { ImGui::End(); return; }

#if AVER_MODULE_FRAMEWORK
    const bool playing = anyPlayActive();
#else
    const bool playing = false;
#endif

    // ---- header: smoothed frame time, worst recent frame, Reset ----
    const f64 emaMs = playProf_.frameEmaMs();
    const f64 worstMs = playProf_.worstFrameMs();
    ImGui::Text("Frame %.2f ms (%.0f fps)", emaMs, emaMs > 0.0 ? 1000.0 / emaMs : 0.0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smoothed over about half a second, so it does not flicker.\n"
                          "The last frame alone: %.2f ms.", playProf_.lastFrameMs());
    ImGui::SameLine();
    ImGui::Text("  worst of last %u: %.1f ms (%.0f fps)", playProf_.windowFrames(), worstMs,
                worstMs > 0.0 ? 1000.0 / worstMs : 0.0);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", playing ? "Play" : "editing");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        e.device()->resetGpuTiming();
        playProf_.resetPhases();
        playProf_.resetFrames();
    }
    uiReg_.track("profiler.reset");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restart the GPU average, the CPU phase table and the frame-time window.\n"
                          "Play start and Play stop already restart the GPU average.");
    ImGui::Separator();

    // ---- CPU phases: what Play adds to onUpdate ----
    if (ImGui::CollapsingHeader("CPU phases (Play)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (playProf_.playFrames() == 0) {
            ImGui::TextDisabled("Nothing yet: these are timed only while a Play session runs.");
        } else {
            ImGui::TextDisabled("%s, %llu frames", playing ? "this Play session" : "last Play session",
                                static_cast<unsigned long long>(playProf_.playFrames()));
            if (ImGui::BeginTable("##cpuphases", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("avg ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
                ImGui::TableSetupColumn("worst ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
                ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, 60.0f * dpi_);
                ImGui::TableHeadersRow();
                f64 timedMs = 0.0;
                for (usize i = 0; i < editor::kPlayPhaseCount; ++i) {
                    const editor::PlayPhase ph = static_cast<editor::PlayPhase>(i);
                    const f64 ms = playProf_.phaseEmaMs(ph);
                    const f64 frac = emaMs > 0.0 ? ms / emaMs : 0.0;
                    timedMs += ms;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(editor::kPlayPhaseLabel[i]);
                    ImGui::TableSetColumnIndex(1);
                    if (frac > 0.20)      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%.2f", ms);
                    else if (frac > 0.08) ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.40f, 1.0f), "%.2f", ms);
                    else                  ImGui::Text("%.2f", ms);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", playProf_.phaseWorstMs(ph));
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", frac * 100.0);
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Timed total");
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%.2f", timedMs);
                ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%.1f%%", emaMs > 0.0 ? timedMs / emaMs * 100.0 : 0.0);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Physics steps per frame: avg %.2f, worst %u (catch-up grows as frames slow)",
                                playProf_.stepsEma(), playProf_.stepsWorst());
            ImGui::TextDisabled("Average over about half a second; worst over the last %u Play frames. "
                                "Drawing, ray tracing and the scene walk are below, not in this table.",
                                editor::PlayProfile::kWindow);
        }
    }

    // ---- scene walk (CPU) and Voxi's acceleration-structure loop ----
    if (ImGui::CollapsingHeader("Scene walk and Voxi (CPU)", ImGuiTreeNodeFlags_DefaultOpen)) {
        const CpuTimingReport w = collectCpuTiming();
        if (!w.supported) {
            ImGui::TextDisabled("CPU span timing is not available in this build.");
        } else if (w.nodes.empty() || w.framesAccumulated == 0) {
            ImGui::TextDisabled("No scene-walk window has completed yet (timing runs only while this panel is open).");
        } else {
            const f64 perWalk = 1.0 / static_cast<f64>(w.framesAccumulated);
            const usize walkIdx = static_cast<usize>(CpuSpan::SceneWalk);
            const usize overheadIdx = static_cast<usize>(CpuSpan::TimingOverhead);
            if (ImGui::BeginTable("##cpuwalk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Span", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ms / walk", ImGuiTableColumnFlags_WidthFixed, 80.0f * dpi_);
                ImGui::TableSetupColumn("calls / walk", ImGuiTableColumnFlags_WidthFixed, 90.0f * dpi_);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < w.nodes.size(); ++i) {
                    if (i == overheadIdx) continue;   // footnote below: see CpuTimingFormat.hpp
                    const CpuTimingNode& n = w.nodes[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (i != walkIdx) ImGui::Indent(14.0f * dpi_);
                    ImGui::TextUnformatted(n.label);
                    if (i != walkIdx) ImGui::Unindent(14.0f * dpi_);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", n.ms * perWalk);
                    ImGui::TableSetColumnIndex(2);
                    if (i == walkIdx) ImGui::TextDisabled("-");
                    else              ImGui::Text("%.0f", static_cast<f64>(n.calls) * perWalk);
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Of which about %.3f ms is this timer itself. Averaged over %u walks.",
                                w.nodes[overheadIdx].ms * perWalk, w.framesAccumulated);
        }
#if AVER_MODULE_VOXI
        ImGui::Text("Voxi acceleration-structure draw loop: %.2f ms CPU", voxiRenderer_.lastAccelBuildCpuMs());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The per-draw loop of the LAST FULL acceleration-structure build, not a\n"
                              "per-frame number: a frame whose gate skips or refits leaves it as it was.\n"
                              "Reads 0 until a build has reached the loop.");
#endif
    }

    // ---- GPU passes ----
    if (!ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::End(); return; }
    const rhi::GpuTimingReport r = e.device()->gpuTiming();
    if (!r.supported) {
        // Two different "no data" cases, reported separately rather than as one empty tree.
        ImGui::TextWrapped("This backend does not report GPU timings. D3D12 does; the Vulkan "
                           "backend has no timestamp machinery yet.");
        ImGui::End();
        return;
    }
    if (r.framesAccumulated == 0 || r.nodes.empty()) {
        ImGui::TextWrapped("No timings collected since the last reset yet. Run a few frames, or "
                           "pass --gpu-timing.");
        ImGui::End();
        return;
    }

    std::vector<f64> childSum(r.nodes.size(), 0.0);
    for (usize i = 0; i < r.nodes.size(); ++i) {
        const u32 p = r.nodes[i].parent;
        if (p != rhi::GpuTimingNode::kNoParent && p < childSum.size()) childSum[p] += r.nodes[i].ms;
    }
    f64 total = 0.0;
    for (usize i = 0; i < r.nodes.size(); ++i)
        if (r.nodes[i].parent == rhi::GpuTimingNode::kNoParent) total += r.nodes[i].ms;

    ImGui::Text("%.2f ms over %u frames", total, r.framesAccumulated);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An AVERAGE over the frames since the last reset (Play start, Play stop or the\n"
                          "Reset button), not one sampled frame -- see the device's own comment on why.\n"
                          "A number here lags a change by a few frames.");
    ImGui::SameLine();
    ImGui::TextDisabled("(marked passes only)");
    ImGui::Separator();

    if (ImGui::BeginTable("##passes", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, std::fmax(ImGui::GetContentRegionAvail().y, 160.0f * dpi_)))) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("incl ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
        ImGui::TableSetupColumn("excl ms", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi_);
        ImGui::TableSetupColumn("% GPU",   ImGuiTableColumnFlags_WidthFixed, 60.0f * dpi_);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (usize i = 0; i < r.nodes.size(); ++i) {
            const rhi::GpuTimingNode& n = r.nodes[i];
            int depth = 0;
            for (u32 p = n.parent; p != rhi::GpuTimingNode::kNoParent && depth < 8; ++depth) {
                if (p >= r.nodes.size()) break;
                p = r.nodes[p].parent;
            }
            const f64 excl = n.ms - childSum[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(depth * 14.0f * dpi_);
            ImGui::TextUnformatted(n.label.c_str());
            ImGui::Unindent(depth * 14.0f * dpi_);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", n.ms);
            ImGui::TableSetColumnIndex(2);
            const f64 frac = total > 0.0 ? (excl / total) : 0.0;
            if (frac > 0.20)      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%.2f", excl);
            else if (frac > 0.08) ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.40f, 1.0f), "%.2f", excl);
            else                  ImGui::Text("%.2f", excl);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", frac * 100.0);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
#endif  // AVER_WITH_IMGUI -- the two panels above

#if AVER_MODULE_SYNAPSE
// Bakes navmesh, writes the .ocnav, rebuilds overlay. Guarded on AVER_MODULE_SYNAPSE and AVER_MODULE_SCENE.
#if AVER_MODULE_SCENE
bool SandboxApp::bakeNavigationNow(Engine& e, std::string* why) {
    editor::NavBakeSettings s;
    s.cellSizeCm = navBakeCell_;
    std::string reason;
    synapse::BakeStats st;
    if (!editor::bakeNavigation(scene::World::instance(), s, nav_, &st, &reason)) {
        AVER_WARN("[Editor] navigation bake failed: {}", reason);
#if AVER_WITH_IMGUI
        notifyOutcome(editor::NotifySeverity::Error, "Navigation bake failed", reason, true);
#endif
        if (why) *why = reason;
        nav_ = fmt::OcNavData{};
        rebuildNavOverlay(e);
        return false;
    }
    rebuildNavOverlay(e);
    showNav_ = true;   // baking something invisible is how a bake gets run twice

    // Unsaved level has no path, so no .ocnav filename to write -- stays session-only, said so.
    if (levelPath_.empty()) {
        AVER_WARN("[Editor] navigation baked but NOT saved -- this level has no path yet; "
                  "save the level and bake again to write its .ocnav");
#if AVER_WITH_IMGUI
        notifyOutcome(editor::NotifySeverity::Warning, "Navigation baked, but not saved",
                     "This level has no path yet. Save it and bake again to write its .ocnav.");
#endif
        return true;
    }
    const std::string path = editor::navPathForLevel(levelPath_);
    std::string wwhy;
    if (!fmt::saveOcNav(path, nav_, &wwhy)) {
        AVER_WARN("[Editor] navigation bake could not be written to {}: {}", path, wwhy);
        if (why) *why = wwhy;
#if AVER_WITH_IMGUI
        notifyOutcome(editor::NotifySeverity::Error, "Could not write the navmesh", wwhy, true);
#endif
        return false;
    }
    AVER_INFO("[Editor] navigation written to {}", path);
#if AVER_WITH_IMGUI
    notifyOutcome(editor::NotifySeverity::Success, "Navigation baked",
                 std::filesystem::path(path).filename().string());
#endif
    return true;
}
#endif  // AVER_MODULE_SCENE -- no world to sample without it; see this function's own comment

#endif

// ---- Revision control: editor UI and timing ----
// Logic in RevisionControl.cpp; this file owns ImGui and worker scheduling. Read-only git commands only.
#if AVER_WITH_IMGUI
namespace {

// Text-diffable asset formats.
bool isDiffableAsset(std::string_view path) {
    const usize dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(path.substr(dot));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ocworld" || ext == ".ocmat" || ext == ".ocgraph" || ext == ".cs";
}

// Normalize separators and case for path comparison (editor vs git).
std::string flattenedPath(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

// Per-file history depth.
constexpr int kRcLogCount = 50;

// Folder status rank (for summarizing child statuses).
int folderRank(editor::FileStatus s) {
    switch (s) {
        case editor::FileStatus::Conflicted: return 3;
        case editor::FileStatus::Untracked:  return 1;
        case editor::FileStatus::Ignored:    return 0;
        default:                             return 2;   // any tracked change
    }
}

// Status bar: icon + branch + change count. Cheap operation over latched data (called twice/frame).
std::string rcStatusFace(bool busy, const editor::StatusBarSummary& summary) {
    std::string face = ICON_TREE " ";
    face += busy ? "Refreshing..." : summary.label;
    if (!busy && summary.changed > 0) face += " (" + std::to_string(summary.changed) + ")";
    return face;
}

} // namespace

// Reaps workers and refreshes state. Once a frame, from buildUI.
void SandboxApp::revisionControlTick() {
    if (rcProjectDir_ != project_.dir) {
        rcProjectDir_ = project_.dir;
        rcRoot_.clear();
        rcRootKey_.clear();
        rcStatus_ = editor::RepoStatus{};
        rcWhy_.clear();
        rcMarks_.clear();
        rcAnswered_ = false;
        rcGitPresent_ = false;
        rcRefreshedAt_ = -1.0;
        rcSelected_.clear();
        rcSelectedDiffable_ = false;
        rcLog_.clear();
        rcDiff_.clear();
        rcLogWhy_.clear();
        rcDiffWhy_.clear();
        // Jobs are dropped (detached workers cannot be cancelled).
        rcStatusJob_.reset();
        rcFileJob_.reset();
    }

    if (rcStatusJob_ && rcStatusJob_->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RcStatusQuery> job = rcStatusJob_;
        rcStatusJob_.reset();
        if (job->dir == project_.dir) {
            rcGitPresent_ = job->gitPresent;
            rcRoot_ = job->root;
            rcRootKey_ = flattenedPath(job->root);
            rcStatus_ = job->status;
            rcWhy_ = job->why;
            rcAnswered_ = true;
            rcRefreshedAt_ = ImGui::GetTime();

            rcMarks_.clear();
            rcMarks_.reserve(rcStatus_.files.size());
            for (const editor::FileEntry& f : rcStatus_.files) {
                if (f.ignored()) continue;   // not asked for, and not a change if it arrived anyway
                editor::FileStatus s = f.conflicted()  ? editor::FileStatus::Conflicted
                                     : f.unstaged != editor::FileStatus::Unmodified ? f.unstaged
                                                                                    : f.staged;
                if (s == editor::FileStatus::Unmodified) continue;
                rcMarks_.emplace_back(f.path, s);
            }
            std::sort(rcMarks_.begin(), rcMarks_.end(),
                      [](const std::pair<std::string, editor::FileStatus>& a,
                         const std::pair<std::string, editor::FileStatus>& b) {
                          return a.first < b.first;
                      });
        }
    }

    if (rcFileJob_ && rcFileJob_->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RcFileQuery> job = rcFileJob_;
        rcFileJob_.reset();
        // Latched only when it is still the row on screen: clicking a second file before the first
        // answer arrives must not put one file's history above another file's diff.
        if (job->root == rcRoot_ && job->path == rcSelected_) {
            rcLog_ = std::move(job->log);
            rcLogWhy_ = std::move(job->logWhy);
            rcDiff_ = std::move(job->diff);
            rcDiffWhy_ = std::move(job->diffWhy);
        }
    }

    // ASKED ONLY WHILE SOMETHING IS SHOWING THE ANSWER. A timer that runs whenever the editor is
    // open would spawn a git process every few seconds for the whole session to keep a panel nobody
    // has opened up to date.
    if (!showRevisionControl_ && drawer_ != Drawer::Content) return;
    // Two answers the timer must not retry (settled facts, not slow ones): git missing, or this
    // project not in a repository. Neither changes while the editor watches; Refresh re-asks because
    // the user knows when they've installed git or run `git init`.
    if (rcAnswered_ && (!rcGitPresent_ || rcRoot_.empty())) return;

    constexpr f64 kRcAutoRefreshSec = 4.0;
    const f64 now = ImGui::GetTime();
    if (rcRefreshedAt_ < 0.0 || now - rcRefreshedAt_ >= kRcAutoRefreshSec) revisionControlRefresh(false);
}

// Hands `git status` to a worker. Returns immediately, always.
void SandboxApp::revisionControlRefresh(bool force) {
    if (rcStatusJob_) return;            // one in flight answers everyone waiting
    if (!project_.valid() || project_.dir.empty()) return;
    if (!force && rcAnswered_ && !rcGitPresent_) return;

    auto job = std::make_shared<RcStatusQuery>();
    job->dir = project_.dir;
    const std::string knownRoot = (force || rcProjectDir_ != job->dir) ? std::string() : rcRoot_;
    rcProjectDir_ = job->dir;
    rcStatusJob_ = job;

    // Capture job by value; detached worker cannot be cancelled.
    std::thread([job, knownRoot] {
        job->gitPresent = editor::gitAvailable();
        if (!job->gitPresent) {
            job->why = "git was not found -- is it installed and on PATH?";
        } else {
            job->root = knownRoot.empty() ? editor::gitRepositoryRoot(job->dir, &job->why) : knownRoot;
            if (!job->root.empty()) job->status = editor::gitStatus(job->root, false, &job->why);
        }
        job->done.store(true, std::memory_order_release);
    }).detach();
}

// Hands one path's history and diff to a worker. Same rules as the refresh above.
void SandboxApp::revisionControlSelect(const std::string& repoRelativePath) {
    rcSelected_ = repoRelativePath;
    rcLog_.clear();
    rcDiff_.clear();
    rcLogWhy_.clear();
    rcDiffWhy_.clear();
    rcSelectedDiffable_ = isDiffableAsset(repoRelativePath);
    if (rcRoot_.empty() || repoRelativePath.empty()) return;
    auto job = std::make_shared<RcFileQuery>();
    job->root = rcRoot_;
    job->path = repoRelativePath;
    job->wantDiff = rcSelectedDiffable_;
    const editor::DiffSide side = rcDiffSide_;
    const bool haveCommits = !rcStatus_.initialCommit;
    rcFileJob_ = job;

    std::thread([job, side, haveCommits] {
        if (haveCommits) job->log = editor::gitLog(job->root, kRcLogCount, job->path, &job->logWhy);
        if (!job->wantDiff) { job->done.store(true, std::memory_order_release); return; }
        std::string text = editor::gitDiff(job->root, job->path, side, &job->diffWhy);
        usize start = 0;
        while (start <= text.size()) {
            usize nl = text.find('\n', start);
            if (nl == std::string::npos) {
                if (start < text.size()) job->diff.emplace_back(text.substr(start));
                break;
            }
            usize end = nl;
            if (end > start && text[end - 1] == '\r') --end;   // a git that writes CRLF
            job->diff.emplace_back(text.substr(start, end - start));
            start = nl + 1;
        }
        job->done.store(true, std::memory_order_release);
    }).detach();
}

// Editor path as git would name it; empty if not in repository.
std::string SandboxApp::rcKeyFor(const std::string& absolute) const {
    if (rcRootKey_.empty() || absolute.size() <= rcRootKey_.size()) return {};
    const std::string flat = flattenedPath(absolute);
    if (flat.size() <= rcRootKey_.size()) return {};
    if (flat.compare(0, rcRootKey_.size(), rcRootKey_) != 0) return {};
    if (flat[rcRootKey_.size()] != '/') return {};

    std::string key = absolute.substr(rcRootKey_.size() + 1);
    for (char& c : key) if (c == '\\') c = '/';
    return key;
}

// What to mark a Content Browser entry with.
bool SandboxApp::rcMarkFor(const std::string& absolute, bool isDir, editor::FileStatus& out) const {
    if (rcMarks_.empty()) return false;
    const std::string key = rcKeyFor(absolute);
    if (key.empty()) return false;

    const auto byPath = [](const std::pair<std::string, editor::FileStatus>& e,
                           const std::string& k) { return e.first < k; };
    if (!isDir) {
        const auto it = std::lower_bound(rcMarks_.begin(), rcMarks_.end(), key, byPath);
        if (it == rcMarks_.end() || it->first != key) return false;
        out = it->second;
        return true;
    }

    // Folder mark is a summary of child statuses; conflicts rank highest, then untracked.
    const std::string prefix = key + "/";
    int best = 0;
    for (auto it = std::lower_bound(rcMarks_.begin(), rcMarks_.end(), prefix, byPath);
         it != rcMarks_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        const int rank = folderRank(it->second);
        if (rank > best) best = rank;
        if (best == 3) break;   // a conflict is the loudest thing a folder can say; stop looking
    }
    if (best == 0) return false;
    out = best == 3 ? editor::FileStatus::Conflicted
        : best == 1 ? editor::FileStatus::Untracked
                    : editor::FileStatus::Modified;
    return true;
}

// Color for status badge (colour is never the only carrier; tooltips and text provided too).
ImU32 SandboxApp::rcStatusColour(editor::FileStatus s) {
    switch (s) {
        case editor::FileStatus::Modified:   return IM_COL32(230, 170,  60, 255);
        case editor::FileStatus::Added:      return IM_COL32( 98, 200, 110, 255);
        case editor::FileStatus::Deleted:    return IM_COL32(226,  92,  80, 255);
        case editor::FileStatus::Renamed:    return IM_COL32(120, 170, 240, 255);
        case editor::FileStatus::Untracked:  return IM_COL32(150, 156, 168, 255);
        case editor::FileStatus::Conflicted: return IM_COL32(240,  70, 150, 255);
        case editor::FileStatus::Ignored:    return IM_COL32(110, 114, 122, 255);
        case editor::FileStatus::Unmodified: break;
    }
    return IM_COL32(150, 156, 168, 255);
}

// Window > Revision Control: what git says about the open project, and nothing that can change it.
void SandboxApp::buildRevisionControlPanel() {
    if (!showRevisionControl_) return;
    ImGui::SetNextWindowSize(ImVec2(680.0f * dpi_, 480.0f * dpi_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Revision Control", &showRevisionControl_)) { ImGui::End(); return; }

    if (!project_.valid()) {
        ImGui::TextWrapped("Open a project first. This panel reports on the repository the "
                           "PROJECT lives in, which is not the same tree as the editor's own.");
        ImGui::End();
        return;
    }

    const bool busy = rcStatusJob_ != nullptr;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_REFRESH " Refresh")) revisionControlRefresh(true);
    ImGui::EndDisabled();
    uiReg_.track("revisionControl.refresh");
    ImGui::SameLine();
    if (busy) {
        ImGui::TextDisabled("asking git...");
    } else if (rcRefreshedAt_ >= 0.0) {
        ImGui::TextDisabled("as of %.0fs ago", ImGui::GetTime() - rcRefreshedAt_);
    } else {
        ImGui::TextDisabled("not asked yet");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("git runs on a worker thread, never in a frame.\n"
                          "A slow or missing git costs the editor nothing.");
    ImGui::Separator();

    if (!rcAnswered_) {
        ImGui::TextWrapped("Waiting for the first answer from git.");
        ImGui::End();
        return;
    }
    if (!rcGitPresent_) {
        ImGui::TextWrapped("git was not found. Install it, or put it on PATH, then press Refresh.");
        if (!rcWhy_.empty()) ImGui::TextDisabled("%s", rcWhy_.c_str());
        ImGui::End();
        return;
    }
    if (rcRoot_.empty()) {
        ImGui::TextWrapped("'%s' is not inside a git repository.", project_.name.c_str());
        ImGui::TextDisabled("That is a normal way to use the editor. Run `git init` in the project "
                            "folder and press Refresh if you want history for it.");
        // A REAL FAILURE, when there is one. gitRepositoryRoot leaves this empty for "not a
        // repository" precisely so the two cannot be shown as the same thing.
        if (!rcWhy_.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcWhy_.c_str());
        ImGui::End();
        return;
    }

    // ---- the branch line ----
    if (rcStatus_.detached) {
        // NOT GIVEN AN INVENTED NAME. git says `(detached)` where the branch goes, and a UI that
        // fills that in tells somebody they are on a branch they are not on.
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), ICON_WARNING " detached HEAD");
        if (!rcStatus_.headOid.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("at %.10s", rcStatus_.headOid.c_str());
        }
    } else {
        ImGui::Text("%s", rcStatus_.branch.empty() ? "(no branch)" : rcStatus_.branch.c_str());
    }
    if (rcStatus_.initialCommit) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no commits yet)");
    }
    if (rcStatus_.hasUpstream) {
        ImGui::SameLine();
        if (rcStatus_.ahead == 0 && rcStatus_.behind == 0)
            ImGui::TextDisabled("= %s", rcStatus_.upstream.c_str());
        else
            ImGui::TextDisabled("%d ahead, %d behind %s", rcStatus_.ahead, rcStatus_.behind,
                                rcStatus_.upstream.c_str());
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(tracks nothing)");
    }
    ImGui::TextDisabled("%s", rcRoot_.c_str());
    if (rcStatus_.hasConflicts())
        ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.59f, 1.0f),
                           ICON_WARNING " This working tree has unresolved conflicts.");
    ImGui::Separator();

    const f32 listH = ImGui::GetContentRegionAvail().y * 0.42f;
    if (rcStatus_.clean()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Nothing changed. The working tree is clean.");
    } else if (ImGui::BeginTable("##rcFiles", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                                 ImVec2(0.0f, listH))) {
        ImGui::TableSetupColumn("XY",     ImGuiTableColumnFlags_WidthFixed, 34.0f * dpi_);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 96.0f * dpi_);
        ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const editor::FileEntry& f : rcStatus_.files) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // Git's two-letter status codes (necessary for conflicts: "both modified" vs "deleted by us").
            const char xy[3] = {f.x, f.y, '\0'};
            ImGui::TextUnformatted(xy);

            ImGui::TableSetColumnIndex(1);
            const editor::FileStatus shown = f.conflicted() ? editor::FileStatus::Conflicted
                                           : f.unstaged != editor::FileStatus::Unmodified ? f.unstaged
                                                                                          : f.staged;
            ImGui::PushStyleColor(ImGuiCol_Text, rcStatusColour(shown));
            ImGui::TextUnformatted(editor::statusName(shown));
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(f.path.c_str());
            if (ImGui::Selectable(f.path.c_str(), rcSelected_ == f.path,
                                  ImGuiSelectableFlags_SpanAllColumns))
                revisionControlSelect(f.path);
            ImGui::PopID();
            // Original path for renames/copies (v2 format).
            if (!f.oldPath.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(f.copied ? "(copied from %s, %d%%)" : "(was %s, %d%%)",
                                    f.oldPath.c_str(), static_cast<int>(f.similarity));
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (rcSelected_.empty()) {
        ImGui::TextDisabled("Pick a file above for its history and its diff.");
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(rcSelected_.c_str());

    if (ImGui::BeginTabBar("##rcDetail")) {
        if (ImGui::BeginTabItem("History")) {
            if (rcStatus_.initialCommit) {
                ImGui::TextDisabled("This repository has no commits yet, so nothing has a history.");
            } else if (rcFileJob_) {
                ImGui::TextDisabled("asking git...");
            } else if (!rcLogWhy_.empty()) {
                ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcLogWhy_.c_str());
            } else if (rcLog_.empty()) {
                // Ordinary answer for untracked files (not a failure).
                ImGui::TextDisabled("No commits touch this path yet.");
            } else if (ImGui::BeginTable("##rcLog", 3,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, 78.0f * dpi_);
                ImGui::TableSetupColumn("When",   ImGuiTableColumnFlags_WidthFixed, 96.0f * dpi_);
                ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const editor::LogEntry& c : rcLog_) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(c.shortOid.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.10s", c.date.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(c.subject.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s <%s>\n%s", c.oid.c_str(), c.author.c_str(),
                                          c.email.c_str(), c.date.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Diff")) {
            if (!rcSelectedDiffable_) {
                ImGui::TextWrapped("No diff for this file. The viewer covers the text formats "
                                   "(.ocworld, .ocmat, .ocgraph, .cs); everything else a project "
                                   "holds is a binary container or an image, and a unified diff of "
                                   "those bytes would say nothing.");
                ImGui::TextDisabled("Its status and its history above still apply.");
            } else {
                // Both sides offered (staged and worktree can differ); make both calls to avoid short-circuit.
                int side = rcDiffSide_ == editor::DiffSide::Worktree ? 0 : 1;
                bool sideChanged = ImGui::RadioButton("Worktree", &side, 0);
                ImGui::SameLine();
                sideChanged = ImGui::RadioButton("Staged", &side, 1) || sideChanged;
                if (sideChanged) {
                    rcDiffSide_ = side == 0 ? editor::DiffSide::Worktree : editor::DiffSide::Index;
                    revisionControlSelect(rcSelected_);
                }
                ImGui::SameLine();
                ImGui::TextDisabled(side == 0 ? "(the index vs the file on disk)"
                                              : "(HEAD vs the index)");
                if (rcFileJob_) {
                    ImGui::TextDisabled("asking git...");
                } else if (!rcDiffWhy_.empty()) {
                    ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", rcDiffWhy_.c_str());
                } else if (rcDiff_.empty()) {
                    ImGui::TextDisabled("Nothing differs on this side.");
                } else {
                    ImGui::BeginChild("##rcDiffText", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    // Clipped: avoid AddText calls for off-screen lines.
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(rcDiff_.size()));
                    while (clip.Step()) {
                        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                            const std::string& line = rcDiff_[static_cast<usize>(i)];
                            // First char is the diff line type; +++ and --- are headers, not add/remove.
                            ImVec4 col(0.86f, 0.87f, 0.89f, 1.0f);
                            if (line.starts_with("+++") || line.starts_with("---"))
                                col = ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
                            else if (line.starts_with("@@"))
                                col = ImVec4(0.47f, 0.67f, 0.94f, 1.0f);
                            else if (!line.empty() && line[0] == '+')
                                col = ImVec4(0.38f, 0.78f, 0.43f, 1.0f);
                            else if (!line.empty() && line[0] == '-')
                                col = ImVec4(0.89f, 0.36f, 0.31f, 1.0f);
                            else if (line.starts_with("diff ") || line.starts_with("index "))
                                col = ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
                            ImGui::TextColored(col, "%s", line.c_str());
                        }
                    }
                    clip.End();
                    ImGui::EndChild();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Status bar widgets ----
// Icon-and-word control, shared by revision control and MCP.
bool SandboxApp::statusBarWidget(const char* id, const char* face, const ImVec4& tint,
                                 const char* tooltip) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    const bool clicked = ImGui::SmallButton(face);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    uiReg_.track(id);
    return clicked;
}

// Status-bar widget color palette (RepoMood states defined in RevisionControl.hpp).
ImVec4 SandboxApp::rcMoodColour(editor::RepoMood m) {
    switch (m) {
        case editor::RepoMood::Clean:
            return ImVec4(0.6f, 0.85f, 0.6f, 1.0f);
        case editor::RepoMood::Dirty:
            return ImVec4(1.0f, 0.72f, 0.25f, 1.0f);
        case editor::RepoMood::Conflicted:
            return ImVec4(0.94f, 0.27f, 0.59f, 1.0f);
        case editor::RepoMood::Unknown:
        case editor::RepoMood::NoProject:
        case editor::RepoMood::NoGit:
        case editor::RepoMood::NotARepo:
            break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

// Status-bar twin of Window > Revision Control (same latched answer via summariseForStatusBar).
void SandboxApp::drawRevisionControlStatusWidget() {
    const bool busy = rcStatusJob_ != nullptr;
    const editor::StatusBarSummary summary = editor::summariseForStatusBar(
        project_.valid(), rcAnswered_, rcGitPresent_, rcRoot_, rcStatus_, rcWhy_);
    const std::string face = rcStatusFace(busy, summary);

    // Carries the same facts the panel would, plus one line this widget owns: what clicking it
    // does -- a tooltip that only repeats facts, never that a button is under the cursor, is one
    // somebody reads once and never acts on.
    std::string tooltip = summary.detail;
    if (busy) {
        if (!tooltip.empty()) tooltip += "\n";
        tooltip += "A refresh is already asking git for the current answer.";
    }
    if (!tooltip.empty()) tooltip += "\n";
    tooltip += "Click for Refresh and the Revision Control panel.";

    // Busy borrows the disabled colour rather than `summary.mood`: that mood describes an answer
    // already being replaced (see rcStatusFace).
    const ImVec4 tint = busy ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                             : rcMoodColour(summary.mood);
    const bool clicked =
        statusBarWidget("statusbar.revisionControl", face.c_str(), tint, tooltip.c_str());
    if (clicked) ImGui::OpenPopup("rcStatusMenu");

    if (ImGui::BeginPopup("rcStatusMenu")) {
        // Same toggle the window menu offers (see "window.revisionControl" above) -- this widget
        // exists so the panel isn't the only door.
        ImGui::MenuItem("Revision Control", nullptr, &showRevisionControl_);
        uiReg_.track("statusbar.revisionControl.togglePanel");

        // Disabled while busy, not hidden: a running query already answers what a second git
        // process would only repeat.
        ImGui::BeginDisabled(busy);
        if (ImGui::MenuItem(ICON_REFRESH " Refresh")) revisionControlRefresh(true);
        ImGui::EndDisabled();
        uiReg_.track("statusbar.revisionControl.refresh");
        if (busy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A refresh is already asking git for the answer.");

        ImGui::Separator();
        ImGui::BeginDisabled(true);
        ImGui::TextUnformatted(summary.detail.empty() ? "(nothing to report)" : summary.detail.c_str());
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}
#endif  // AVER_WITH_IMGUI -- the whole revision-control UI half

// Whether HUD preview may draw (tab has rect and no session playing).
bool SandboxApp::hudPreviewActive() const {
#if AVER_MODULE_SCRIPTING && AVER_MODULE_FRAMEWORK
    return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f &&
           aver_fw_play_state() != AVER_FW_PLAY_PLAYING;
#elif AVER_MODULE_SCRIPTING
    return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f;
#else
    return false;
#endif
}

// Called by the HUD tab each frame; cleared when not drawn.
void SandboxApp::setHudPreview(int index, f32 x, f32 y, f32 w, f32 h) {
    hudPreviewIndex_ = index; hudRectX_ = x; hudRectY_ = y; hudRectW_ = w; hudRectH_ = h;
}

#if AVER_WITH_IMGUI
// Draws a button with a drop-down triangle. Returns true when clicked.
bool SandboxApp::dropButton(const char* label) {
    const f32 extra = 16.0f*dpi_;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const bool clicked = ImGui::Button(label, ImVec2(ts.x + ImGui::GetStyle().FramePadding.x*2 + extra, 0));
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const f32 cx = mx.x - extra*0.5f - 2.0f*dpi_, cy = (mn.y+mx.y)*0.5f, s = 3.0f*dpi_;
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(cx-s,cy-s*0.55f), ImVec2(cx+s,cy-s*0.55f), ImVec2(cx,cy+s*0.8f), ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

#endif

// Records upgrade outcome as a notification.
void SandboxApp::setUpgradeStatus(std::string msg,
                      editor::NotifySeverity sev) {
#if AVER_WITH_IMGUI
    notifyOutcome(sev, std::move(msg), "", sev == editor::NotifySeverity::Error);
#else
    if (sev == editor::NotifySeverity::Error) AVER_ERROR("[Editor] {}", msg);
    else                                      AVER_INFO("[Editor] {}", msg);
#endif
}

// May the window close? Called from WM_CLOSE on the message thread.
bool SandboxApp::onCloseGuardThunk(void* user) {
    return static_cast<SandboxApp*>(user)->onCloseGuard();
}

bool SandboxApp::onCloseGuard() {
#if AVER_WITH_IMGUI
    if (exitPrompt_) return false;
    if (assetEditors_.anyDirty() || levelHasUnsavedEdits()) { exitPrompt_ = true; return false; }
#endif
    return true;
}

// Does the level differ from its file (uses per-edit serial, not undo stack depth)?
bool SandboxApp::levelHasUnsavedEdits() const { return currentEditMark() != savedEditMark_; }

// Mark document as saved (after save, load, or new).
void SandboxApp::markLevelSaved() { savedEditMark_ = currentEditMark(); }

// Force dirty state on (for content from outside levelPath_).
void SandboxApp::markLevelUnsaved() { savedEditMark_ = ~0ull; }

// Exit, unless something is unsaved (ask first).
void SandboxApp::requestExitChecked(Engine& e) {
#if AVER_WITH_IMGUI
    if (assetEditors_.anyDirty() || levelHasUnsavedEdits()) { exitPrompt_ = true; return; }
#endif
    e.requestExit();
}

// File > Save All, Ctrl+Shift+S (save level and all dirty asset tabs).
void SandboxApp::saveAll() {
#if AVER_WITH_IMGUI
    saveLevelInteractive();

    std::string why;
    const usize failed = assetEditors_.saveAllDirty(&why);
    if (failed != 0) {
        notifyOutcome(editor::NotifySeverity::Error,
                      failed == 1 ? "1 asset could not be saved"
                                  : std::to_string(failed) + " assets could not be saved",
                      why, true);
        AVER_ERROR("[Editor] Save All: {} editor(s) could not be saved: {}", failed, why);
    }
#endif
}

// The unsaved-changes modal. Names the files, because "you have unsaved changes" is not
// something a user can act on.
// Help > About. Deliberately short: build, renderer, and where prefs live -- the three things a
// bug report needs.
//
// Guarded inside the braces like every other draw*Prompt (declaration in SandboxApp.hpp is
// unguarded; buildUI calls this from inside AVER_WITH_IMGUI; a body of raw ImGui:: calls can't
// compile without imgui.h). drawPendingOpenPrompt, drawUpgradePrompt and drawExitPrompt already
// read this way; this one and drawSaveLevelAsPrompt below were the two that were missed, caught
// by a -DAVER_ENABLE_UI=OFF build.
void SandboxApp::drawAboutPrompt(Engine& e) {
#if AVER_WITH_IMGUI
    if (!showAbout_) return;
    constexpr const char* kTitle = "About Aver Engine";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (logoUiId_) {
        const f32 h = 56.0f * dpi_;
        ImGui::Image(static_cast<ImTextureID>(logoUiId_), ImVec2(h * logoAspect_, h));
        ImGui::SameLine(0.0f, 14.0f * dpi_);
    }
    ImGui::BeginGroup();
    if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
    ImGui::Text("%.*s", static_cast<int>(kEngineName.size()), kEngineName.data());
    if (fontMedium_) ImGui::PopFont();
    ImGui::TextDisabled("Version %.*s", static_cast<int>(kEngineVersion.size()), kEngineVersion.data());
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (rhi::IDevice* dev = e.device()) {
        ImGui::Text("Renderer   %s", rhi::backendName(dev->backend()));
        const char* adapter = dev->adapterName();
        if (adapter && *adapter) ImGui::Text("Adapter    %s", adapter);
    }
    ImGui::Text("Interface  %.0f%% scale", static_cast<double>(dpi_) * 100.0);
    ImGui::TextDisabled("Settings   %s", editor::editorPrefsPath().c_str());

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(110.0f * dpi_, 0.0f))) {
        showAbout_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

// Names a new file for the level and saves it there (prompted, not file dialog).
void SandboxApp::drawSaveLevelAsPrompt() {
#if AVER_WITH_IMGUI
    if (!wantSaveLevelAs_) return;
    constexpr const char* kTitle = "Save Level As";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const std::string maps = project_.valid()
        ? (std::filesystem::path(project_.contentDir()) / "Maps").string()
        : std::string();

    ImGui::TextUnformatted("Name");
    ImGui::PushItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool submit = ImGui::InputTextWithHint("##savelevelas", "Arena", saveLevelAsName_,
                                                 sizeof saveLevelAsName_,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    const std::string stem = saveLevelAsName_;
    const bool named = !stem.empty();
    const std::filesystem::path target = named && !maps.empty()
        ? std::filesystem::path(maps) / (stem + ".ocworld")
        : std::filesystem::path();
    if (!target.empty()) ImGui::TextDisabled("Saves to: %s", target.string().c_str());
    else                 ImGui::TextDisabled("Saves to: <project>\\Content\\Maps\\<Name>.ocworld");

    std::error_code ec;
    const bool exists = !target.empty() && std::filesystem::exists(target, ec);
    if (exists) ImGui::TextDisabled("A level of that name is already there and will be replaced.");
    if (!saveLevelAsError_.empty())
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", saveLevelAsError_.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    const bool valid = named && !maps.empty() && stem.find_first_of("\\/:*?\"<>|") == std::string::npos;
    ImGui::BeginDisabled(!valid);
    const bool go = ImGui::Button("Save", ImVec2(110.0f * dpi_, 0.0f));
    ImGui::EndDisabled();
    if (!valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(maps.empty() ? "Open or create a project first."
                                       : "Enter a name with no path separators.");
    if ((go || (submit && valid)) && valid) {
        std::filesystem::create_directories(maps, ec);
#if AVER_MODULE_SCENE
        // Copy gets a new ID; zero it so the writer recomputes from the new name.
        const bool renamed = stem != levelName_;
        const std::string prevName = levelName_;
        const u64 prevId = levelHeader_.contentId, prevLegacyId = legacyMapHeader_.contentId;
        if (renamed) {
            levelName_ = stem;
            levelHeader_.contentId = 0;
            legacyMapHeader_.contentId = 0;
        }
        if (saveLevel(target.string())) {
#if AVER_MODULE_PHYSICS
            // Lanes file travels with the level (copy reads from <name>.oclanes).
            if (!levelPath_.empty()) {
                const std::filesystem::path from = game::GameLevel::laneSidecarPath(levelPath_);
                const std::filesystem::path to = game::GameLevel::laneSidecarPath(target.string());
                std::error_code lec;
                if (from != to && std::filesystem::exists(from, lec)) {
                    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, lec);
                    if (lec)
                        AVER_WARN("[Level] could not copy {} beside the saved copy: {}", from.string(), lec.message());
                }
            }
#endif
            levelPath_ = target.string();
            cbInvalidate(maps);
            setUpgradeStatus("Saved " + target.filename().string());
            wantSaveLevelAs_ = false;
            ImGui::CloseCurrentPopup();
        } else {
            if (renamed) {
                levelName_ = prevName;
                levelHeader_.contentId = prevId;
                legacyMapHeader_.contentId = prevLegacyId;
            }
            saveLevelAsError_ = "Could not write " + target.filename().string() + ".";
        }
#else
        wantSaveLevelAs_ = false;
        ImGui::CloseCurrentPopup();
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        wantSaveLevelAs_ = false;
        saveLevelAsError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#endif
}

void SandboxApp::drawExitPrompt(Engine& e) {
#if AVER_WITH_IMGUI
    if (!exitPrompt_) return;
    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::vector<std::string> dirty = assetEditors_.dirtyTitles();
#if AVER_MODULE_SCENE
    // List the level first (most likely to be significant work).
    const bool levelDirty = levelHasUnsavedEdits();
    if (levelDirty)
        dirty.insert(dirty.begin(),
                     "Level: " + (levelName_.empty() ? std::string("untitled") : levelName_));
#endif
    ImGui::TextWrapped("%zu item%s ha%s unsaved changes:",
                       dirty.size(), dirty.size() == 1 ? "" : "s", dirty.size() == 1 ? "s" : "ve");
    ImGui::Spacing();
    for (const std::string& t : dirty) ImGui::BulletText("%s", t.c_str());
#if AVER_MODULE_SCENE
    if (levelDirty && levelPath_.empty())
        ImGui::TextDisabled("This level has never been saved -- \"Save all\" cannot name a file "
                            "for it. Cancel, then File > Save Level As.");
#endif
    if (!exitPromptError_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", exitPromptError_.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save all and exit", ImVec2(150.0f * dpi_, 0.0f))) {
        std::string why;
        usize failed = assetEditors_.saveAllDirty(&why);
#if AVER_MODULE_SCENE
        if (levelDirty && !levelPath_.empty() && !saveLevel(levelPath_)) {
            ++failed;
            why += (why.empty() ? "" : "; ") + std::string("could not save the level to ") + levelPath_;
        }
#endif
        if (failed == 0) {
            ImGui::CloseCurrentPopup();
            exitPrompt_ = false;
            e.requestExit();
        } else {
            // Stay open on failure (user asked to keep the work).
            exitPromptError_ = why;
            AVER_ERROR("[Editor] {} editor(s) could not be saved; the exit was cancelled: {}",
                       failed, why);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard and exit", ImVec2(150.0f * dpi_, 0.0f))) {
        AVER_WARN("[Editor] exiting with {} unsaved item(s); the changes are gone", dirty.size());
        ImGui::CloseCurrentPopup();
        exitPrompt_ = false;
        e.requestExit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        exitPrompt_ = false;
        exitPromptError_.clear();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

// Open level picker, rebuilding its list (shared by menu item and --open-level-picker).
void SandboxApp::openLevelPickerNow() {
    openLevelList_ = editor::listLevels(project_.contentDir());
    openLevelSelected_ = -1;
    openLevelError_.clear();
#if AVER_MODULE_SCENE
    // Pre-select the currently open level so list shows where you are.
    for (int i = 0; i < static_cast<int>(openLevelList_.size()); ++i) {
        std::error_code lec;
        if (!levelPath_.empty() &&
            std::filesystem::equivalent(openLevelList_[static_cast<usize>(i)].absPath,
                                        levelPath_, lec))
            openLevelSelected_ = i;
    }
#endif
    openLevelPicker_ = true;
}

// File > Open Level picker (project-local list, not OS file dialog).
void SandboxApp::drawOpenLevelPrompt(Engine& e) {
    (void)e;   // required by the caller's uniform signature (buildPanels/menu dispatch); unused here
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    if (armOpenLevelPicker_) { armOpenLevelPicker_ = false; openLevelPickerNow(); }
    if (!openLevelByName_.empty()) {
        const std::string want = openLevelByName_;
        openLevelByName_.clear();
        bool found = false;
        for (const editor::LevelEntry& l : editor::listLevels(project_.contentDir())) {
            if (l.relPath != want && l.name != want) continue;
            found = true;
            if (!requestOpenLevel(l.absPath, "opened by --open-level"))
                AVER_ERROR("[Level] --open-level {}: {}", want, openLevelError_);
            break;
        }
        if (!found) AVER_ERROR("[Level] --open-level {}: no level of that name in this project", want);
    }
    if (!openLevelPicker_) return;
    constexpr const char* kTitle = "Open Level";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (openLevelList_.empty()) {
        ImGui::TextDisabled("This project has no levels yet.");
        ImGui::TextDisabled("File > New Level, then Save Level As, writes one to Content\\Maps.");
    } else {
        ImGui::TextUnformatted("Levels in this project");
        ImGui::BeginChild("##levels", ImVec2(0.0f, 240.0f * dpi_), true);
        for (int i = 0; i < static_cast<int>(openLevelList_.size()); ++i) {
            const editor::LevelEntry& l = openLevelList_[static_cast<usize>(i)];
            // Current level is marked, not hidden (re-opening discards edits).
            const bool current = !levelPath_.empty() &&
                std::filesystem::path(l.absPath) == std::filesystem::path(levelPath_);
            ImGui::PushID(i);
            if (ImGui::Selectable(l.name.c_str(), openLevelSelected_ == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                openLevelSelected_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (requestOpenLevel(l.absPath, "opened from File > Open Level")) {
                        openLevelPicker_ = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::SameLine();
            if (current) ImGui::TextDisabled("%s  (open)", l.relPath.c_str());
            else         ImGui::TextDisabled("%s", l.relPath.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (!openLevelError_.empty())
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", openLevelError_.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    const bool picked = openLevelSelected_ >= 0 &&
                        openLevelSelected_ < static_cast<int>(openLevelList_.size());
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Open", ImVec2(110.0f * dpi_, 0.0f)) && picked) {
        const editor::LevelEntry& l = openLevelList_[static_cast<usize>(openLevelSelected_)];
        if (requestOpenLevel(l.absPath, "opened from File > Open Level")) {
            openLevelPicker_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        openLevelPicker_ = false;
        openLevelError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#endif
}

// The unsaved-changes modal for ANY pending open. Same Save/Discard/Cancel shape as
// drawExitPrompt, relabeled: this asks about the CURRENT level's undo history and opens a
// different level rather than exiting, so reusing exitPrompt_ risked a "Discard and exit"
// button that actually opened a file. One modal for three callers (picker, Content Browser,
// forwarded launch) -- the sentence below is built from pendingOpenWhy_ rather than hardcoding
// "forwarded", the only case that used to exist.
void SandboxApp::drawPendingOpenPrompt(Engine& e) {
#if AVER_WITH_IMGUI
    if (!pendingOpenPrompt_ && !pendingNewLevel_) return;
    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (pendingNewLevel_)
        ImGui::TextWrapped("The current level has unsaved changes. Starting a new level "
                           "discards them.");
    else
        ImGui::TextWrapped("'%s' is about to be %s. The current level has unsaved changes.",
                           std::filesystem::path(pendingOpenPath_).filename().string().c_str(),
                           pendingOpenWhy_.empty() ? "opened" : pendingOpenWhy_.c_str());
#if AVER_MODULE_SCENE
    // levelPath_ only exists under AVER_MODULE_SCENE; this modal only opens via requestOpenLevel or
    // New Level, both themselves SCENE-guarded, so nothing below can run without it either.
    if (levelPath_.empty())
        ImGui::TextDisabled("This level has never been saved, so there is no file to save it to. "
                            "Cancel, then File > Save Level As.");
#endif
    ImGui::Spacing();
    ImGui::Separator();

    // DISABLED WITH NO PATH TO SAVE TO. It used to treat "never saved" as success and open
    // straight through -- the one case where "Save and open" was a button that discarded.
#if AVER_MODULE_SCENE
    ImGui::BeginDisabled(levelPath_.empty());
    if (ImGui::Button(pendingNewLevel_ ? "Save and continue" : "Save and open",
                      ImVec2(160.0f * dpi_, 0.0f))) {
        if (saveLevel(levelPath_)) {
            const std::string toOpen = pendingOpenPath_;
            const bool wasNew = pendingNewLevel_;
            ImGui::CloseCurrentPopup();
            pendingOpenPrompt_ = false;
            pendingNewLevel_ = false;
            pendingOpenPath_.clear();
            if (wasNew) startNewLevel(e);
            else        openLevelDirect(e, toOpen);
        } else {
            // STAY OPEN on a failed save -- same reasoning as drawExitPrompt's identical guard:
            // proceeding anyway would discard exactly the work this prompt exists to protect.
            AVER_ERROR("[Sandbox] could not save '{}'; the forwarded open was cancelled", levelPath_);
        }
    }
    ImGui::EndDisabled();
#else
    // saveLevel/startNewLevel/openLevelDirect only exist under AVER_MODULE_SCENE, and per the
    // comment above this modal can't actually be open without it -- kept disabled, not removed, so
    // the row's three-button layout doesn't shift between configurations.
    ImGui::BeginDisabled(true);
    ImGui::Button(pendingNewLevel_ ? "Save and continue" : "Save and open", ImVec2(160.0f * dpi_, 0.0f));
    ImGui::EndDisabled();
#endif
    ImGui::SameLine();
    if (ImGui::Button(pendingNewLevel_ ? "Discard and continue" : "Discard and open",
                      ImVec2(170.0f * dpi_, 0.0f))) {
#if AVER_MODULE_SCENE
        const std::string toOpen = pendingOpenPath_;
        const bool wasNew = pendingNewLevel_;
#endif
        AVER_WARN("[Sandbox] the current level's unsaved changes are gone");
        ImGui::CloseCurrentPopup();
        pendingOpenPrompt_ = false;
        pendingNewLevel_ = false;
        pendingOpenPath_.clear();
#if AVER_MODULE_SCENE
        if (wasNew) startNewLevel(e);
        else        openLevelDirect(e, toOpen);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        pendingOpenPrompt_ = false;
        pendingNewLevel_ = false;
        pendingOpenPath_.clear();
    }
    ImGui::EndPopup();
#else
    (void)e;
#endif
}

// File > Launch in Aver Engine Runtime. Starts a SEPARATE process that reads the level from disk,
// the same inputs a packaged game gets -- not anything held only in the editor's memory -- so an
// unsaved edit would silently not be there. `skipDirtyCheck` is how drawLaunchRuntimePrompt's own
// buttons launch after already answering that question, without re-triggering the same prompt.
void SandboxApp::launchInRuntime(Engine& e, bool skipDirtyCheck) {
    (void)e;   // no engine access needed; kept for the same uniform signature other menu actions use
#if AVER_MODULE_SCENE
    if (!project_.valid() || levelPath_.empty()) return;
    // Anything the runtime would read from disk and find stale: the level, an asset tab, the terrain.
    bool anythingUnsaved = levelHasUnsavedEdits() || assetEditors_.anyDirty();
#if AVER_MODULE_LANDSCAPE
    anythingUnsaved = anythingUnsaved || landscape_.dirty();
#endif
    if (!skipDirtyCheck && anythingUnsaved) { launchRuntimePrompt_ = true; return; }

    const std::string levelFile = std::filesystem::path(levelPath_).filename().string();
    std::string why;
    if (editor::launchRuntime(project_.manifestPath, levelPath_, playStandaloneArgs_, &why)) {
        setUpgradeStatus("Launched " + levelFile + " in Aver Engine Runtime");
        AVER_INFO("[Editor] launched AverEngineRuntime.exe on {}", levelPath_);
    } else {
        setUpgradeStatus("Could not launch Aver Engine Runtime: " + why, editor::NotifySeverity::Error);
        AVER_ERROR("[Editor] could not launch Aver Engine Runtime: {}", why);
    }
#else
    (void)skipDirtyCheck;
#endif
}

// The unsaved-changes modal for File > Launch in Aver Engine Runtime. Same Save/Discard/Cancel
// shape as drawPendingOpenPrompt: the runtime is a second process reading the level from disk, so
// proceeding with unsaved edits silently ships a stale level, same as opening over them would.
void SandboxApp::drawLaunchRuntimePrompt(Engine& e) {
    (void)e;
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    if (!launchRuntimePrompt_) return;
    constexpr const char* kTitle = "Launch in Aver Engine Runtime";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const bool levelDirty = levelHasUnsavedEdits();
    bool otherDirty = assetEditors_.anyDirty();
#if AVER_MODULE_LANDSCAPE
    otherDirty = otherDirty || landscape_.dirty();
#endif
    ImGui::TextWrapped("The runtime reads everything from disk.");
    if (levelDirty)
        ImGui::TextWrapped("This level has unsaved changes.");
    if (otherDirty)
        ImGui::TextDisabled("Unsaved asset or terrain edits are not included -- save them first.");
    ImGui::Spacing();
    ImGui::Separator();

    // Saves the LEVEL only; offered only when the level is what is unsaved.
    ImGui::BeginDisabled(!levelDirty);
    const bool saveAndLaunch = ImGui::Button("Save and launch", ImVec2(160.0f * dpi_, 0.0f));
    ImGui::EndDisabled();
    if (saveAndLaunch) {
        if (saveLevel(levelPath_)) {
            ImGui::CloseCurrentPopup();
            launchRuntimePrompt_ = false;
            launchInRuntime(e, true);
        } else {
            // STAY OPEN on a failed save -- same reasoning as drawPendingOpenPrompt's identical
            // guard: proceeding anyway would launch a level missing the very edits this is for.
            AVER_ERROR("[Editor] could not save '{}'; the runtime was not launched", levelPath_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Launch saved version", ImVec2(170.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        launchRuntimePrompt_ = false;
        launchInRuntime(e, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
        ImGui::CloseCurrentPopup();
        launchRuntimePrompt_ = false;
    }
    ImGui::EndPopup();
#endif
}

// Draws the modal offering to add the project files this project is missing, listing each fix.
void SandboxApp::drawUpgradePrompt() {
#if AVER_WITH_IMGUI
    if (pendingUpgrade_.empty() || upgradeAsked_) return;
    constexpr const char* kTitle = "Upgrade project?";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);

    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("'%s' was created by an earlier version of the editor and is missing some "
                       "of the files a project now needs.", project_.name.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Nothing you wrote is replaced. The .csproj is edited by appending, not "
                        "regenerated, so anything you added by hand stays.");
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::BeginChild("##upgradeList", ImVec2(0.0f, 200.0f * dpi_), ImGuiChildFlags_Borders)) {
        for (const editor::ProjectFix& f : pendingUpgrade_.fixes) {
            ImGui::BulletText("%s", f.summary.c_str());
            if (!f.detail.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("%s", f.detail.c_str());
                ImGui::Unindent();
            }
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    if (ImGui::Button("Upgrade", ImVec2(120.0f * dpi_, 0.0f))) {
        std::string err;
        if (editor::applyProjectUpgrade(project_, pendingUpgrade_, &err)) {
            setUpgradeStatus("Project upgraded - Compile C# to rebuild.");
            AVER_INFO("[Editor] '{}' upgraded", project_.name);
        } else {
            setUpgradeStatus("Upgrade failed: " + err, editor::NotifySeverity::Error);
            AVER_ERROR("[Editor] upgrade of '{}' failed: {}", project_.name, err);
        }
        pendingUpgrade_ = {};
        upgradeAsked_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(120.0f * dpi_, 0.0f))) {
        upgradeAsked_ = true;
        setUpgradeStatus("Project left as it is.", editor::NotifySeverity::Info);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Not now leaves every file untouched.");
    ImGui::EndPopup();
#endif
}

#if AVER_WITH_IMGUI
// Play toolbar: Pause/Resume/Eject/Stop, or Idle/Play dropdown.
void SandboxApp::drawPlayToolbar(Engine& e) {
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const f32 wsizeX = mv->WorkSize.x;

#if AVER_MODULE_FRAMEWORK
    const ImGuiStyle& style = ImGui::GetStyle();
    auto btnW = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };

    const int32_t ps = aver_fw_play_state();
    const bool paused = ps == AVER_FW_PLAY_PAUSED;
    // anyPlayActive() includes stand-ins (drones/spectators have no begin_play).
    const bool anyPlay = anyPlayActive();
    // Pause/Skip/Eject need active framework session.
    const bool session = playSessionActive();
    const bool ejected = playEjected();

    const char* modeName = playMode_ == PlayMode::Simulate   ? "Simulate"
                          : playMode_ == PlayMode::Standalone ? "Standalone" : "Play";
    const std::string playLabel = std::string(ICON_PLAY " ") + modeName;
    const char* pauseLabel = paused ? ICON_PLAY " Resume" : ICON_PAUSE " Pause";
    constexpr const char* kFrameSkipLabel = ICON_SKIP_NEXT " Frame Skip";
    const char* ejectLabel = ejected ? ICON_GAMEPAD " Possess" : ICON_EJECT " Eject";
    constexpr const char* kStopLabel = ICON_STOP " Stop";
    // dropButton returns only the click.
    const f32 dropW = btnW(" ") + 16.0f * dpi_;

    f32 grpW = 0.0f;
    u32 buttons = 0;
    const auto add = [&](f32 w) { grpW += w; ++buttons; };
    if (!anyPlay) {
        add(btnW(playLabel.c_str()));
        add(dropW);
    } else {
        if (session) add(btnW(pauseLabel));
        if (session && paused) add(btnW(kFrameSkipLabel));
        if (session) add(btnW(ejectLabel));
        add(btnW(kStopLabel));
    }
    grpW += style.ItemSpacing.x * static_cast<f32>(buttons > 0 ? buttons - 1 : 0);
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsizeX - grpW) * 0.5f));

    if (!anyPlay) {
        if (ImGui::Button(playLabel.c_str())) launchPlay(e, playMode_);
        uiReg_.track("toolbar.play");
        if (ImGui::IsItemHovered()) {
            if (playMode_ == PlayMode::Standalone) {
                ImGui::SetTooltip("Standalone Game: a separate Aver Engine Runtime on the saved level");
            } else if (playMode_ == PlayMode::NewWindow) {
                ImGui::SetTooltip("Play in a new window (Esc there returns to the editor)");
            } else {
                const editor::CommandId modeCmd = playMode_ == PlayMode::Simulate
                                                 ? editor::CommandId::PlaySimulate : editor::CommandId::PlayStart;
                ImGui::SetTooltip("%s (%s)", playMode_ == PlayMode::Simulate ? "Simulate" : "Play in the viewport",
                                  editor::chordToString(keybinds_.chordFor(modeCmd)).c_str());
            }
        }
        ImGui::SameLine(0.0f, 0.0f);

        if (dropButton(" ")) ImGui::OpenPopup("playOptions");
        uiReg_.track("toolbar.play.options");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play options");
        if (ImGui::BeginPopup("playOptions")) {
            ImGui::TextDisabled("MODES");
            if (ImGui::MenuItem(ICON_PLAY " Selected Viewport",
                                editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayStart)).c_str(),
                                playMode_ == PlayMode::SelectedViewport))
                launchPlay(e, PlayMode::SelectedViewport);
            uiReg_.track("toolbar.play.options.viewport");
            if (ImGui::MenuItem(ICON_PLAY " New Window", nullptr, playMode_ == PlayMode::NewWindow))
                launchPlay(e, PlayMode::NewWindow);
            uiReg_.track("toolbar.play.options.newWindow");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Plays in a window of its own, without the editor around it.\n"
                                  "Esc in that window (or closing it) returns to the editor.");
            if (ImGui::MenuItem(ICON_EJECT " Simulate",
                                editor::chordToString(keybinds_.chordFor(editor::CommandId::PlaySimulate)).c_str(),
                                playMode_ == PlayMode::Simulate))
                launchPlay(e, PlayMode::Simulate);
            uiReg_.track("toolbar.play.options.simulate");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Runs the game without possessing the player: the editor keeps the\n"
                                  "camera, selection and gizmos. %s possesses.",
                                  editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayEject)).c_str());
            const bool haveLevel = !levelPath_.empty();
            const bool haveRuntime = !editor::runtimeExecutablePath().empty();
            const bool standaloneOk = project_.valid() && haveLevel && haveRuntime;
            if (ImGui::MenuItem(ICON_GAMEPAD " Standalone Game", nullptr,
                                playMode_ == PlayMode::Standalone, standaloneOk))
                launchPlay(e, PlayMode::Standalone);
            uiReg_.track("toolbar.play.options.standalone");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", !project_.valid() ? "Open a project first."
                                  : !haveLevel ? "Save the level first -- Standalone reads it from disk."
                                  : !haveRuntime ? "AverEngineRuntime.exe was not found beside the editor."
                                  : "A separate Aver Engine Runtime process on the saved level.");

            ImGui::Separator();
            ImGui::TextDisabled("SPAWN PLAYER AT");
            // Radio buttons keep the popup open while setting up.
            if (ImGui::RadioButton("Current Camera Location", playSpawnAt_ == PlaySpawnAt::CameraLocation))
                playSpawnAt_ = PlaySpawnAt::CameraLocation;
            uiReg_.track("toolbar.play.options.spawnCamera");
            if (ImGui::RadioButton("Default Player Start", playSpawnAt_ == PlaySpawnAt::PlayerStart))
                playSpawnAt_ = PlaySpawnAt::PlayerStart;
            uiReg_.track("toolbar.play.options.spawnPlayerStart");

            ImGui::Separator();
            ImGui::TextDisabled("WITH NO GAMEMODE");
            if (ImGui::RadioButton("Fly (drone, collides)", !defaultPawnWalk_)) defaultPawnWalk_ = false;
            uiReg_.track("toolbar.play.options.pawnFly");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Like Unreal's DefaultPawn: flies with no gravity inside a %.0f cm collision "
                                  "capsule that stops at walls and slides along them. WASD, Q/E down/up.", 35.0f);
            if (ImGui::RadioButton("Walk (gravity, stairs, Space jumps)", defaultPawnWalk_)) defaultPawnWalk_ = true;
            uiReg_.track("toolbar.play.options.pawnWalk");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The engine's default pawn walks as a capsule character, so a level can be "
                                  "walked without writing a GameMode. A project's own GameMode always wins.");

            ImGui::Separator();
            ImGui::Checkbox("Game Gets Mouse Control", &playGameGetsMouse_);
            uiReg_.track("toolbar.play.options.mouseControl");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Capture the mouse the moment Play starts. Off: click the viewport to\n"
                                  "give it to the game.");

            ImGui::Separator();
            if (ImGui::MenuItem("Advanced Settings...")) {
                showEditorPrefs_ = true;
                scrollPrefsToPlay_ = true;
            }
            uiReg_.track("toolbar.play.options.advanced");
            ImGui::EndPopup();
        }
        return;
    }

    if (session) {
        if (ImGui::Button(pauseLabel)) aver_fw_set_paused(paused ? 0 : 1);
        uiReg_.track("toolbar.play.pause");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (%s)", paused ? "Resume" : "Pause",
                              editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayPause)).c_str());
        ImGui::SameLine();
        if (paused) {
            if (ImGui::Button(kFrameSkipLabel)) requestPlayFrameStep();
            uiReg_.track("toolbar.play.frameSkip");
            if (ImGui::IsItemHovered()) {
                const editor::Chord& skipChord = keybinds_.chordFor(editor::CommandId::PlayFrameSkip);
                if (skipChord.isBound())
                    ImGui::SetTooltip("Advance one gameplay tick, then pause again (%s)",
                                      editor::chordToString(skipChord).c_str());
                else
                    ImGui::SetTooltip("Advance one gameplay tick, then pause again");
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(ejectLabel)) togglePlayEject();
        uiReg_.track("toolbar.play.eject");
        if (ImGui::IsItemHovered()) {
            const std::string ejectChord =
                editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayEject));
            const editor::Chord& pawnChord = keybinds_.chordFor(editor::CommandId::PlayPawnToCamera);
            if (ejected && pawnChord.isBound())
                ImGui::SetTooltip("Possess the player again (%s)\n"
                                  "%s first brings the pawn to the camera",
                                  ejectChord.c_str(), editor::chordToString(pawnChord).c_str());
            else if (ejected)
                ImGui::SetTooltip("Possess the player again (%s)", ejectChord.c_str());
            else
                ImGui::SetTooltip("Detach from the player: the game keeps running and the editor\n"
                                  "camera, selection and gizmos come back (%s)",
                                  ejectChord.c_str());
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(kStopLabel)) stopPlay();
    uiReg_.track("toolbar.stop");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stop (%s)",
                          editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayStop)).c_str());
#else
    (void)e;
    const f32 grpW = 200.0f * dpi_;
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsizeX - grpW) * 0.5f));
    ImGui::BeginDisabled(true);
    ImGui::Button(ICON_PLAY " Play"); ImGui::SameLine();
    ImGui::Button(ICON_PAUSE " Pause"); ImGui::SameLine();
    ImGui::Button(ICON_STOP " Stop");
    ImGui::EndDisabled();
#endif
}
#endif // AVER_WITH_IMGUI

// Builds the whole editor UI for one frame: menu bar, toolbars, panels, drawers and dialogs.
void SandboxApp::buildUI(Engine& e) {
#if AVER_WITH_IMGUI
    uiReg_.beginFrame();
#endif
    prefsDevice_ = e.device();
#if AVER_WITH_IMGUI
    if (!prefsLoaded_) { prefsLoaded_ = true; loadEditorPreferences(); }
#endif
#if AVER_MODULE_SCENE
    if (wantMeshReload_) {
        wantMeshReload_ = false;
        releaseProjectMeshes(e);
        loadProjectMeshes(e);
    }
#endif
    if (vsyncOffRequested_) {
        vsyncOffRequested_ = false;
        if (prefsDevice_->vsyncCanDisable()) { prefsDevice_->setVSync(false); AVER_INFO("[Sandbox] vsync OFF (--no-vsync)"); }
        else AVER_WARN("[Sandbox] --no-vsync ignored: this display path cannot tear");
    }
    // --vsync: forces it ON, overriding stored preference.
    if (vsyncOnRequested_) {
        vsyncOnRequested_ = false;
        prefsDevice_->setVSync(true);
        AVER_INFO("[Sandbox] vsync ON (--vsync)");
    }
#if AVER_WITH_IMGUI
    if (!e.device()->uiActive()) return;
    ++frameNo_;   // the Content Browser's directory-cache freshness clock

    if (browserActive_) {
        // Test hook: AVER_TEST_BROWSER_OPEN=<.ocproject> opens it through this browser path after 120 frames.
        static const char* testOpen = std::getenv("AVER_TEST_BROWSER_OPEN");
        if (testOpen && *testOpen && frameNo_ >= 120 && browser_.open(testOpen, nullptr)) {
            applyProject(e);
            browserActive_ = false;
            return;
        }
        switch (browser_.draw(dpi_, fontMedium_, logoUiId_, logoAspect_)) {
            case editor::BrowserAction::Open: applyProject(e); browserActive_ = false; break;
            case editor::BrowserAction::Skip: browserActive_ = false; break;
            case editor::BrowserAction::Quit: requestExitChecked(e); break;
            case editor::BrowserAction::Stay: break;
        }
        return;
    }

    // Test hook: AVER_TEST_OPEN_LEVEL=<level path> opens it as the Content Browser does, 300 frames after the
    // project is up (the mid-session level open a user does).
#if AVER_MODULE_SCENE
    {
        static const char* testLevel = std::getenv("AVER_TEST_OPEN_LEVEL");
        static u64 projectUpAt = 0;
        static bool levelDone = false;
        if (testLevel && *testLevel && !levelDone) {
            if (!projectUpAt) projectUpAt = frameNo_;
            else if (frameNo_ >= projectUpAt + 300) { levelDone = true; requestOpenLevel(testLevel, "test hook"); }
        }
    }
#endif
    // Before anything draws; reaps finished workers and may start one.
    revisionControlTick();

    // Drawer shortcuts (Ctrl+Space: Content Browser, `: Console, Escape: close).
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !io.WantCaptureKeyboard) {
            if (keybinds_.pressed(editor::CommandId::DrawerToggleContent, io)) toggleDrawer(Drawer::Content);
            if (keybinds_.pressed(editor::CommandId::DrawerToggleConsole, io)) toggleDrawer(Drawer::Console);
            if (drawer_ != Drawer::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                keybinds_.pressed(editor::CommandId::DrawerDismiss, io))
                drawer_ = Drawer::None;
        }
    }

    // ---- menu bar ----
    if (ImGui::BeginMainMenuBar()) {
        if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
        ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE");
                    const bool open_file = ImGui::BeginMenu("File");
        uiReg_.track("menu.file");
        if (open_file){
#if AVER_MODULE_SCENE
            const bool haveProject = project_.valid();
            ImGui::BeginDisabled(!haveProject);
            // Asks first; prevents silently discarding unsaved edits.
            if (ImGui::MenuItem("New Level")) {
                if (levelHasUnsavedEdits()) pendingNewLevel_ = true;
                else                        startNewLevel(e);
            }
            uiReg_.track("file.newLevel");
            // Opens a picker; shows all levels, not just the start level.
            if (ImGui::MenuItem("Open Level...")) openLevelPickerNow();
            uiReg_.track("file.openLevel");
            // Reload: reopens the start level to discard edits.
            ImGui::BeginDisabled(project_.startMap.empty());
            if (ImGui::MenuItem("Reload Start Level")) {
                const std::string start = project_.contentDir() + "\\" + project_.startMap;
                if (!requestOpenLevel(start, "reloaded from the project's start level"))
                    AVER_WARN("[Level] reload: {}", openLevelError_);
            }
            ImGui::EndDisabled();
            uiReg_.track("file.reloadStartLevel");
            if (ImGui::MenuItem("Save Level", "Ctrl+S")) saveLevelInteractive();
            uiReg_.track("file.saveLevel");
            if (ImGui::MenuItem("Save All", editor::chordToString(keybinds_.chordFor(editor::CommandId::SaveAll)).c_str()))
                saveAll();
            uiReg_.track("file.saveAll");
            // Save Level As: saveLevel takes an arbitrary path; this provides the UI.
            if (ImGui::MenuItem("Save Level As...")) {
                saveLevelAsName_[0] = '\0';
                const std::string stem = levelPath_.empty()
                    ? levelName_
                    : std::filesystem::path(levelPath_).stem().string();
                std::snprintf(saveLevelAsName_, sizeof saveLevelAsName_, "%s",
                              stem.empty() ? "untitled" : stem.c_str());
                saveLevelAsError_.clear();
                wantSaveLevelAs_ = true;
            }
            uiReg_.track("file.saveLevelAs");
            ImGui::EndDisabled();
            if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Open or create a project first - a level belongs to one.");
#else
            ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level");
#endif
#if AVER_MODULE_LANDSCAPE
            // Sculpt changes the resident section in memory; save separately.
            {
                const bool canSave = landscape_.loaded() && !landscape_.path().empty();
                ImGui::BeginDisabled(!canSave);
                if (ImGui::MenuItem(landscape_.dirty() ? "Save Landscape *" : "Save Landscape")) saveLandscape();
                ImGui::EndDisabled();
                uiReg_.track("file.saveLandscape");
                if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(!landscape_.loaded()
                        ? "No landscape section loaded (--landscape, or <levelname>.ocland beside the level)."
                        : "This section has no file path to save back to.");
            }
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Take Screenshot", editor::chordToString(keybinds_.chordFor(editor::CommandId::Screenshot)).c_str()))
                requestViewportScreenshot();
            uiReg_.track("file.takeScreenshot");
#if AVER_MODULE_SCENE
            // Starts AverEngineRuntime.exe on this level (separate process).
            {
                const bool haveProj = project_.valid();
                const bool haveLevel = !levelPath_.empty();
                const std::string runtimeExe = editor::runtimeExecutablePath();
                ImGui::BeginDisabled(!haveProj || !haveLevel || runtimeExe.empty());
                if (ImGui::MenuItem("Launch in Aver Engine Runtime")) launchInRuntime(e);
                ImGui::EndDisabled();
                uiReg_.track("file.launchRuntime");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (!haveProj)       ImGui::SetTooltip("Open a project first.");
                    else if (!haveLevel) ImGui::SetTooltip("Save the level first - the runtime reads it from disk.");
                    else if (runtimeExe.empty())
                        ImGui::SetTooltip("AverEngineRuntime.exe is not beside the editor - build the runtime (AVER_BUILD_GAME).");
                    else ImGui::SetTooltip("Starts AverEngineRuntime.exe on this level, as a packaged game would run it.");
                }
            }
#endif
            ImGui::Separator();
            // Packaging disabled only with specific reason.
            {
                const bool haveProj = project_.valid();
                const bool haveScripts = haveProj &&
                    std::filesystem::exists(editor::scriptsBinaryDir(project_));
                ImGui::BeginDisabled(!haveProj || tools_.compiling());
                if (ImGui::MenuItem("Package Project...")) tools_.openPackageProject();
                ImGui::EndDisabled();
                uiReg_.track("file.packageProject");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (!haveProj)              ImGui::SetTooltip("Open a project first.");
                    else if (tools_.compiling()) ImGui::SetTooltip("A script build is running; packaging would stage a half-written assembly.");
                    else if (!haveScripts)       ImGui::SetTooltip("No compiled scripts yet - run Compile .NET first.\nThe packager refuses a project whose Binaries\\Scripts is empty.");
                    else                         ImGui::SetTooltip("Stage this project into a standalone, runnable game directory.\nRuns scripts/stage-game.ps1, which you can also run from a shell.");
                }
            }
            ImGui::Separator(); if(ImGui::MenuItem("Exit")) requestExitChecked(e); ImGui::EndMenu(); }
                    const bool open_edit = ImGui::BeginMenu("Edit");
        uiReg_.track("menu.edit");
        if (open_edit){
            if (ImGui::MenuItem("Undo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditUndo)).c_str(), false, canUndo())) undo();
            uiReg_.track("edit.undo");
            if (ImGui::MenuItem("Redo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditRedo)).c_str(), false, canRedo())) redo();
            uiReg_.track("edit.redo");
            ImGui::Separator();
            // Shortcut hints from the same registry as handleManip(), so menu and keyboard agree.
            if (ImGui::MenuItem("Copy", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditCopy)).c_str(), false, anySelected())) copySelection();
            uiReg_.track("edit.copy");
            if (ImGui::MenuItem("Paste", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditPaste)).c_str(), false, !clipboard_.entities.empty() || clipboard_.hasObject)) pasteClipboard();
            uiReg_.track("edit.paste");
            if (ImGui::MenuItem("Duplicate", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDuplicate)).c_str(), false, anySelected())) duplicateSelection();
            uiReg_.track("edit.duplicate");
            if (ImGui::MenuItem("Delete", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDelete)).c_str(), false, anySelected())) deleteSelection();
            uiReg_.track("edit.delete");
            ImGui::Separator();
            if (ImGui::MenuItem("Editor Preferences...")) showEditorPrefs_ = true;
            uiReg_.track("edit.editorPreferences");
            if (ImGui::MenuItem("Project Settings...")) showProjectSettings_ = true;
            uiReg_.track("edit.projectSettings");
            ImGui::EndMenu();
        }
                    const bool open_window = ImGui::BeginMenu("Window");
        uiReg_.track("menu.window");
        if (open_window){
            // MenuItem now captures return value so panels can be reopened.
            ImGui::MenuItem("World Outliner", nullptr, &showOutliner_);
            uiReg_.track("window.worldOutliner");
            ImGui::MenuItem("Details", nullptr, &showDetails_);
            uiReg_.track("window.details");
            ImGui::MenuItem("Soft Body (plastic)", nullptr, &showSoftBody_);
            uiReg_.track("window.softBody");
            if (ImGui::MenuItem("Content Browser", "Ctrl+Space", drawer_ == Drawer::Content)) toggleDrawer(Drawer::Content);
            uiReg_.track("window.contentBrowser");
            if (ImGui::MenuItem("Output Log", nullptr, drawer_ == Drawer::Log)) toggleDrawer(Drawer::Log);
            // track() names the LAST item; keep it immediately after the widget.
            uiReg_.track("window.outputLog");
            if (ImGui::MenuItem("Console", "`", drawer_ == Drawer::Console)) toggleDrawer(Drawer::Console);
            uiReg_.track("window.console");
            if (ImGui::MenuItem("World Settings", nullptr, showWorldSettings_))
                showWorldSettings_ = !showWorldSettings_;
            uiReg_.track("window.worldSettings");
            ImGui::Separator();
#if AVER_MODULE_SCENE
            {
                const bool streamOn = streaming_.enabled();
                ImGui::BeginDisabled(!project_.valid());
                if (ImGui::MenuItem("Chunk Streaming", nullptr, streamOn)) setChunkStreamingEnabled(!streamOn);
                ImGui::EndDisabled();
                uiReg_.track("window.chunkStreaming");
                ImGui::MenuItem("GPU Profiler", nullptr, &showProfiler_);
                uiReg_.track("window.gpuProfiler");
                ImGui::MenuItem("Neural Visualiser", nullptr, &showNeuralViz_);
                uiReg_.track("window.neuralVisualiser");
                // Which files name an asset; reachable from Content Browser context menu too.
                ImGui::MenuItem("References", nullptr, &showReferences_);
                uiReg_.track("window.references");
                if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - a streamed world belongs to one.");
                else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(streamOn
                        ? "Streams chunks generated + saved under <project>\\Chunks around the editor camera.\nTurning this off releases every streamed entity."
                        : "Opt-in: streams chunks generated + saved under <project>\\Chunks around the editor camera.\nDoes nothing to your level until switched on.");
            }
            {
                const bool droneOn = droneEntity_ != scene::kInvalidEntity;
                ImGui::BeginDisabled(!project_.valid());
                if (ImGui::MenuItem("Drone", nullptr, droneOn)) setDroneEnabled(!droneOn);
                ImGui::EndDisabled();
                uiReg_.track("window.drone");
                if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - the drone reads its graph from Content\\Scripts.");
                else if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(droneOn
                        ? "A cube driven by Content\\Scripts\\Drone.ocgraph through GraphHost.\nTurning this off releases it."
                        : "Opt-in: spawns a cube and drives it from Content\\Scripts\\Drone.ocgraph through GraphHost.");
            }
            ImGui::Separator();
#endif
            // Outside AVER_MODULE_SCENE; git info is independent of ECS.
            ImGui::MenuItem("Revision Control", nullptr, &showRevisionControl_);
            uiReg_.track("window.revisionControl");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("What git says about this project: the branch, what changed, and\n"
                                  "each file's history and diff. It READS the repository and cannot\n"
                                  "alter one -- there is no commit, revert or push here.");
            ImGui::Separator();
            ImGui::BeginDisabled(gameUi_ == nullptr);
            if (ImGui::MenuItem("Game UI Demo", nullptr, showUiDemo_)) showUiDemo_ = !showUiDemo_;
            uiReg_.track("window.gameUiDemo");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(gameUi_ ? "A hand-written Aver.UI draw list, until there is a widget tree to produce one."
                                          : "The UI render feature is unavailable on this backend.");
            ImGui::Separator();
            // Reset Layout applies to the focused tab.
            const bool assetTabActive = !levelVisible_ && assetEditors_.anyOpen();
            if (ImGui::MenuItem(assetTabActive ? "Reset Tab Layout" : "Reset Layout")) {
                if (assetTabActive) assetEditors_.resetFocusedLayout();
                else              { dockBuilt_ = false; dockResetRequested_ = true; }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(assetTabActive
                    ? "Restores this tab's column widths.\nThe editor's panel layout is left alone."
                    : "Restores every panel to its default slot.\nOpen an asset tab to reset that tab instead.");
            ImGui::EndMenu();
        }
        tools_.drawMenu(project_);
        if (ImGui::BeginMenu("Build")){
            // Build Geometry is gone; no CSG/brush system exists. Build Lighting planned.
            ImGui::BeginDisabled(true);
            ImGui::MenuItem("Build Lighting");
            ImGui::EndDisabled();
            uiReg_.track("build.buildLighting");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("No lightmap baker yet.\nLighting is real-time: the sun, and voxel cone-traced GI.");
#if AVER_MODULE_SYNAPSE
            ImGui::Separator();
#if AVER_MODULE_SCENE
            // Needs both SYNAPSE (grid math) and SCENE (a world to sample).
            if (ImGui::MenuItem("Bake Navigation")) bakeNavigationNow(e);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Samples the live physics scene into a walkable grid and "
                                  "writes it beside this level as .ocnav.\n"
                                  "Colours in the overlay are REGIONS: two patches of floor "
                                  "in different colours have no path between them.");
#endif
            // Show Navigation works even without SCENE (reads a baked .ocnav).
            ImGui::MenuItem("Show Navigation", nullptr, &showNav_);
#if AVER_MODULE_SCENE
            ImGui::MenuItem("Show Decals", nullptr, &showDecals_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draws every decal's projector box, not just the selected one's.");
            if (ImGui::BeginMenu("Show AI")) {
                ImGui::MenuItem("Sight Cones", nullptr, &aiDebug_.sight);
                ImGui::MenuItem("Hearing", nullptr, &aiDebug_.hearing);
                ImGui::MenuItem("Path / Goal", nullptr, &aiDebug_.path);
                ImGui::MenuItem("Steering", nullptr, &aiDebug_.steering);
                ImGui::MenuItem("Behaviour Tree State", nullptr, &aiDebug_.btState);
                ImGui::MenuItem("Cover", nullptr, &aiDebug_.cover);
                ImGui::EndMenu();
            }
            uiReg_.track("view.showAi");
#endif
#if AVER_MODULE_PHYSICS
            // Toggling forgets the overlay; rebuildColliderOverlay skips unchanged frames.
            if (ImGui::MenuItem("Show Colliders", nullptr, &showColliders_))
                colliderOverlayBuilt_ = false;
            uiReg_.track("view.showColliders");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every physics body's WORLD-SPACE BOUNDING BOX.\n"
                                  "Green = static box, blue = static triangle mesh, amber = it moves.\n"
                                  "A box collider is drawn exactly; a triangle mesh, sphere or capsule\n"
                                  "is drawn as its bound -- it collides with its actual shape inside.");
#endif
            if (ImGui::MenuItem("Region Colours", nullptr, &navRegionColours_))
                rebuildNavOverlay(e);
#endif
            ImGui::EndMenu();
        }
        uiReg_.track("menu.build");
        if (ImGui::BeginMenu("Select")){
            // Select All now works; disabled only when nothing to select.
#if AVER_MODULE_SCENE
            ImGui::BeginDisabled(outlinerOrder_.empty());
            if (ImGui::MenuItem("Select All",
                                editor::chordToString(
                                    keybinds_.chordFor(editor::CommandId::EditSelectAll)).c_str()))
                selectAllInOutliner();
            ImGui::EndDisabled();
            uiReg_.track("select.all");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && outlinerOrder_.empty())
                ImGui::SetTooltip("Nothing is listed in the World Outliner to select.");
#endif
            // Clears both sel_ and selEntity_ to avoid stale data.
            if(ImGui::MenuItem("Select None")) { sel_=-1; selEntity_=kInvalidId; }
            uiReg_.track("select.none");
            ImGui::EndMenu();
        }
        uiReg_.track("menu.select");
        if (ImGui::BeginMenu("Help")){
            if (ImGui::MenuItem("About Aver Engine")) showAbout_ = true;
            uiReg_.track("help.about");
            ImGui::EndMenu();
        }
        uiReg_.track("menu.help");
        if (fontMedium_) ImGui::PopFont();
        ImGui::EndMainMenuBar();
    }

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize; // already excludes the menu bar
    const f32 toolbarH = 42.0f*dpi_, statusH = 26.0f*dpi_;

    // ---- main toolbar ----
    ImGui::SetNextWindowPos(wpos);
    ImGui::SetNextWindowSize(ImVec2(wsize.x, toolbarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##maintoolbar", nullptr, kChromeFlags);
    ImGui::SetCursorPosY((toolbarH - ImGui::GetFrameHeight()) * 0.5f);
    // levelPath_ and saveLevel() are under AVER_MODULE_SCENE only.
#if AVER_MODULE_SCENE
    // Opens Save As if level has no path; otherwise saves.
    if (ImGui::Button(ICON_SAVE " Save")) saveLevelInteractive();
    uiReg_.track("toolbar.save");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(levelPath_.empty() ? "Save Level As..."
                                             : "Save Level (Ctrl+S)");
#else
    ImGui::BeginDisabled(true);
    ImGui::Button("Save");
    uiReg_.track("toolbar.save");
    ImGui::EndDisabled();
#endif
    ImGui::SameLine();
    if (dropButton("Add")) ImGui::OpenPopup("addActor");
    uiReg_.track("toolbar.add");
    if (ImGui::BeginPopup("addActor")) {
        ImGui::TextDisabled("Place Actor"); ImGui::Separator();
        if (ImGui::Selectable("Cube"))     spawnCube(e);
        if (ImGui::Selectable("Player Start")) addPlayerStart(e);
        uiReg_.track("toolbar.add.playerStart");
        if (ImGui::Selectable("Sphere")) spawnPrimitive(e, "Meshes/sphere.ocmesh", "Sphere");
        uiReg_.track("toolbar.add.sphere");
        ImGui::Selectable("Plane",   false, ImGuiSelectableFlags_Disabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No plane primitive yet.\nA flattened cube is the convention: add a Cube and scale Z down.");
#if AVER_MODULE_SCENE
        if (ImGui::Selectable("Point Light")) spawnLightAtCamera(scene::kLightPoint);
        uiReg_.track("toolbar.add.pointLight");
        if (ImGui::Selectable("Spot Light")) spawnLightAtCamera(scene::kLightSpot);
        uiReg_.track("toolbar.add.spotLight");
        if (ImGui::Selectable("Rect Light")) spawnLightAtCamera(scene::kLightRect);
        uiReg_.track("toolbar.add.rectLight");
        if (ImGui::Selectable("Decal")) spawnDecalAtCamera();
        uiReg_.track("toolbar.add.decal");
#endif
        ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
    uiReg_.track("toolbar.compileCs");
    ImGui::SameLine();
    drawPlayToolbar(e);
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 130.0f*dpi_));
    if (dropButton(ICON_SETTINGS " Settings")) ImGui::OpenPopup("settingsMenu");
    if (ImGui::BeginPopup("settingsMenu")) {
        ImGui::Checkbox("Show Grid", &showGrid_);
        ImGui::Checkbox("Wireframe", &wireframe_);
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // ---- dockspace host ----
    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, wsize.y - toolbarH - statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("##dockhost", nullptr, kChromeFlags | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("AverDockspace");
    const ImVec2 dockSize(wsize.x, wsize.y - toolbarH - statusH);
    ImGui::DockSpace(dockId, ImVec2(0,0), ImGuiDockNodeFlags_PassthruCentralNode);

    // Defer to restored layout from ini file; reset only on explicit Reset Layout.
    const ImGuiDockNode* restored = ImGui::DockBuilderGetNode(dockId);
    const bool adoptRestored = !dockBuilt_ && !dockResetRequested_ &&
                               restored && restored->IsSplitNode();
    if (adoptRestored) {
        dockBuilt_ = true;
        // Logged once to detect regressions (default == restored on first launch).
        AVER_INFO("[Editor] dock layout restored from editor-layout.ini");
    }

    if (!dockBuilt_ && dockSize.x > 1.0f && dockSize.y > 1.0f) {
        dockResetRequested_ = false;
        dockBuilt_ = true;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace); // private flag, required here
        ImGui::DockBuilderSetNodeSize(dockId, dockSize); // must precede the splits
        ImGuiID centre = dockId, right = 0, rightTop = 0, rightBottom = 0, left = 0;
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, &right,  &centre);
        ImGui::DockBuilderSplitNode(right,  ImGuiDir_Down,  0.60f, &rightBottom, &rightTop);
        // Mode panel left (opposite details); left "what am I doing", right "what is this object".
        ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, &left, &centre);
        ImGui::DockBuilderDockWindow("World Outliner",  rightTop);
        ImGui::DockBuilderDockWindow("Details",         rightBottom);
        ImGui::DockBuilderDockWindow("Mode",            left);
        ImGui::DockBuilderDockWindow("Level",           centre);
        ImGui::DockBuilderFinish(dockId);
    }
    if (const ImGuiDockNode* cn = ImGui::DockBuilderGetCentralNode(dockId)) centralDock_ = cn->ID;
    ImGui::End(); // ##dockhost

    // ---- the level, as a tab drawing the scene texture ----
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (focusLevelAt_ > 0 && ImGui::GetFrameCount() == focusLevelAt_) {
            ImGui::SetWindowFocus("Level");
            AVER_INFO("[Editor] --focus-level-at: bringing the Level tab forward");
        }
        levelVisible_ = ImGui::Begin("Level", nullptr,
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                     ImGuiWindowFlags_NoCollapse);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 w = avail.x > 8.0f ? avail.x : 8.0f;
        const f32 h = avail.y > 8.0f ? avail.y : 8.0f;

        // Play in New Window owns the scene rect (updatePlayWindow); the tab only says where the game went.
        if (!playWindow_) vpX_ = at.x, vpY_ = at.y, vpW_ = w, vpH_ = h;
        drawGraphPrintOverlay(at, ImVec2(at.x + w, at.y + h));
        levelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        levelHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        if (playWindow_) {
            if (e.device()) e.device()->setViewportToTexture(true);
            const char* note = "Playing in a separate window. Press Esc there, or Stop, to return.";
            const ImVec2 ts = ImGui::CalcTextSize(note);
            ImGui::SetCursorScreenPos(ImVec2(at.x + std::fmax(0.0f, (w - ts.x) * 0.5f), at.y + h * 0.5f - ts.y));
            ImGui::TextDisabled("%s", note);
        } else if (levelVisible_ && e.device()) {
            e.device()->setViewportToTexture(true);
            if (const u64 tex = e.device()->viewportTextureId()) {
                // The texture is the whole backbuffer; the scene is this sub-rect of it.
                const f32 bw = ImGui::GetIO().DisplaySize.x;
                const f32 bh = ImGui::GetIO().DisplaySize.y;
                if (bw > 1.0f && bh > 1.0f) {
                    const ImVec2 uv0(at.x / bw, at.y / bh);
                    const ImVec2 uv1((at.x + w) / bw, (at.y + h) / bh);
                    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h), uv0, uv1);
                    // Drop target for content-browser assets.
                    if (ImGui::BeginDragDropTarget()) {
                        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kCbMoveDragDropType);
                        if (!payload) payload = ImGui::AcceptDragDropPayload(kAssetDragDropType);
                        if (payload && payload->Data) {
                            const std::string droppedPath(
                                static_cast<const char*>(payload->Data),
                                payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
                            const ImVec2 mp = ImGui::GetMousePos();
#if AVER_MODULE_SCENE
                            // Multiple paths separated by newlines; drop all at cursor.
                            const std::vector<std::string> dragged = editor::splitDropPayload(droppedPath);
                            usize placed = 0;
                            for (const std::string& one : dragged) {
                                std::error_code dirEc;
                                if (std::filesystem::is_directory(std::filesystem::path(one), dirEc)) continue;
                                if (!isPlaceableAssetExt(lowerExt(std::filesystem::path(one)))) continue;
                                spawnFromAssetDrop(e, one, mp.x, mp.y);
                                ++placed;
                            }
                            if (placed == 0) {
                                cbStatus_ = "Nothing in that drag can be placed in the level (only .ocmesh/.ocparticle assets can)";
                                AVER_INFO("[Editor] drop: none of the {} dragged item(s) is a placeable asset",
                                          dragged.size());
                            }
#else
                            cbStatus_ = "Placing objects needs the scene module";
                            AVER_WARN("[Editor] drop: scene module not compiled in, ignoring '{}'", droppedPath);
#endif
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // No mode panel in Select (redundant with viewport toolbar).
    outlinerFocused_ = false;
    detailsFocused_  = false;
    if (levelVisible_ || !assetEditors_.anyOpen()) {
        if (mode_ != EditorMode::Select) buildModePanel(e);
#if AVER_MODULE_SCENE
        // A floating timeline over the viewport's bottom edge until the user docks it.
        if (mode_ == EditorMode::Animate) buildSequencerWindow();
#endif
        buildPanels(e);
    }
    if (levelVisible_) buildViewportOverlay();
    drawDrawer(e);
    // drawNotifications after drawDrawer (needs drawerPixelH_ from this frame).
    drawNotifications();
#if AVER_MODULE_SCENE
    editor::prefabCreateDialogDraw(prefabDlg_, prefabModel_, prefabUiCallbacks());
#endif
    buildEditorPrefs();
    buildProjectSettings();
    buildWorldSettings();
    buildProfilerPanel(e);
    buildNeuralVisualiserPanel(e);
    buildReferencesPanel();
    buildRevisionControlPanel();
#if AVER_MODULE_SCENE
#if AVER_WITH_IMGUI
    buildChunkStreamingPanel();
#endif
#endif
#if AVER_MODULE_SCENE
    // Cleared after it fires (prevent repeated firing each frame).
    if (!selectEntity_.empty() && frameNo_ > 5) {
        const std::string want = selectEntity_;
        selectEntity_.clear();
        // Pseudo-entries (Sun, Sky, Post Process) aren't entities but appear in Outliner/Details.
        {
            struct Pseudo { int sel; const char* shown; };
            static const Pseudo kPseudo[] = {
                {-2, "Directional Light (Sun)"}, {-3, "Sky + Atmosphere"}, {-4, "Post Process"},
            };
            for (const Pseudo& ps : kPseudo) {
                if (std::string(ps.shown).find(want) == std::string::npos) continue;
                sel_ = ps.sel; selEntity_ = kInvalidId;
                AVER_INFO("[Editor] --select matched '{}' for '{}'", ps.shown, want);
                return;
            }
        }
        scene::World& sw = scene::World::instance();
        scene::Entity found = kInvalidId;
        // Enumerate like Outliner does (count/at), match against world authority.
        const u32 n = sw.count();
        for (u32 i = 0; i < n && found == kInvalidId; ++i) {
            const scene::Entity ent = sw.at(i);
            if (!sw.valid(ent) || sw.destroyPending(ent)) continue;
            const std::string nm = sw.name(ent);
            const auto lit = entityLabels_.find(static_cast<u32>(ent));
            const std::string shown = lit != entityLabels_.end() ? lit->second : nm;
            if (shown.find(want) != std::string::npos || nm.find(want) != std::string::npos)
                found = ent;
        }
        if (found != kInvalidId) {
            sel_ = kSelScene; selEntity_ = found;
            AVER_INFO("[Editor] --select matched entity {} ('{}') for '{}'",
                      (u32)found, sw.name(found), want);
        } else {
            AVER_WARN("[Editor] --select found no entity matching '{}' among {} in the world",
                      want, n);
        }
    }
#endif
    if (!openAsset_.empty() && frameNo_ > 5) {
        std::string want = openAsset_;
        openAsset_.clear();
        // Relative path is project-relative ("Content/..."), not editor launch directory.
        if (project_.valid() && !std::filesystem::path(want).is_absolute()) {
            const std::filesystem::path inProject = std::filesystem::path(project_.dir) / want;
            std::error_code ec;
            if (std::filesystem::exists(inProject, ec)) want = inProject.string();
        }
        lastOpenAssetPath_ = want;
        if (assetEditors_.open(want)) AVER_INFO("[Editor] --open-asset opened {}", want);
        else AVER_ERROR("[Editor] --open-asset: no registered editor accepts {}", want);
    }
    // --graph-select: run one frame after --open-asset so editor exists by then.
    if (!graphSelectNode_.empty() && frameNo_ > 6) {
        const std::string nodeId = graphSelectNode_;
        graphSelectNode_.clear();
        if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
            if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                // Report result, not call (captures against non-existent nodes were marked successful).
                if (ge->selectNode(nodeId))
                    AVER_INFO("[Editor] --graph-select selected '{}'", nodeId);
                else
                    AVER_ERROR("[Editor] --graph-select: '{}' names no node or component in {}",
                               nodeId, lastOpenAssetPath_);
            } else {
                AVER_ERROR("[Editor] --graph-select: '{}' is not a graph editor", lastOpenAssetPath_);
            }
        } else {
            AVER_ERROR("[Editor] --graph-select: no open editor for '{}'", lastOpenAssetPath_);
        }
    }
    // --graph-tab: run one frame after --open-asset (like --graph-select); graph editors only.
    if (!graphTab_.empty() && frameNo_ > 6) {
        const std::string tab = graphTab_;
        graphTab_.clear();
        if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
            if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                if (tab == "viewport") { ge->showViewportTab(); AVER_INFO("[Editor] --graph-tab viewport"); }
                else AVER_WARN("[Editor] --graph-tab: only 'viewport' is selectable, got '{}'", tab);
            } else {
                AVER_ERROR("[Editor] --graph-tab: '{}' is not a graph editor", lastOpenAssetPath_);
            }
        } else {
            AVER_ERROR("[Editor] --graph-tab: no open editor for '{}'", lastOpenAssetPath_);
        }
    }
    pumpContentWatch();
    cbApplyWatchEvents();
    assetEditors_.draw(e, centralDock_, dpi_);
    tools_.drawModals(project_, dpi_);
    nrd2Session_.drawWindow(dpi_);
    drawUpgradePrompt();
    drawAboutPrompt(e);
    drawSaveLevelAsPrompt();
    drawOpenLevelPrompt(e);
    drawRecoveryPrompt(e);
    drawExitPrompt(e);
    drawPendingOpenPrompt(e);
    drawLaunchRuntimePrompt(e);
#if AVER_MODULE_SCENE
    applyPendingOpen(e);
#endif

    // ---- status bar ----
    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##statusbar", nullptr, kChromeFlags);
    const f32 dt = e.time().dt;
    // Smooth FPS for profiler/Play CPU table; fold Play frame only if measured.
#if AVER_MODULE_FRAMEWORK
    const bool foldPlayFrame = anyPlayActive() &&
        (playSessionActive() ? playProf_.ranThisFrame(editor::PlayPhase::Gameplay)
                             : playProf_.anyRanThisFrame());
    playProf_.endFrame(dt, foldPlayFrame);
#else
    playProf_.endFrame(dt, false);
#endif
    const f64 smoothMs = playProf_.frameEmaMs();
    ImGui::SetCursorPosY((statusH - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::Text("%s  |  %s  |  %s  |  DPI %.0f%%  |  ",
                project_.valid() ? project_.name.c_str() : "No project",
                rhi::backendName(e.device()->backend()), e.device()->adapterName(), dpi_*100.f);
    ImGui::SameLine(0.0f, 0.0f);
    // Frame rate (clickable): with interpolation, shows real or real+interpolated. Real FPS only.
    {
        const bool interpolating = e.device()->frameInterpolated();
        const f64 realFps = smoothMs > 1e-3 ? 1000.0 / smoothMs : 0.0;
        char fpsLabel[96];
        // Short label; tooltip shows full text.
        if (interpolating && fpsCountsInterpolated_)
            std::snprintf(fpsLabel, sizeof fpsLabel, "%.0f FPS (%.0f real)###fps", realFps * 2.0, realFps);
        else if (interpolating)
            std::snprintf(fpsLabel, sizeof fpsLabel, "%.0f FPS real###fps", realFps);
        else
            std::snprintf(fpsLabel, sizeof fpsLabel, "%.0f FPS (%.2f ms)###fps", realFps, smoothMs);
        const char* shown = fpsLabel;
        const ImVec2 size = ImGui::CalcTextSize(shown, std::strstr(shown, "###"));
        if (ImGui::Selectable(fpsLabel, false, ImGuiSelectableFlags_None, size)) ImGui::OpenPopup("##fpsMode");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s"
                              "Smoothed over about half a second.\n"
                              "This real frame: %.0f FPS (%.2f ms)\n"
                              "Worst of the last %u frames: %.1f ms\n"
                              "Window > GPU Profiler has the CPU and GPU breakdown.\n"
                              "Click to choose whether interpolated frames are counted.",
                              interpolating ? "Frame interpolation is on: one interpolated image is shown\n"
                                              "between every two rendered (real) frames.\n" : "",
                              dt > 1e-6f ? 1.f / dt : 0.f, dt * 1000.f, playProf_.windowFrames(),
                              playProf_.worstFrameMs());
        if (ImGui::BeginPopup("##fpsMode")) {
            ImGui::TextDisabled("Frame rate shows");
            if (ImGui::RadioButton("Real frames only", !fpsCountsInterpolated_)) fpsCountsInterpolated_ = false;
            if (ImGui::RadioButton("Real + interpolated frames", fpsCountsInterpolated_)) fpsCountsInterpolated_ = true;
            if (!interpolating)
                ImGui::TextDisabled("Frame interpolation is not running, so both read the same.");
            ImGui::EndPopup();
        }
    }
    // VRAM against budget: over = GPU paging to RAM via PCIe.
    // Polled 2x/sec: grey <90%, amber >90%, red >100%.
    {
        vramPollS_ -= dt;
        if (vramPollS_ <= 0.0f) {
            vramPollS_ = 0.5f;
            vram_ = e.device()->videoMemory();
        }
        if (vram_.supported && vram_.localBudgetBytes > 0) {
            const f64 usedGb = static_cast<f64>(vram_.localUsageBytes) / (1024.0 * 1024.0 * 1024.0);
            const f64 budgetGb = static_cast<f64>(vram_.localBudgetBytes) / (1024.0 * 1024.0 * 1024.0);
            const f64 frac = static_cast<f64>(vram_.localUsageBytes) / static_cast<f64>(vram_.localBudgetBytes);
            const bool over = frac > 1.0;
            if (over != vramOverLogged_) {
                if (over)
                    AVER_WARN("[Editor] GPU memory over budget: {:.1f} of {:.1f} GB -- the system is paging GPU "
                              "memory to RAM and frame rate will suffer", usedGb, budgetGb);
                else
                    AVER_INFO("[Editor] GPU memory back under budget: {:.1f} of {:.1f} GB", usedGb, budgetGb);
                vramOverLogged_ = over;
            }
            const ImVec4 col = over ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
                             : frac > 0.9 ? ImVec4(0.95f, 0.72f, 0.25f, 1.0f)
                                          : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::SameLine(0.0f, 0.0f);
            // Short label; right-hand buttons are nearby.
            ImGui::TextColored(col, over ? "  |  VRAM! %.1f/%.1f GB" : "  |  VRAM %.1f/%.1f GB", usedGb, budgetGb);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s"
                                  "GPU memory this editor uses, against the budget Windows gives it (the card's\n"
                                  "memory minus what other programs and the desktop hold).\n\n"
                                  "Over budget, Windows moves part of it to system RAM and the GPU then reads\n"
                                  "that across PCIe -- frame rate drops, often unevenly.\n\n"
                                  "To get back under: lower the render scale (Editor Preferences > Display),\n"
                                  "lower the GI volume resolution (Project Settings > Rendering), turn frame\n"
                                  "interpolation off, or close GPU-heavy programs (browsers, video).\n\n"
                                  "Also in system memory now: %.1f GB.",
                                  over ? "OVER BUDGET.\n\n" : "",
                                  static_cast<f64>(vram_.nonLocalUsageBytes) / (1024.0 * 1024.0 * 1024.0));
        }
    }

#ifndef NDEBUG
    // A Debug editor runs several times slower; say so where the frame rate is read.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "  |  DEBUG BUILD");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("This editor was compiled in Debug: frame rates are not representative.\n"
                          "Build and run the Release configuration (build-release) to measure.");
#endif
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Text("  |  %zu actors  |  %s", objects_.size(), selectionLabel().c_str());

    // Upgrade outcome: 12s is readable but not permanent. Failure in error color.
    auto drawerButton = [&](const char* label, Drawer d, const char* tip) {
        const bool on = drawer_ == d;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(label)) toggleDrawer(d);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    // Cluster width not hardcoded; sum actual button widths per ImGui's layout.
    const bool rcBusy = rcStatusJob_ != nullptr;
    const editor::StatusBarSummary rcSummary = editor::summariseForStatusBar(
        project_.valid(), rcAnswered_, rcGitPresent_, rcRoot_, rcStatus_, rcWhy_);
    const std::string rcFace = rcStatusFace(rcBusy, rcSummary);

    const ImGuiStyle& barStyle = ImGui::GetStyle();
    const f32 barBtnPad = barStyle.FramePadding.x * 2.0f;
    const auto faceWidth = [&](const char* s) { return ImGui::CalcTextSize(s).x + barBtnPad; };
    f32 clusterW = faceWidth("Content Browser") + barStyle.ItemSpacing.x +
                   faceWidth("Output Log") + barStyle.ItemSpacing.x +
                   faceWidth("Console") + barStyle.ItemSpacing.x +
                   faceWidth(rcFace.c_str());
#if AVER_MODULE_MCP
    clusterW += barStyle.ItemSpacing.x + faceWidth(ICON_LINK " MCP :65535");
#endif
    // Small margin: cluster runs off edge rather than overlap text.
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - clusterW - 8.0f*dpi_));
    drawerButton("Content Browser", Drawer::Content, "Show the Content Browser  (Ctrl+Space)");
    ImGui::SameLine();
    drawerButton("Output Log", Drawer::Log, "Show the Output Log");
    ImGui::SameLine();
    drawerButton("Console", Drawer::Console, "Show the Console  (`)");
    // RC at far right; MCP drawn first (SameLine lays left to right).
#if AVER_MODULE_MCP
    ImGui::SameLine();
    drawMcpStatusWidget();
#endif
    ImGui::SameLine();
    drawRevisionControlStatusWidget();
    ImGui::End();
    ImGui::PopStyleVar(2);
#else
    (void)e;
#endif
}

#if AVER_WITH_IMGUI
// On-screen graph print feed (transient overlay, bottom-left).
void SandboxApp::drawGraphPrintOverlay(ImVec2 vpMin, ImVec2 vpMax) {
    const f64 now = ImGui::GetTime();
    std::vector<std::pair<std::string, f32>> visible;
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        for (auto& gp : graphPrints_) {
            // Stamped on first sight (not when logged) so minimized window doesn't expire before viewing.
            if (gp.at < 0.0) gp.at = now;
            const f64 age = now - gp.at;
            if (age > kGraphPrintHoldSec) continue;
            const f64 fadeStart = kGraphPrintHoldSec - kGraphPrintFadeSec;
            const f32 a = age <= fadeStart
                ? 1.0f
                : static_cast<f32>(1.0 - (age - fadeStart) / kGraphPrintFadeSec);
            visible.emplace_back(gp.count > 1 ? gp.text + "  (x" + std::to_string(gp.count) + ")"
                                              : gp.text,
                                 a);
        }
        // Reap in loop, not sink: line stays visible until viewed once.
        while (!graphPrints_.empty() && graphPrints_.front().at >= 0.0 &&
               now - graphPrints_.front().at > kGraphPrintHoldSec)
            graphPrints_.pop_front();
    }
    if (visible.empty()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 pad = 8.0f * dpi_;
    const f32 lineH = ImGui::GetTextLineHeight() + 2.0f * dpi_;
    f32 y = vpMax.y - pad - lineH * static_cast<f32>(visible.size());
    for (const auto& [text, alpha] : visible) {
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        // Backing plate: white text on 3D background is unreadable.
        dl->AddRectFilled(ImVec2(vpMin.x + pad - 4.0f * dpi_, y - 1.0f * dpi_),
                          ImVec2(vpMin.x + pad + sz.x + 4.0f * dpi_, y + lineH - 1.0f * dpi_),
                          IM_COL32(0, 0, 0, static_cast<int>(150.0f * alpha)), 3.0f * dpi_);
        dl->AddText(ImVec2(vpMin.x + pad, y),
                    IM_COL32(150, 230, 255, static_cast<int>(255.0f * alpha)), text.c_str());
        y += lineH;
    }
}

// Output Log: clear / level filter / auto-scroll, under logMutex_.
// Shared severity palette (Output Log and Console): prevents drift.
 void SandboxApp::logLineStyle(LogLevel l, ImVec4& text, ImVec4& row, bool& filled) {
    filled = false;
    row    = ImVec4(0, 0, 0, 0);
    switch (l) {
        case LogLevel::Fatal:
            text   = ImVec4(0.04f, 0.04f, 0.05f, 1.0f);
            row    = ImVec4(0.86f, 0.16f, 0.13f, 1.0f);
            filled = true;
            break;
        case LogLevel::Critical:
            text   = ImVec4(0.86f, 0.20f, 0.17f, 1.0f);
            row    = ImVec4(0.34f, 0.05f, 0.05f, 0.55f);
            filled = true;
            break;
        case LogLevel::Error: text = ImVec4(0.95f, 0.30f, 0.28f, 1.0f); break;
        case LogLevel::Warn:  text = ImVec4(1.00f, 0.62f, 0.15f, 1.0f); break;
        case LogLevel::Trace: text = ImVec4(0.55f, 0.57f, 0.62f, 1.0f); break;
        default:              text = ImVec4(0.82f, 0.84f, 0.88f, 1.0f); break;
    }
}

// Draws one line in its severity's style, with row background for Fatal/Critical.
 void SandboxApp::drawLogLine(LogLevel level, const char* text) {
    ImVec4 fg, bg;
    bool filled = false;
    logLineStyle(level, fg, bg, filled);
    if (filled) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        const float  h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                                                  ImGui::ColorConvertFloat4ToU32(bg));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void SandboxApp::drawOutputLog() {
    if (ImGui::SmallButton("Clear")) { std::lock_guard<std::mutex> lk(logMutex_); logLines_.clear(); logMultiRowCount_ = 0; }
    ImGui::SameLine();
    // Text is not selectable; log is frequently needed.
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            for (const LogLine& ln : logLines_) { all += ln.text; all += char(10); }
        }
        ImGui::SetClipboardText(all.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the whole log to the clipboard");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * dpi_);
    // Appended only; order is persisted in editor.ini.
    ImGui::Combo("##loglevel", &logLevelFilter_, "All\0Info+\0Warn+\0Error+\0Critical+\0");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::Separator();

    ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        const int minLevel = logLevelFilter_ == 4 ? (int)LogLevel::Critical
                          : logLevelFilter_ == 3 ? (int)LogLevel::Error
                          : logLevelFilter_ == 2 ? (int)LogLevel::Warn
                          : logLevelFilter_ == 1 ? (int)LogLevel::Info : (int)LogLevel::Trace;
        // Only visible rows submitted: ImGuiListClipper, or spacers over summed row heights once
        // any multi-row line exists.
        std::vector<u32> shown;
        shown.reserve(logLines_.size());
        for (usize i = 0; i < logLines_.size(); ++i)
            if ((int)logLines_[i].level >= minLevel) shown.push_back(static_cast<u32>(i));
        if (logMultiRowCount_ != 0) {
            const f32 lineH = ImGui::GetTextLineHeight();
            const f32 spacing = ImGui::GetStyle().ItemSpacing.y;
            // top[k] = y of row k below the first row, each height floored as ImGui snaps its cursor.
            std::vector<f32> top(shown.size() + 1, 0.0f);
            for (usize k = 0; k < shown.size(); ++k)
                top[k + 1] = top[k] + std::floor(static_cast<f32>(logLines_[shown[k]].rows) * lineH + spacing);
            const ImDrawList* dl = ImGui::GetWindowDrawList();
            const f32 y0 = ImGui::GetCursorScreenPos().y;
            const f32 visTop = dl->GetClipRectMin().y - y0;
            const f32 visBot = dl->GetClipRectMax().y - y0;
            const usize n = shown.size();
            const usize first = static_cast<usize>(
                std::max<std::ptrdiff_t>(0, std::upper_bound(top.begin(), top.end(), visTop) - top.begin() - 1));
            const usize last = std::max(first, static_cast<usize>(
                std::lower_bound(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(n), visBot) - top.begin()));
            if (first > 0) ImGui::Dummy(ImVec2(0.0f, top[first] - spacing));
            for (usize k = first; k < last; ++k)
                drawLogLine(logLines_[shown[k]].level, logLines_[shown[k]].text.c_str());
            if (last < n) ImGui::Dummy(ImVec2(0.0f, top[n] - top[last] - spacing));
        } else {
            // Floor of line height with spacing (for DPI scale agreement).
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()),
                          std::floor(ImGui::GetTextLineHeightWithSpacing()));
            while (clipper.Step())
                for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                    const LogLine& ln = logLines_[shown[static_cast<usize>(k)]];
                    drawLogLine(ln.level, ln.text.c_str());
                }
        }
    }
    if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// Truncates to maxLen, appending "..." -- full text shown in tooltip.
 std::string SandboxApp::elide(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen > 3 ? maxLen - 3 : 0) + "...";
}

// Live suggestions for consoleInput_: substring match on command/variable names.
std::vector<SandboxApp::ConsoleSuggestion> SandboxApp::computeConsoleSuggestions() const {
    std::vector<ConsoleSuggestion> out;
    const std::string_view sv(consoleInput_);
    if (sv.empty() || std::isspace(static_cast<unsigned char>(sv.back()))) return out;

    std::vector<std::string_view> words;
    for (std::size_t i = 0; i < sv.size();) {
        while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
        const std::size_t start = i;
        while (i < sv.size() && !std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
        if (i > start) words.push_back(sv.substr(start, i - start));
    }
    if (words.empty()) return out;
    const std::string_view partial = words.back();

    if (words.size() == 1) {
        for (const editor::ConsoleCommandDesc& c : editor::consoleCatalog()) {
            if (!editor::commandMatchesQuery(c, partial)) continue;
            out.push_back({c.name + "  -- " + elide(c.help, 70), c.name, c.help});
            if (out.size() >= 6) break;
        }
    }
    // Variable names fill the rest of the list.
    if (out.size() < 8) {
        // Shortest match first (most general name wins; ties break on name).
        std::vector<const editor::ConsoleVar*> hits;
        for (const editor::ConsoleVar& v : editor::allVars())
            if (editor::varMatchesQuery(v, partial)) hits.push_back(&v);
        std::sort(hits.begin(), hits.end(), [](const editor::ConsoleVar* a, const editor::ConsoleVar* b) {
            if (a->name.size() != b->name.size()) return a->name.size() < b->name.size();
            return a->name < b->name;
        });
        for (const editor::ConsoleVar* v : hits) {
            std::string disp = v->name + "  =  " + editor::formatValue(v->read());
            if (v->readOnly) disp += " (ro)";
            disp += "  -- " + elide(v->help, 60);
            out.push_back({std::move(disp), v->name, editor::varTooltipText(*v)});
            if (out.size() >= 8) break;
        }
    }
    return out;
}

// Seeds input and arms focus latch (toggleDrawer behavior).
void SandboxApp::seedConsoleInput(const std::string& line) {
    std::strncpy(consoleInput_, line.c_str(), sizeof(consoleInput_) - 1);
    consoleInput_[sizeof(consoleInput_) - 1] = '\0';
    consoleFocusPending_ = true;
}

// Completes last word, keeping prefix.
void SandboxApp::acceptConsoleSuggestion(const std::string& insertText) {
    const std::string cur(consoleInput_);
    const std::size_t lastSpace = cur.find_last_of(" \t");
    const std::string prefix = lastSpace == std::string::npos ? std::string() : cur.substr(0, lastSpace + 1);
    seedConsoleInput(prefix + insertText + " ");
}

// Splits on dots to enable per-token hovers (var names are dotted).
void SandboxApp::drawConsoleTranscriptLine(LogLevel level, const std::string& text) {
    ImGui::BeginGroup();
    if (text.find('.') == std::string::npos) {
        drawLogLine(level, text.c_str());
    } else {
        ImVec4 fg, bg; bool filled = false;
        logLineStyle(level, fg, bg, filled);
        if (filled) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                                                      ImGui::ColorConvertFloat4ToU32(bg));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, fg);
        bool any = false;
        auto emit = [&](const std::string& s) {
            if (s.empty()) return;
            if (any) ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(s.c_str());
            any = true;
        };
        std::size_t lastEnd = 0, i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            const std::size_t wordStart = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (wordStart == i) break;
            const std::string word = text.substr(wordStart, i - wordStart);
            if (word.find('.') != std::string::npos) {
                std::string trimmed = word;
                while (!trimmed.empty() &&
                       (trimmed.back() == ':' || trimmed.back() == ',' || trimmed.back() == '='))
                    trimmed.pop_back();
                if (const editor::ConsoleVar* v = editor::findVar(trimmed)) {
                    emit(text.substr(lastEnd, wordStart - lastEnd));
                    emit(word);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", editor::varTooltipText(*v).c_str());
                    lastEnd = i;
                }
            }
        }
        emit(text.substr(lastEnd));
        if (!any) ImGui::TextUnformatted("");
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}

// Draws variable row: name, value, tags, trimmed description.
void SandboxApp::drawConsoleVarRow(const editor::ConsoleVar& v) {
    ImGui::PushID(v.name.c_str());
    std::string row = v.name + "  =  " + editor::formatValue(v.read());
    if (v.readOnly) row += "   (read-only)";
    const char* tag = editor::varHonestyTag(editor::varHonesty(v));
    if (*tag) { row += "   "; row += tag; }
    if (!v.help.empty()) { row += "  -- "; row += elide(v.help, 100); }
    if (ImGui::Selectable(row.c_str()))
        seedConsoleInput((v.readOnly ? std::string("get ") : std::string("set ")) + v.name + " ");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", editor::varTooltipText(v).c_str());
    ImGui::PopID();
}

// REPL: transcript, suggestions, input line.
void SandboxApp::drawConsoleTranscriptTab(Engine& e) {
    if (ImGui::SmallButton("Clear")) consoleLines_.clear();
    ImGui::SameLine();
    // Copy, since text can't be selected (colored text loses drag-selection).
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (const LogLine& ln : consoleLines_) { all += ln.text; all += char(10); }
        ImGui::SetClipboardText(all.c_str());
        consoleLines_.push_back({LogLevel::Info,
                                 "[console] copied " + std::to_string(consoleLines_.size()) +
                                 " line(s) to the clipboard"});
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy every line here to the clipboard");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &consoleAutoScroll_);
    ImGui::Separator();

    // Welcome lines: orientation for first-time users.
    if (consoleLines_.empty())
        for (const std::string& line : editor::consoleWelcomeLines())
            consoleLines_.push_back({LogLevel::Info, line});

    // Pre-fills input for screenshot capture (--drawer console:<seed>).
    if (!drawerStartSub_.empty()) {
        std::strncpy(consoleInput_, drawerStartSub_.c_str(), sizeof(consoleInput_) - 1);
        consoleInput_[sizeof(consoleInput_) - 1] = '\0';
        drawerStartSub_.clear();
    }

    // Compute suggestions before layout to know height.
    const std::vector<ConsoleSuggestion> suggestions = computeConsoleSuggestions();
    const f32 inputLineH = ImGui::GetFrameHeightWithSpacing();
    const f32 suggestRowH = ImGui::GetTextLineHeightWithSpacing();
    const f32 suggestH = suggestions.empty() ? 0.0f
        : std::min<f32>(static_cast<f32>(suggestions.size()), 6.0f) * suggestRowH + 8.0f * dpi_;

    ImGui::BeginChild("##consolescroll", ImVec2(0, -(inputLineH + suggestH)), false,
                       ImGuiWindowFlags_HorizontalScrollbar);
    {
        for (const LogLine& ln : consoleLines_) {
            drawConsoleTranscriptLine(ln.level, ln.text);
            // Right-click to copy single line.
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::SetClipboardText(ln.text.c_str());
        }
    }
    if (consoleAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (!suggestions.empty()) {
        ImGui::BeginChild("##consolesuggest", ImVec2(0, suggestH), true);
        for (std::size_t i = 0; i < suggestions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool clicked = ImGui::Selectable(suggestions[i].display.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", suggestions[i].tooltip.c_str());
            if (clicked) acceptConsoleSuggestion(suggestions[i].insert);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (consoleFocusPending_) { ImGui::SetKeyboardFocusHere(); consoleFocusPending_ = false; }
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputText("##consoleinput", consoleInput_, sizeof(consoleInput_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_CallbackCompletion, &SandboxApp::consoleInputCallback, this);
    if (submitted && consoleInput_[0] != '\0') {
        runConsoleLine(e, consoleInput_);
        consoleHistory_.emplace_back(consoleInput_);
        consoleHistoryPos_ = -1;
        consoleInput_[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// Variable browser: filtered by name/description, grouped by prefix.
void SandboxApp::drawConsoleBrowserTab() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##consolebrowsefilter", "Filter by name or description (e.g. \"gi\", \"exposure\", \"read-only\")...",
                              consoleBrowseFilter_, sizeof(consoleBrowseFilter_));
    ImGui::Separator();

    ImGui::BeginChild("##consolevarlist", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()));
    std::string lastGroup;
    std::size_t shown = 0, total = 0;
    for (const editor::ConsoleVar& v : editor::allVars()) {
        ++total;
        if (!editor::varMatchesQuery(v, consoleBrowseFilter_)) continue;
        const std::string group = editor::varGroupKey(v.name);
        if (group != lastGroup) {
            if (!lastGroup.empty()) ImGui::Spacing();
            ImGui::TextDisabled("%s", editor::varGroupLabel(group).c_str());
            lastGroup = group;
        }
        drawConsoleVarRow(v);
        ++shown;
    }
    if (shown == 0) ImGui::TextDisabled("No variable's name or description matches '%s'.", consoleBrowseFilter_);
    ImGui::EndChild();

    // Two variable groups omitted from browsing; see EditorConsole.hpp.
    ImGui::TextDisabled("%zu of %zu variables shown. Click a row to copy a ready-to-run line to the console input.", shown, total);
}

// Console drawer tabs: Transcript (forced front on focus) and Browse Variables.
void SandboxApp::drawConsole(Engine& e) {
    // Device passed to post.* variables (ConsoleVar::read takes no args).
    editor::setConsoleDevice(e.device());

    if (ImGui::BeginTabBar("##consoleTabs")) {
        const ImGuiTabItemFlags replFlags =
            consoleFocusPending_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Transcript", nullptr, replFlags)) {
            drawConsoleTranscriptTab(e);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Browse Variables")) {
            drawConsoleBrowserTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Parses and dispatches commands; no quoting support.
void SandboxApp::runConsoleLine(Engine& e, const std::string& line) {
    consoleLines_.push_back({LogLevel::Info, "> " + line});
    if (consoleLines_.size() > kMaxConsoleLines) consoleLines_.pop_front();

    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : line) {
            if (std::isspace(static_cast<unsigned char>(c))) { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    if (tokens.empty()) return;

    const editor::ConsoleCommandDesc* cmd = editor::findCommand(tokens[0]);
    auto print = [this](LogLevel lvl, std::string text) {
        consoleLines_.push_back({lvl, std::move(text)});
        if (consoleLines_.size() > kMaxConsoleLines) consoleLines_.pop_front();
    };
    if (!cmd) {
        print(LogLevel::Error, "Unknown command '" + tokens[0] + "'. Type 'help' for a list.");
        return;
    }
    const std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    cmd->handler(*this, e, args, print);
}

// ImGui InputText callback: Up/Down walks history, Tab completes command name.
 int SandboxApp::consoleInputCallback(ImGuiInputTextCallbackData* data) {
    SandboxApp* self = static_cast<SandboxApp*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        // consoleHistory_ is most-recent-last; Up/Down walk older/newer.
        if (self->consoleHistory_.empty()) return 0;
        const int last = static_cast<int>(self->consoleHistory_.size()) - 1;
        int pos = self->consoleHistoryPos_;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (pos < 0) pos = last;
            else if (pos > 0) --pos;
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (pos < 0) return 0;
            ++pos;
            if (pos > last) {
                self->consoleHistoryPos_ = -1;
                data->DeleteChars(0, data->BufTextLen);
                return 0;
            }
        } else {
            return 0;
        }
        self->consoleHistoryPos_ = pos;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, self->consoleHistory_[static_cast<size_t>(pos)].c_str());
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // v1 completes command names only, not args.
        const char* bufStart = data->Buf;
        const char* wordEnd = data->Buf + data->CursorPos;
        const char* wordStart = wordEnd;
        while (wordStart > bufStart && !std::isspace(static_cast<unsigned char>(wordStart[-1]))) --wordStart;
        if (wordStart != bufStart) return 0;
        const std::string_view prefix(wordStart, static_cast<size_t>(wordEnd - wordStart));
        std::vector<const editor::ConsoleCommandDesc*> matches;
        for (const editor::ConsoleCommandDesc& c : editor::consoleCatalog())
            if (c.name.size() >= prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), c.name.begin(),
                           [](char a, char b){ return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
                matches.push_back(&c);
        if (matches.size() == 1) {
            data->DeleteChars(static_cast<int>(wordStart - bufStart), static_cast<int>(wordEnd - wordStart));
            data->InsertChars(data->CursorPos, matches[0]->name.c_str());
            data->InsertChars(data->CursorPos, " ");
        } else if (matches.size() > 1) {
            std::string list = "Candidates:";
            for (const editor::ConsoleCommandDesc* m : matches) list += " " + m->name;
            self->consoleLines_.push_back({LogLevel::Info, list});
            if (self->consoleLines_.size() > kMaxConsoleLines) self->consoleLines_.pop_front();
        }
    }
    return 0;
}

// Draws the bottom drawer, sliding Content Browser or Output Log up over viewport.
void SandboxApp::drawDrawer(Engine& e) {
    const f32 dt = std::fmin(e.time().dt, 0.05f);
    const f32 target = drawer_ == Drawer::None ? 0.0f : 1.0f;
    drawerAnim_ += (target - drawerAnim_) * (1.0f - std::exp(-drawerRate_ * dt));
    if (drawer_ != Drawer::None) drawerShown_ = drawer_;
    if (drawer_ == Drawer::None && drawerAnim_ < 0.004f) { drawerAnim_ = 0.0f; drawerPixelH_ = 0.0f; return; }

    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize;
    const f32 statusH = 26.0f * dpi_;
    const f32 fullH = (wsize.y - statusH) * drawerFrac_;
    const f32 h = fullH * drawerAnim_;
    // Published for viewport hint (prevents drawing through drawer).
    drawerPixelH_ = h;

    ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH - h));
    ImGui::SetNextWindowSize(ImVec2(wsize.x, h));
    if (drawerRaise_) { ImGui::SetNextWindowFocus(); drawerRaise_ = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##drawer", nullptr, kDrawerFlags);

    // Top edge is a resize grip, claimed before anything else is submitted.
    const f32 gripH = 5.0f * dpi_;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##drawergrip", ImVec2(std::fmax(wsize.x, 1.0f), gripH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && wsize.y > statusH + 1.0f) {
        drawerFrac_ -= ImGui::GetIO().MouseDelta.y / (wsize.y - statusH);
        drawerFrac_ = std::fmin(0.88f, std::fmax(0.14f, drawerFrac_));
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, gripH + ImGui::GetStyle().WindowPadding.y));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, ImGui::GetWindowPos().y),
                                        ImVec2(wpos.x + wsize.x, ImGui::GetWindowPos().y),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

    if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
    ImGui::TextUnformatted(drawerShown_ == Drawer::Log ? "Output Log" :
                            drawerShown_ == Drawer::Console ? "Console" : "Content Browser");
    if (fontMedium_) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled(drawerShown_ == Drawer::Content ? "(Ctrl+Space or Esc to dismiss)" :
                         drawerShown_ == Drawer::Console ? "(` or Esc to dismiss)" : "(Esc to dismiss)");
    ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 34.0f * dpi_));
    if (ImGui::Button("X")) drawer_ = Drawer::None;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the drawer");
    ImGui::Separator();

    if (ImGui::GetContentRegionAvail().y > ImGui::GetFrameHeight()) {
        if (drawerShown_ == Drawer::Log)          drawOutputLog();
        else if (drawerShown_ == Drawer::Console) drawConsole(e);
        else                                       drawContentBrowser();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Opens a drawer, or closes it if it is already the one showing.
void SandboxApp::toggleDrawer(Drawer d) {
    drawer_ = (drawer_ == d) ? Drawer::None : d;
    if (drawer_ != Drawer::None) drawerRaise_ = true;
    // Arms console focus latch (window doesn't 'appear' when switching tabs).
    if (drawer_ == Drawer::Console) consoleFocusPending_ = true;
}

// Reuses Output Log/Console palette (avoid drift).
 ImVec4 SandboxApp::notifyColour(editor::NotifySeverity s) {
    switch (s) {
        case editor::NotifySeverity::Success:  return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
        case editor::NotifySeverity::Warning:  return ImVec4(1.00f, 0.62f, 0.15f, 1.0f);
        case editor::NotifySeverity::Error:    return ImVec4(0.95f, 0.30f, 0.28f, 1.0f);
        case editor::NotifySeverity::Critical: return ImVec4(0.86f, 0.20f, 0.17f, 1.0f);
        default:                               return ImVec4(0.82f, 0.84f, 0.88f, 1.0f);
    }
}

// Draws the notification stack, bottom-right, newest nearest the corner.
void SandboxApp::drawNotifications() {
    // --frames (measurement mode): skip toasts.
    if (maxFrames_ != 0 && !notifyTestLift_) return;

    editor::NotificationQueue& q = editor::notifications();
    static std::vector<editor::Notification> shown;
    usize hidden = 0;
    q.tick(ImGui::GetTime(), shown, hidden);
    if (shown.empty() && hidden == 0) return;

    // Own windows (separate Begin) for correct z-order and hit-testing.
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    const f32 pad     = 8.0f * dpi_;
    const f32 statusH = 26.0f * dpi_;
    const f32 width   = 340.0f * dpi_;
    const f32 anchorX = mv->WorkPos.x + mv->WorkSize.x - pad;
    // Above status bar and drawer; drawerPixelH_ is current frame.
    f32 y = mv->WorkPos.y + mv->WorkSize.y - statusH - drawerPixelH_ - pad;

    const ImGuiWindowFlags base =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;

    // Newest first (bottom-right pivot): older ones need stored heights.
    static std::unordered_map<u64, f32> heights;
    for (usize i = shown.size(); i-- > 0;) {
        const editor::Notification& n = shown[i];
        const bool interactive = n.actions[0] != editor::NotifyAction::None;

        ImGui::SetNextWindowPos(ImVec2(anchorX, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        // Opaque (legibility over Details panel).
        ImGui::SetNextWindowBgAlpha(1.0f);

        char id[32];
        std::snprintf(id, sizeof id, "##notify_%llu", static_cast<unsigned long long>(n.id));
        if (ImGui::Begin(id, nullptr, interactive ? base : (base | ImGuiWindowFlags_NoInputs))) {
            if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
            ImGui::TextColored(notifyColour(n.severity), "%s", n.title.c_str());
            if (fontMedium_) ImGui::PopFont();
            if (n.count > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("x%u", n.count);
            }
            if (!n.body.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(n.body.c_str());
                ImGui::PopTextWrapPos();
            }
            if (n.hasProgress) {
                // Negative progress = indeterminate; no text overlay.
                ImGui::ProgressBar(n.progress, ImVec2(-FLT_MIN, 6.0f * dpi_), "");
                if (!n.progressNote.empty()) ImGui::TextDisabled("%s", n.progressNote.c_str());
            }
            for (int a = 0; a < 2; ++a) {
                if (n.actions[a] == editor::NotifyAction::None) continue;
                if (a) ImGui::SameLine();
                ImGui::PushID(a);
                if (ImGui::SmallButton(n.actionLabels[a].c_str())) q.activate(n.id, n.actions[a]);
                ImGui::PopID();
            }
            heights[n.id] = ImGui::GetWindowSize().y;
        }
        ImGui::End();

        const auto it = heights.find(n.id);
        y -= (it == heights.end() ? 48.0f * dpi_ : it->second) + pad * 0.5f;
    }

    if (hidden > 0) {
        ImGui::SetNextWindowPos(ImVec2(anchorX, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##notify_more", nullptr, base | ImGuiWindowFlags_NoInputs))
            ImGui::TextDisabled("+%zu more", hidden);
        ImGui::End();
    }

    // Drain activations after iteration (prevent concurrent modification).
    u64 actId = 0;
    editor::NotifyAction act = editor::NotifyAction::None;
    while (q.drainActivation(actId, act)) {
        switch (act) {
            case editor::NotifyAction::ShowOutputLog:
                // Open (not toggle) to avoid closing if already open.
                drawer_ = Drawer::Log;
                drawerRaise_ = true;
                break;
            case editor::NotifyAction::Dismiss:          q.dismiss(actId); break;
            case editor::NotifyAction::PostponeAutosave: autosavePostponeRequested_ = true; break;
            case editor::NotifyAction::RetryAutosave:    autosaveRetryRequested_ = true; break;
            case editor::NotifyAction::CancelNeuralTraining: nrd2Session_.cancel(); break;
            default: break;
        }
    }
    // Forget heights for notifications that are gone (prevent unbounded growth).
    if (heights.size() > editor::NotificationQueue::kMaxLive * 4) heights.clear();
}

#endif

} // namespace aver
