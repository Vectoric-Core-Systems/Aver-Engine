#pragma once
// The editor's "Compiling shaders N of M" notification, driven by VoxiRenderer's background pipeline builds
// (docs/rendering/ASYNC_SHADERS.md). While the scene set builds the viewport is blank; a later rebuild (a
// material graph, an edited shader) keeps drawing with the old pipelines and only shows the notification.
// ImGui-free: arithmetic over the queue, like ModeSwitchNotice.hpp.
#include "EditorNotifications.hpp"
#include "aver/core/Types.hpp"

#include <chrono>
#include <string>

namespace aver::editor {

class ShaderBuildNotice {
public:
    // Once a frame, on the main thread. `building`: a build is in flight (done of total requests finished).
    // `blank`: the viewport is blank until it lands.
    void poll(bool building, bool blank, u32 done, u32 total) {
        using Clock = std::chrono::steady_clock;
        NotificationQueue& q = notifications();
        if (building) {
            if (!since_.time_since_epoch().count()) since_ = Clock::now();
            // A rebuild that keeps drawing is only worth a notification if it is not instant.
            const bool slow = Clock::now() - since_ > std::chrono::milliseconds(750);
            if (!toast_ && (blank || slow)) {
                Notification n;
                n.severity = NotifySeverity::Info;
                n.title = blank ? "Compiling shaders" : "Rebuilding shaders";
                n.body = blank ? "The viewport shows the scene once its pipelines are ready; the editor stays usable."
                               : "The current pipelines keep drawing until the new ones are ready.";
                n.sticky = true;
                n.hasProgress = true;
                n.progress = -1.0f;
                n.dedupKey = "shader-build";
                toast_ = q.push(std::move(n));
            }
            if (toast_) {
                const f32 frac = total ? static_cast<f32>(done) / static_cast<f32>(total) : -1.0f;
                q.setProgress(toast_, frac,
                              "Compiling shaders " + std::to_string(done) + " of " + std::to_string(total));
            }
            return;
        }
        since_ = {};
        if (toast_) {
            q.setSticky(toast_, false);   // finish() leaves sticky set; it must fade
            q.finish(toast_, NotifySeverity::Success, "Shaders compiled", "", 2.5);
            toast_ = 0;
        }
    }

private:
    u64 toast_ = 0;
    std::chrono::steady_clock::time_point since_{};
};

}  // namespace aver::editor
