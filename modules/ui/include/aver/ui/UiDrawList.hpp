#pragma once
// The UI draw list: what a widget tree produces and a renderer consumes.
//
// THIS MODULE KNOWS NOTHING ABOUT THE RHI, and that is the load-bearing decision. A widget tree
// produces vertices, indices, a texture id and a clip rectangle; something else turns that into draw
// calls. The same discipline that lets Aver.Render.PBR be a P/Invoke DLL applies here, and it buys
// three things: the whole UI system is testable with no GPU, a second backend needs no UI changes,
// and the editor could one day render the same lists ImGui renders today without this module
// growing a dependency on either.
//
// WHY LAYERS. A HUD, a pause menu and a tooltip are separate widget trees that must composite in a
// predictable order without any of them knowing the others exist. Sorting individual draws by a
// per-widget depth is the alternative and it is worse: it makes every z decision global, so adding a
// tooltip means auditing every other widget's number. A layer is a coarse, named band -- the HUD is
// below menus, menus are below tooltips, debug is above everything -- and ORDER WITHIN a layer is
// submission order, which is what a tree traversal already gives for free. This is UE's ZOrder and
// Unity's sort order, and it is what both converged on.
//
// SCREEN SPACE, PIXELS, TOP-LEFT ORIGIN. Not normalised device coordinates: a UI is authored against
// a resolution and every layout number a designer types is a pixel. The projection to clip space is
// the renderer's job and happens once.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::ui {

// 20 bytes: position, texture coordinate, packed colour. Deliberately not the engine's MeshVertex --
// a UI has no normal and no 3D position, and reusing a 32-byte vertex would waste a third of every
// buffer on fields no UI shader reads.
struct UiVertex {
    f32 x = 0, y = 0;      // screen pixels, top-left origin
    f32 u = 0, v = 0;
    u32 rgba = 0xFFFFFFFF; // premultiplied on the CPU; see UiDrawList::addRect
};

// A clip rectangle in screen pixels. An empty rect (right <= left) means "draw nothing", which is
// what an element scrolled fully out of its parent produces -- and it must be representable, or
// clipping has to be special-cased at every call site.
struct UiClip {
    i32 left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
    // The intersection, which is how nested clips compose: a child is clipped by its own bounds AND
    // by every ancestor's.
    UiClip intersect(const UiClip& o) const;
};

// One batch: a run of indices sharing a texture and a clip rect. The renderer issues one draw per
// command, so merging adjacent compatible commands is what keeps the count down -- see append().
struct UiDrawCmd {
    u32   indexOffset = 0;
    u32   indexCount  = 0;
    u64   texture     = 0;      // 0 = the white 1x1 texture; the renderer supplies it
    UiClip clip{};
};

// Named bands, coarse on purpose. A widget picks the band it belongs in, not a number it has to
// reason about relative to every other widget.
enum class UiLayer : u8 {
    Background = 0,   // world-space-ish backdrops, letterboxing
    Content,          // the HUD and ordinary widgets
    Overlay,          // menus and modal panels
    Tooltip,          // things that must sit above menus
    Debug,            // never shipped, always on top
    Count
};

// The output of one frame of UI. Vertices and indices are shared across layers -- one buffer upload
// rather than five -- and the layers only partition the COMMANDS.
class UiDrawList {
public:
    void clear();

    // Everything below appends to the CURRENT layer and clip, which is what makes a tree traversal
    // read naturally: push state, recurse, pop.
    void setLayer(UiLayer l) { layer_ = l; }
    UiLayer layer() const { return layer_; }

    void pushClip(const UiClip& c);
    void popClip();
    const UiClip& clip() const { return clipStack_.empty() ? noClip_ : clipStack_.back(); }

    // A solid rectangle. `rgba` is straight (non-premultiplied) and is premultiplied on the way in:
    // the blend mode is src.a / 1-src.a, and premultiplying on the CPU is what makes an additive
    // draw and an alpha draw share one pipeline instead of needing two.
    void addRect(f32 x, f32 y, f32 w, f32 h, u32 rgba);
    // The same with a texture and explicit UVs.
    void addTexturedRect(f32 x, f32 y, f32 w, f32 h, u64 texture, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba);

    // Read back, for the renderer and for tests.
    const std::vector<UiVertex>& vertices() const { return verts_; }
    const std::vector<u32>&      indices()  const { return idx_; }
    const std::vector<UiDrawCmd>& commands(UiLayer l) const { return cmds_[static_cast<usize>(l)]; }
    usize totalCommands() const;
    bool  empty() const { return idx_.empty(); }

private:
    // Appends `indexCount` indices to the current layer, merging into the previous command when the
    // texture and clip match. Without merging, a hundred glyphs in one string would be a hundred
    // draw calls; with it they are one.
    void append(u32 indexCount, u64 texture);

    std::vector<UiVertex> verts_;
    std::vector<u32>      idx_;
    std::vector<UiDrawCmd> cmds_[static_cast<usize>(UiLayer::Count)];
    std::vector<UiClip>   clipStack_;
    UiClip                noClip_{-1 << 24, -1 << 24, 1 << 24, 1 << 24};
    UiLayer               layer_ = UiLayer::Content;
};

// Straight RGBA -> premultiplied, in the packing the vertex uses (0xAABBGGRR, which is what a
// R8G8B8A8_UNORM vertex attribute reads on a little-endian machine).
u32 uiPremultiply(u32 rgba);

} // namespace aver::ui
