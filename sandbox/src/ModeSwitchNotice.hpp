#pragma once
// Announces a renderer mode switch (ReSTIR RT <-> ReSTIR PT, reference Path Tracing, primary visibility,
// denoiser) before the work that blocks the editor, and says when it is done.
//
// The switch builds pipelines and caches on the main thread. Applied the same frame the setting changes, the
// editor froze with nothing on screen. Now the change is held back one frame so the notification draws first,
// applied the next, and the notification finishes after the frame that rendered with it (the lazy pipeline
// builds land in that frame). Only interactive runs: a capture applies settings the frame they are set.
//
// PATH TRACING WAITS FOR THE SHADER WARM-UP: its pipelines (createPathTraceTwins, 13 heavy compute shaders) are
// built the first time it is switched on. Built on the main thread while the background warm-up had not reached
// them yet, they took minutes and Windows closed the editor as not responding. While the warm-up runs, the switch
// to Path Tracing is held (the editor keeps rendering, the other settings wait with it), the warm-up is raised to
// normal priority, and the notification shows its progress; once it is done the pipelines load from the cache.
#include "EditorNotifications.hpp"
#include "ShaderWarmup.hpp"
#include "aver/core/Types.hpp"
#include "aver/voxi/Voxi.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace aver::editor {

class ModeSwitchNotice {
public:
    struct Key {
        u32 pt = 0, ptMode = 0, giMode = 0, rtRenderMode = 0, denoiser = 0;
        bool operator==(const Key& o) const {
            return pt == o.pt && ptMode == o.ptMode && giMode == o.giMode && rtRenderMode == o.rtRenderMode &&
                   denoiser == o.denoiser;
        }
        bool operator!=(const Key& o) const { return !(*this == o); }
    };

    static Key keyOf(const voxi::Settings& s) {
        return {static_cast<u32>(s.pathTracing), s.ptMode, s.giMode, s.rtRenderMode, voxi::denoiserMode(s)};
    }

    static std::string describe(const voxi::Settings& s) {
        std::string mode = s.pathTracing != voxi::Quality::Off ? (s.ptMode == 1u ? "Path Tracing (reference)" : "ReSTIR PT")
                           : s.rtRenderMode == 0u                ? "raster"
                                                                 : "ReSTIR RT";
        static const char* kDenoiser[3] = {"no denoiser", "FidelityFX", "NRD2"};
        return mode + ", " + kDenoiser[voxi::denoiserMode(s) > 2u ? 2u : voxi::denoiserMode(s)];
    }

    // Called once a frame with the settings about to be applied. False: hold them back this frame.
    bool shouldApply(const voxi::Settings& s, bool interactive, ShaderWarmup* warm = nullptr) {
        using Clock = std::chrono::steady_clock;
        const Key k = keyOf(s);
        if (!inited_) { inited_ = true; applied_ = k; return true; }
        NotificationQueue& q = notifications();
        // Path Tracing switched on, or the light path changing (NRD2 builds the unified light shaders, FidelityFX and
        // None the legacy ones), while the warm-up still runs: hold everything until it is done.
        const bool ptOn       = k.pt != 0u && applied_.pt == 0u;
        const bool lightsFlip = (k.denoiser == 2u) != (applied_.denoiser == 2u);
        if (k != applied_ && interactive && (ptOn || lightsFlip) && warm && warm->running()) {
            warm->boost();
            std::string note;
            const float frac = warm->progress(note);
            if (!waitToast_) {
                Notification n;
                n.severity = NotifySeverity::Info;
                n.title = "Preparing " + describe(s);
                n.body = "Switches once its shaders are in the cache; the editor keeps running meanwhile.";
                n.sticky = true;
                n.hasProgress = true;
                n.progress = frac;
                n.dedupKey = "mode-switch-wait";
                waitToast_ = q.push(std::move(n));
            }
            q.setProgress(waitToast_, frac, note);
            return false;
        }
        if (waitToast_) {
            q.close(waitToast_);
            waitToast_ = 0;
        }
        if (stage_ == Stage::Measuring) {
            const double sec = std::chrono::duration<double>(Clock::now() - t0_).count();
            char body[64];
            std::snprintf(body, sizeof body, "Took %.1f s.", sec);
            q.setSticky(toast_, false);   // finish() leaves sticky set; it must fade
            q.finish(toast_, NotifySeverity::Success, describe(s) + " ready", body, 3.0);
            stage_ = Stage::Idle;
        }
        if (stage_ == Stage::Announced) {
            stage_ = Stage::Measuring;
            t0_ = Clock::now();
            applied_ = k;
            return true;
        }
        if (k != applied_ && interactive) {
            Notification n;
            n.severity = NotifySeverity::Info;
            n.title = "Switching to " + describe(s);
            n.body = "Building pipelines and caches; the editor pauses briefly.";
            n.sticky = true;
            n.hasProgress = true;
            n.progress = -1.0f;
            n.dedupKey = "mode-switch";
            toast_ = q.push(std::move(n));
            stage_ = Stage::Announced;
            return false;
        }
        applied_ = k;
        return true;
    }

private:
    enum class Stage : u8 { Idle, Announced, Measuring };
    bool inited_ = false;
    Stage stage_ = Stage::Idle;
    Key applied_{};
    u64 toast_ = 0;
    u64 waitToast_ = 0;   // "Preparing Path Tracing" while the warm-up finishes
    std::chrono::steady_clock::time_point t0_{};
};

}  // namespace aver::editor
