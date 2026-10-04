#pragma once
// Where the editor's widgets are, by name, so automation resolves a name to a rect instead of
// guessing a coordinate.
#if AVER_WITH_IMGUI
#include "imgui.h"

#include "aver/core/Types.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace aver::editor {

// One named widget rect.
struct TrackedWidget {
    std::string name;
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // screen pixels, the space a click uses
};

// Collects named widget rects for a frame and publishes them whole. Reads are thread-safe.
class UiRegistry {
public:
    // Starts a frame's UI build.
    void beginFrame() { building_.clear(); }

    // Records the last item submitted to ImGui under `name`. Call immediately after the widget.
    void track(const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        if (b.x - a.x < 1.0f || b.y - a.y < 1.0f) return;
        building_.push_back(TrackedWidget{name, a.x, a.y, b.x - a.x, b.y - a.y});
    }

    // Publishes the completed frame in one swap, so a reader never sees a partial UI.
    void endFrame() {
        std::lock_guard<std::mutex> lock(mutex_);
        published_.swap(building_);
    }

    // Resolves a name to the centre of its rect. Safe to call from any thread.
    bool centreOf(const std::string& name, f32& outX, f32& outY) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const TrackedWidget& t : published_) {
            if (t.name == name) {
                outX = t.x + t.w * 0.5f;
                outY = t.y + t.h * 0.5f;
                return true;
            }
        }
        return false;
    }

    // Everything drawn last frame, as "name=x,y,w,h" separated by spaces. An item inside a closed
    // menu is absent, because it is not on screen.
    std::string describe() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out;
        char buf[160];
        for (const TrackedWidget& t : published_) {
            std::snprintf(buf, sizeof buf, "%s%s=%.0f,%.0f,%.0f,%.0f",
                          out.empty() ? "" : " ", t.name.c_str(), t.x, t.y, t.w, t.h);
            out += buf;
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::vector<TrackedWidget> building_;    // this frame, main thread only
    std::vector<TrackedWidget> published_;   // last complete frame, read by anyone
};

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
