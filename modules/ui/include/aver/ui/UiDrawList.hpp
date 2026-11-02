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

// How a draw samples its texture (P41). The default is what every draw used before: linear, clamped.
enum class UiFilter : u8 { Linear = 0, Nearest = 1 };
enum class UiAddress : u8 { Clamp = 0, Repeat = 1, Mirror = 2 };
// How a draw combines with the backbuffer (P41). Mix is the default (the vertices are premultiplied, so it is the
// premultiplied-alpha blend). Add and Sub take the premultiplied colour; Mul darkens by lerp(1, colour, coverage);
// Disabled writes the colour as it is.
enum class UiBlend : u8 { Mix = 0, Add = 1, Sub = 2, Mul = 3, Disabled = 4, Count };

// A 2D affine map, column form: x' = a*x + c*y + tx, y' = b*x + d*y + ty (P40).
struct UiAffine {
    f32 a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
    // this * o: o is applied first, then this.
    UiAffine operator*(const UiAffine& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
    void apply(f32 x, f32 y, f32& ox, f32& oy) const { ox = a * x + c * y + tx; oy = b * x + d * y + ty; }
};

// One batch: a run of indices sharing a texture, a clip rect and a sampler/blend state. One draw call each.
struct UiDrawCmd {
    u32   indexOffset = 0;
    u32   indexCount  = 0;
    u64   texture     = 0;      // 0 = the white 1x1 texture
    UiClip clip{};
    UiFilter  filter  = UiFilter::Linear;
    UiAddress address = UiAddress::Clamp;
    UiBlend   blend   = UiBlend::Mix;
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

// ---- world-space text (E6) ----------------------------------------------------------------------------
// A label in the 3D world: glyph quads in a text plane (x right, y up, units of the plane) and the matrix that places that
// plane in the world. The renderer draws these in the scene's transparent pass with plain raster pipelines; they ride the
// UI draw list only because it is already what hosts hand the renderer once per frame.
enum UiWorldTextFlag : u32 {
    kWorldTextNoDepth = 1,       // drawn over the scene instead of depth-tested against it
    kWorldTextAdditive = 2,      // added to the scene instead of premultiplied alpha
    kWorldTextBillboard = 4,     // the plane turns to face the camera
    kWorldTextBillboardY = 8,    // the plane turns about the world's up axis only (with kWorldTextBillboard unset)
    kWorldTextOneSided = 16,     // the back of the plane is culled
};
struct UiWorldTextRun { u32 indexOffset = 0, indexCount = 0; u32 page = 0; };
struct UiWorldText {
    f32 world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // row-major, row vectors: plane (x, y, 0, 1) * world
    u32 vertexOffset = 0, vertexCount = 0;     // into worldVertices()
    u32 runBegin = 0, runCount = 0;            // into worldTextRuns(); a run's indices are relative to vertexOffset
    u32 flags = 0;
    f32 alphaCut = 0.0f;                       // pixels with less alpha than this are dropped (0 = none)
    f32 intensity = 1.0f;                      // colour multiplier (scene units)
    f32 fixedPxPerUnit = 0.0f;                 // > 0: constant on-screen size, one plane unit drawn as this many pixels
    f32 sdfSoftness = 0.0f;                    // 0 = the renderer's default for distance-field glyphs
};
// The caps on what one frame can carry, so a script cannot ask for an unbounded buffer.
constexpr u32 kUiMaxWorldTexts = 1024;
constexpr u32 kUiMaxWorldVertices = 131072;
constexpr u32 kUiMaxWorldIndices = 196608;
constexpr u32 kUiMaxWorldRuns = 8192;

// The output of one frame of UI: shared vertex and index buffers, commands partitioned by layer.
class UiDrawList {
public:
    // Adds a label: `verts` and `indices` are copied (colours must already be premultiplied); each run names a range of
    // `indices` (a multiple of three) and an atlas page; the header's offsets are filled in. Nothing is added when a cap
    // would be passed, a count is zero, a run leaves the index array, an index is past the vertices or a float is not
    // finite. True when added.
    bool addWorldText(const UiWorldText& header, const UiVertex* verts, usize vertCount, const u32* indices, usize indexCount,
                      const UiWorldTextRun* runs, usize runCount);
    const std::vector<UiWorldText>& worldTexts() const { return worldTexts_; }
    const std::vector<UiWorldTextRun>& worldTextRuns() const { return worldRuns_; }
    const std::vector<UiVertex>& worldVertices() const { return worldVerts_; }
    const std::vector<u32>& worldIndices() const { return worldIdx_; }

