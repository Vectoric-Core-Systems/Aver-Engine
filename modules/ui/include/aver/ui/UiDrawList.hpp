#pragma once
// The UI draw list: what a widget tree produces and a renderer consumes.
// Screen pixels, top-left origin. Knows nothing about the RHI.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::ui {

// One UI vertex: position, texture coordinate, packed colour.
struct UiVertex {
    f32 x = 0, y = 0;      // screen pixels, top-left origin
    f32 u = 0, v = 0;
    u32 rgba = 0xFFFFFFFF; // premultiplied
};

// A clip rectangle in screen pixels. Empty (right <= left) means draw nothing.
struct UiClip {
    i32 left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
    // Returns the intersection of this clip and another; may be empty.
    UiClip intersect(const UiClip& o) const;
};

// One batch: a run of indices sharing a texture and a clip rect. One draw call each.
struct UiDrawCmd {
    u32   indexOffset = 0;
    u32   indexCount  = 0;
    u64   texture     = 0;      // 0 = the white 1x1 texture
    UiClip clip{};
};

// The bands UI draws into, low to high. Order within a band is submission order.
enum class UiLayer : u8 {
    Background = 0,
    Content,
    Overlay,
    Tooltip,
    Debug,
    Count
};

// The output of one frame of UI: shared vertex and index buffers, commands partitioned by layer.
class UiDrawList {
public:
    // Drops all geometry and commands and resets the layer and clip stack.
    void clear();

    void setLayer(UiLayer l) { layer_ = l; }
    UiLayer layer() const { return layer_; }

    // Pushes a clip rect, intersected with the one already in force.
    void pushClip(const UiClip& c);
    // Pops the innermost clip rect.
    void popClip();
    const UiClip& clip() const { return clipStack_.empty() ? noClip_ : clipStack_.back(); }

    // Appends a solid rectangle. `rgba` is straight and is premultiplied on the way in.
    void addRect(f32 x, f32 y, f32 w, f32 h, u32 rgba);
    // Appends a textured rectangle with explicit UVs.
    void addTexturedRect(f32 x, f32 y, f32 w, f32 h, u64 texture, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba);

    const std::vector<UiVertex>& vertices() const { return verts_; }
    const std::vector<u32>&      indices()  const { return idx_; }
    const std::vector<UiDrawCmd>& commands(UiLayer l) const { return cmds_[static_cast<usize>(l)]; }
    // Returns the command count across every layer.
    usize totalCommands() const;
    bool  empty() const { return idx_.empty(); }

private:
    // Appends `indexCount` indices to the current layer, merging into the previous command when the
    // texture and clip match.
    void append(u32 indexCount, u64 texture);

    std::vector<UiVertex> verts_;
    std::vector<u32>      idx_;
    std::vector<UiDrawCmd> cmds_[static_cast<usize>(UiLayer::Count)];
    std::vector<UiClip>   clipStack_;
    UiClip                noClip_{-1 << 24, -1 << 24, 1 << 24, 1 << 24};
    UiLayer               layer_ = UiLayer::Content;
};

// Converts straight RGBA to premultiplied, in the vertex's 0xAABBGGRR packing.
u32 uiPremultiply(u32 rgba);

} // namespace aver::ui
