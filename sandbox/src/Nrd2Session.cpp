// Tools > Train Neural Denoiser... and --nrd2-train (Nrd2Session.hpp).

#include "Nrd2Session.hpp"

#include "EditorNotifications.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#if AVER_MODULE_VOXI
#include "aver/render/denoise/Nrd2Trainer.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aver::editor {

#if AVER_MODULE_VOXI

namespace dn = render::denoise;

namespace {

// A passive render feature: its prePass runs before the scene targets are bound, so the trainer's
// compute is legal on both backends. It draws nothing and claims nothing.
struct TrainerHook final : rhi::IRenderFeature {
    dn::Nrd2Trainer* trainer = nullptr;
    const char* name() const override { return "NRD2 trainer"; }
    void prePass(rhi::IRenderContext& ctx) override { if (trainer) trainer->step(ctx); }
};

std::string defaultDatasetDir() {
    const std::string d = userDataDir();
    return d.empty() ? std::string() : d + "\\nrd2_dataset";
}

std::string userWeightsPath() {
    const std::string d = userDataDir();
    return d.empty() ? std::string() : d + "\\" + dn::kNrd2WeightsFileName;
}

const char* phaseName(dn::Nrd2TrainStatus::Phase p) {
    using P = dn::Nrd2TrainStatus::Phase;
    switch (p) {
        case P::Idle:       return "Idle";
        case P::Loading:    return "Loading";
        case P::Training:   return "Training";
        case P::Validating: return "Validating";
        case P::Stopping:   return "Stopping";
        case P::Finished:   return "Finished";
        case P::Failed:     return "Failed";
        case P::Cancelled:  return "Cancelled";
    }
    return "?";
}

}  // namespace

struct Nrd2Session::Impl {
    rhi::IDevice* dev = nullptr;
    std::unique_ptr<dn::Nrd2Trainer> trainer;
    TrainerHook hook;
    bool hooked = false;
    u32 idleFrames = 0;
    bool open = false;
    bool pending = false;
    dn::Nrd2TrainConfig pendingCfg;
    u64 toast = 0;
    bool toastOpen = false;

    char dir[512] = {};
    int steps = 30000, vramMiB = 1536;
    float gpuMs = 8.0f;
    bool resume = true;
    bool scanned = false;
    std::vector<dn::Nrd2PoseInfo> scan;
    std::vector<std::string> problems;

    Impl() {
        const std::string d = defaultDatasetDir();
        std::snprintf(dir, sizeof dir, "%s", d.c_str());
    }

    dn::Nrd2TrainConfig config() const {
        dn::Nrd2TrainConfig c;
        c.dirs = {dir};
        c.weightsPath = userWeightsPath();
        c.steps = static_cast<u32>(std::max(steps, 1));
        c.vramBudget = static_cast<u64>(std::clamp(vramMiB, 64, 16384)) << 20;
        c.gpuBudgetMs = std::clamp(gpuMs, 0.5f, 100.0f);
        c.resume = resume;
        return c;
    }

    void rescan() {
        problems.clear();
        scan = dn::scanNrd2Dataset({dir}, &problems);
        for (const std::string& p : problems) AVER_WARN("[NRD2] dataset: skipped {}", p);
        scanned = true;
    }

    void start(const dn::Nrd2TrainConfig& cfg) {
        if (!dev || !dev->resources()) return;
        if (!trainer) trainer = std::make_unique<dn::Nrd2Trainer>(*dev);
        if (trainer->active()) return;
        if (!trainer->start(cfg)) {
            Notification n;
            n.severity = NotifySeverity::Error;
            n.title = "Neural denoiser training did not start";
            n.body = "See the Output Log.";
            n.actions[0] = NotifyAction::ShowOutputLog;
            n.actionLabels[0] = "Output Log";
            notifications().push(std::move(n));
            return;
        }
        hook.trainer = trainer.get();
        if (!hooked) {
            dev->addRenderFeature(&hook);
            hooked = true;
        }
        idleFrames = 0;
        Notification n;
        n.severity = NotifySeverity::Info;
        n.title = "Training the neural denoiser";
        n.body = std::to_string(cfg.steps) + " steps; weights " + cfg.weightsPath;
        n.sticky = true;
        n.hasProgress = true;
        n.progress = 0.0f;
        n.progressNote = "loading poses";
        n.actions[0] = NotifyAction::CancelNeuralTraining;
        n.actionLabels[0] = "Cancel";
        toast = notifications().push(std::move(n));
        toastOpen = true;
    }
};