    // Drops all geometry and commands and resets the layer and clip stack.
    void clear();

    void setLayer(UiLayer l) { layer_ = l; }
    UiLayer layer() const { return layer_; }

    // Pushes a clip rect, intersected with the one already in force.
    void pushClip(const UiClip& c);
    // Pops the innermost clip rect.
    void popClip();
    const UiClip& clip() const { return clipStack_.empty() ? noClip_ : clipStack_.back(); }

    // ---- P40: a 2D affine transform on everything added after it ----------------------------------
    // Pushes `local` composed onto the transform in force (it is applied first). Rects, glyph quads and meshes are
    // transformed vertex by vertex; a clip rect and a hit rect become the axis-aligned box of their transformed
    // corners (exact for a quarter turn, a bounding box for any other angle). The stack is empty after clear().
    // A non-finite matrix is ignored (and its pop too, so pushes and pops stay paired).
    void pushTransform(const UiAffine& local);
    void popTransform();
    const UiAffine& transform() const { return xformStack_.empty() ? identity_ : xformStack_.back(); }
    bool hasTransform() const { return !xformStack_.empty() && !xformStack_.back().identity(); }

    // ---- P41: per-draw sampler and blend state, in force for what is added after it ---------------
    void setSampler(UiFilter f, UiAddress a) { filter_ = f; address_ = a; }
    void setBlend(UiBlend b) { blend_ = b; }
    UiFilter filter() const { return filter_; }
    UiAddress address() const { return address_; }
    UiBlend blend() const { return blend_; }

    // Appends a solid rectangle. `rgba` is straight and is premultiplied on the way in.
    void addRect(f32 x, f32 y, f32 w, f32 h, u32 rgba);
    // Appends a textured rectangle with explicit UVs.
    void addTexturedRect(f32 x, f32 y, f32 w, f32 h, u64 texture, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba);
    // A glyph quad. `skew` (0 = none) shears the top edge right by skew * (shearBaselineY - y): the
    // synthetic italic of a face that has no italic. Merges like any other textured rect.
    void addGlyphQuad(f32 x, f32 y, f32 w, f32 h, u64 texture, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba,
                      f32 skew = 0.0f, f32 shearBaselineY = 0.0f);

    // Appends triangles built elsewhere (the style boxes: UiStyleBox.hpp). Vertex colours are STRAIGHT and are
    // premultiplied on the way in; `indices` are relative to `verts`. Merges like any other textured run. Nothing is
    // drawn for an empty clip, no triangles, or an index past the vertices.
    void addMesh(const UiVertex* verts, usize vertCount, const u32* indices, usize indexCount, u64 texture);

    // ---- TEXT, which this draw list could not produce at all -----------------------------------
    //
    // The text is UTF-8 and glyphs are looked up per code point (a multi-byte sequence used to draw one
    // wrong glyph per byte).
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

    // The same, with every glyph (metrics and advance) multiplied by `scale`. The widget system
    // draws at a size that follows the DPI scale, from a font baked at one size.
    f32 addTextScaled(f32 x, f32 y, std::string_view text, const UiFont& font, u32 rgba, f32 scale);

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
    std::vector<UiWorldText> worldTexts_;
    std::vector<UiWorldTextRun> worldRuns_;
    std::vector<UiVertex> worldVerts_;
    std::vector<u32> worldIdx_;
    std::vector<UiClip>   clipStack_;
    std::vector<UiAffine> xformStack_;     // each entry is the composed transform; a rejected push repeats the one below
    UiAffine              identity_{};
    UiFilter              filter_  = UiFilter::Linear;
    UiAddress             address_ = UiAddress::Clamp;
    UiBlend               blend_   = UiBlend::Mix;
    std::vector<UiHitRect> hits_;
    UiClip                noClip_{-1 << 24, -1 << 24, 1 << 24, 1 << 24};
    UiLayer               layer_ = UiLayer::Content;
};

// Converts straight RGBA to premultiplied, in the vertex's 0xAABBGGRR packing.
u32 uiPremultiply(u32 rgba);

} // namespace aver::ui
