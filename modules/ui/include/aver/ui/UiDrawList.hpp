#pragma once
// The UI draw list: what a widget tree produces and a renderer consumes.
// Screen pixels, top-left origin. Knows nothing about the RHI.
#include "aver/core/Types.hpp"
#include "aver/ui/UiFont.hpp"

#include <string_view>
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

    // ---- TEXT, which this draw list could not produce at all -----------------------------------
    //
    // (x, y) is the pen: the LEFT END OF THE BASELINE, not the top-left of a box. That is the
    // convention every glyph's offY is expressed against, and picking the box corner instead would
    // make two strings at different sizes fail to sit on the same line.
    //
    // One textured quad per glyph, all sharing the font's atlas, so a whole string merges into a
    // single draw command through append()'s existing texture/clip check. A codepoint the font does
    // not carry is SKIPPED, not boxed: a missing glyph should cost a gap, not a wall of tofu.
    //
    // Returns the pen's x after the last glyph, so a caller can chain runs (a label then a value)
    // without measuring twice.
    f32 addText(f32 x, f32 y, std::string_view text, const UiFont& font, u32 rgba);

    // ---- HIT TESTING, which this draw list could not do either -----------------------------------
    //
    // A rectangle registered under a caller-chosen id. The draw list already knows the clip stack
    // and the layer, which are exactly what decides whether a point actually reaches a widget, so
    // this records both rather than making every caller re-derive them.
    //
    // NOT A WIDGET TREE, deliberately. This is the smallest thing that turns "the UI drew a button"
    // into "the UI can tell you the pointer is over that button": a game builds its own widgets on
    // top, and a retained tree is a much larger design that should not be smuggled in here.
    void addHitRect(u64 id, f32 x, f32 y, f32 w, f32 h);

    // The id under (x, y), or 0. TOPMOST WINS -- later layers first, and within a layer the LAST
    // registration, because that is the one drawn on top and therefore the one a person sees.
    u64 hitTest(f32 x, f32 y) const;

    struct UiHitRect { u64 id; f32 x, y, w, h; UiClip clip; UiLayer layer; };
    const std::vector<UiHitRect>& hitRects() const { return hits_; }

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
    std::vector<UiHitRect> hits_;
    UiClip                noClip_{-1 << 24, -1 << 24, 1 << 24, 1 << 24};
    UiLayer               layer_ = UiLayer::Content;
};

// Converts straight RGBA to premultiplied, in the vertex's 0xAABBGGRR packing.
u32 uiPremultiply(u32 rgba);

} // namespace aver::ui
