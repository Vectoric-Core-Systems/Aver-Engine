#pragma once
// Where the editor's widgets ARE, by name, so nothing outside has to guess a coordinate.
//
// WHY. Driving the editor by pixel coordinates read off a screenshot is fragile in three separate
// ways: the layout moves, the DPI scales, and a screenshot pixel is not necessarily an ImGui unit. The
// first attempt at forcing a click measured the tool buttons by hunting for their orange fill in a PNG,
// which is exactly the kind of thing that works once and then silently clicks the wrong button.
//
// So the editor publishes what it drew. `track("tool.rotate")` after a widget records its rect, and the
// control channel resolves a NAME to that rect. Coordinates never cross the wire.
//
// AND IT IS STILL A REAL CLICK. Resolving a name gives a point; the point is then clicked through the
// ordinary Win32 path, so the button is what gets tested rather than the handler behind it. A registry
// that invoked the handler directly would pass on a button that no longer exists.
//
// DOUBLE-BUFFERED, and that is not caution. The reader is another thread and the writer fills this over
// the course of a frame, so a lookup landing mid-build would see half a UI -- a name might be missing
// simply because its widget had not been drawn yet this frame, which is indistinguishable from the name
// being wrong. `beginFrame`/`endFrame` build into one set and publish it whole.
#if AVER_WITH_IMGUI
#include "imgui.h"

#include "aver/core/Types.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace aver::editor {

struct TrackedWidget {
    std::string name;
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // screen pixels, the space a click uses
};

class UiRegistry {
public:
    // Called at the top of the frame's UI build.
    void beginFrame() { building_.clear(); }

    // Record the LAST ITEM submitted to ImGui. Must be called immediately after the widget, which is
    // the same discipline IsItemHovered/IsItemClicked already require -- so it reads naturally at the
    // call site and cannot drift from the thing it names.
    void track(const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        // A zero-area item is a widget that was clipped or never drawn. Recorded anyway would mean
        // publishing a click target that cannot be hit.
        if (b.x - a.x < 1.0f || b.y - a.y < 1.0f) return;
        building_.push_back(TrackedWidget{name, a.x, a.y, b.x - a.x, b.y - a.y});
    }

    // Publish the completed frame. One swap under the lock, so a reader sees a whole UI or the previous
    // whole UI, never a partial one.
    void endFrame() {
        std::lock_guard<std::mutex> lock(mutex_);
        published_.swap(building_);
    }

    // Resolve a name to the CENTRE of its rect. Safe to call from any thread.
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

    // Everything drawn last frame, as "name=x,y,w,h" separated by spaces.
    //
    // THIS LIST IS THE CLICKABLE SURFACE. It is generated from what actually drew, so it cannot claim a
    // button that is not there -- which is what makes "can this be driven like a user would?" a question
    // with an answer rather than an aspiration.
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

    usize count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<TrackedWidget> building_;    // this frame, main thread only
    std::vector<TrackedWidget> published_;   // last complete frame, read by anyone
};

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