Nrd2Session::Nrd2Session() : impl_(std::make_unique<Impl>()) {}

Nrd2Session::~Nrd2Session() {
    // shutdown() normally ran while the device was alive. If it did not, the trainer's GPU objects and its
    // worker thread cannot be released safely now: leak them rather than touch a destroyed device.
    if (impl_ && impl_->trainer) (void)impl_->trainer.release();
}

bool Nrd2Session::available() const { return true; }

void Nrd2Session::openWindow() {
    impl_->open = true;
    impl_->scanned = false;
}

void Nrd2Session::requestCli(u32 steps, std::vector<std::string> dirs) {
    dn::Nrd2TrainConfig c = impl_->config();
    c.steps = std::max(steps, 1u);
    if (!dirs.empty()) c.dirs = std::move(dirs);
    impl_->pendingCfg = std::move(c);
    impl_->pending = true;
    AVER_INFO("[NRD2] --nrd2-train: {} steps, starting once the device is up", steps);
}

bool Nrd2Session::active() const { return impl_->trainer && impl_->trainer->active(); }

void Nrd2Session::cancel() {
    if (impl_->trainer && impl_->trainer->active()) {
        impl_->trainer->cancel();
        AVER_INFO("[NRD2] training cancel requested; saving a checkpoint");
    }
}

void Nrd2Session::tick(rhi::IDevice* dev) {
    Impl& m = *impl_;
    if (!dev) return;
    if (!m.dev) m.dev = dev;
    if (m.pending && dev->resources()) {
        m.pending = false;
        m.start(m.pendingCfg);
    }
    if (!m.trainer) return;
    const dn::Nrd2TrainStatus st = m.trainer->status();
    const bool running = m.trainer->active();
    if (running) {
        m.idleFrames = 0;
        if (m.toastOpen) {
            char note[160];
            if (st.phase == dn::Nrd2TrainStatus::Phase::Loading)
                std::snprintf(note, sizeof note, "%s", st.message.c_str());
            else if (st.bestRatio >= 0.0f)
                std::snprintf(note, sizeof note, "step %u / %u, held-out ratio %.3f (best %.3f)", st.sessionSteps,
                              st.targetSteps, static_cast<double>(st.lastRatio), static_cast<double>(st.bestRatio));
            else
                std::snprintf(note, sizeof note, "step %u / %u", st.sessionSteps, st.targetSteps);
            notifications().setProgress(m.toast, st.phase == dn::Nrd2TrainStatus::Phase::Loading ? -1.0f : st.progress,
                                        note);
        }
        return;
    }
    // The trainer frees its GPU side on the step after it ends; keep the hook a few frames longer.
    if (m.hooked && ++m.idleFrames >= 3) {
        m.dev->removeRenderFeature(&m.hook);
        m.hooked = false;
    }
    if (m.toastOpen) {
        m.toastOpen = false;
        using P = dn::Nrd2TrainStatus::Phase;
        const NotifySeverity sev = st.phase == P::Finished ? NotifySeverity::Success
                                 : st.phase == P::Cancelled ? NotifySeverity::Warning
                                                            : NotifySeverity::Error;
        char body[256];
        std::snprintf(body, sizeof body, "%s. %u steps (%llu lifetime); best held-out ratio %.3f%s", st.message.c_str(),
                      st.sessionSteps, static_cast<unsigned long long>(st.lifetimeSteps),
                      static_cast<double>(st.bestRatio),
                      st.bestRatio >= 0.0f && st.bestRatio <= dn::kNrd2GateOpen ? " - the renderer uses it" : "");
        notifications().finish(m.toast, sev,
                               st.phase == P::Finished ? "Neural denoiser trained"
                               : st.phase == P::Cancelled ? "Neural denoiser training cancelled"
                                                          : "Neural denoiser training failed",
                               body, 12.0);
        notifications().setActions(m.toast, NotifyAction::None, "", NotifyAction::None, "");
    }
}

void Nrd2Session::shutdown() {
    Impl& m = *impl_;
    if (!m.trainer) return;
    m.trainer->saveOnExit();
    if (m.hooked && m.dev) m.dev->removeRenderFeature(&m.hook);
    m.hooked = false;
    m.trainer.reset();
}

void Nrd2Session::drawWindow(f32 dpi) {
#if AVER_WITH_IMGUI
    Impl& m = *impl_;
    if (!m.open) return;
    ImGui::SetNextWindowSize(ImVec2(600.0f * dpi, 560.0f * dpi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Train Neural Denoiser", &m.open)) {
        ImGui::End();
        return;
    }
    if (!m.scanned) m.rescan();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "Trains NRD2's network to predict the per-tile filter parameters the oracle fitted for each captured "
        "pose (--nrd2-capture). It never scores the filtered image: training, validation and the live gate all "
        "compare parameters. The renderer switches to the network once its held-out error is at most 0.80 of "
        "the default parameters' (voxi.nrd2Network).");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::InputText("Dataset folder", m.dir, sizeof m.dir);
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) m.rescan();
    if (m.scan.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "No pose files here. Capture them with --nrd2-capture.");
    } else {
        std::vector<std::pair<std::string, std::pair<u32, u32>>> scenes;
        for (const dn::Nrd2PoseInfo& p : m.scan) {
            if (scenes.empty() || scenes.back().first != p.scene) scenes.push_back({p.scene, {0, 0}});
            ++scenes.back().second.first;
            if (p.heldOut) ++scenes.back().second.second;
        }
        for (const auto& [name, n] : scenes)
            ImGui::BulletText("%s: %u poses, %u held out%s", name.c_str(), n.first, n.second,
                              n.second == 0 && n.first >= 2 ? " (none flagged: every 4th is held out)" : "");
    }
    if (!m.problems.empty()) ImGui::TextDisabled("%zu file(s) skipped (see the Output Log)", m.problems.size());

    const bool running = m.trainer && m.trainer->active();
    ImGui::BeginDisabled(running);
    ImGui::InputInt("Steps this session", &m.steps, 1000, 10000);
    m.steps = std::clamp(m.steps, 1, 10000000);
    ImGui::InputInt("Pose cache (MiB of VRAM)", &m.vramMiB, 128, 512);
    ImGui::InputFloat("GPU time per frame (ms)", &m.gpuMs, 1.0f, 4.0f, "%.1f");
    ImGui::Checkbox("Resume from the last checkpoint (same dataset only)", &m.resume);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(running || m.scan.empty() || !m.dev);
    if (ImGui::Button("Start training")) m.start(m.config());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel")) cancel();
    ImGui::EndDisabled();

    ImGui::Separator();
    if (m.trainer) {
        const dn::Nrd2TrainStatus st = m.trainer->status();
        ImGui::Text("%s: %s", phaseName(st.phase), st.message.c_str());
        ImGui::Text("Steps %u / %u this session, %llu lifetime; learning rate %.2e; %.1f steps a frame", st.sessionSteps,
                    st.targetSteps, static_cast<unsigned long long>(st.lifetimeSteps),
                    static_cast<double>(st.learningRate), static_cast<double>(st.stepsPerFrame));
        if (st.loss >= 0.0f) ImGui::Text("Training loss (standardised, weighted) %.4f", static_cast<double>(st.loss));
        if (st.evaluations > 0 || st.bestRatio >= 0.0f)
            ImGui::Text("Held-out error vs the defaults: last %.3f, best %.3f (%u checks, %u without improvement)",
                        static_cast<double>(st.lastRatio), static_cast<double>(st.bestRatio), st.evaluations,
                        st.sinceBest);
        for (const dn::Nrd2SceneRatio& s : st.scenes)
            if (s.poses) ImGui::BulletText("%s: %.3f over %u held-out poses", s.scene.c_str(), static_cast<double>(s.ratio), s.poses);
        if (st.trainPoses)
            ImGui::Text("Poses: %u training (%u resident), %u held out", st.trainPoses, st.residentPoses, st.heldOutPoses);
    }
    ImGui::TextDisabled("Weights: %s", userWeightsPath().c_str());
    ImGui::End();
#else
    (void)dpi;
#endif
}

#else  // !AVER_MODULE_VOXI

struct Nrd2Session::Impl {};
Nrd2Session::Nrd2Session() : impl_(std::make_unique<Impl>()) {}
Nrd2Session::~Nrd2Session() = default;
bool Nrd2Session::available() const { return false; }
void Nrd2Session::openWindow() {}
void Nrd2Session::requestCli(u32, std::vector<std::string>) {
    AVER_WARN("[NRD2] --nrd2-train needs the Voxi module; ignored");
}
void Nrd2Session::tick(rhi::IDevice*) {}
void Nrd2Session::drawWindow(f32) {}
void Nrd2Session::cancel() {}
void Nrd2Session::shutdown() {}
bool Nrd2Session::active() const { return false; }

#endif

}  // namespace aver::editor
