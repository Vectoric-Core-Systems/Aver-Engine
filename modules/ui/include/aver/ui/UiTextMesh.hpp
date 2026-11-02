#pragma once
// Text as a mesh in a plane (parity E6, the engine half of Label3D): the text engine's layout turned into glyph quads in
// the text plane's own units (+x right, +y up, the text origin at (0, 0)), with the UVs of the WORLD glyph atlas. Core-only
// and CPU-only, so the layout, the alignment, the outline and the page bookkeeping are all decidable without a device.
//
// WHERE THE QUADS LIVE. A mesh is a vertex and index list plus a short list of runs, one per atlas page (and per
// outline/fill pass) in draw order. The UVs name atlas pages, and a page can be recycled for other glyphs (UiGlyphAtlas
// evicts the least recently used page whole), so a mesh remembers the atlas epoch and every page's generation it was built
// against: stale() says when its UVs would now show other glyphs, and the owner rebuilds it. touch() stamps its pages as
// used this frame, which is what keeps the recycler off them while the label stays on screen.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiGlyphAtlas.hpp"
#include "aver/ui/UiTextEngine.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::ui {

enum class UiTextHAlign : u8 { Left = 0, Center = 1, Right = 2 };
enum class UiTextVAlign : u8 { Top = 0, Center = 1, Bottom = 2 };

// What a world label asks for. Sizes are in layout pixels (a Label3D's font_size); pixelSize turns them into plane units.
struct UiTextMeshStyle {
    std::string family;
    f32 px = 32.0f;                  // the em size, layout pixels
    f32 pixelSize = 0.01f;           // plane units per layout pixel
    f32 width = 0.0f;                // wrap width in layout pixels; 0 = one line per paragraph
    bool wrap = false, ellipsis = false, rich = false, bold = false, italic = false;
    u32 maxLines = 0;
    UiTextDir dir = UiTextDir::Auto;
    UiTextHAlign hAlign = UiTextHAlign::Center;
    UiTextVAlign vAlign = UiTextVAlign::Center;
    f32 lineSpacing = 1.0f, letterSpacing = 0.0f, spaceSpacing = 0.0f, lineExtra = 0.0f;   // as UiTextStyle
    u32 rgba = 0xFFFFFFFF;           // straight 0xAABBGGRR
    f32 outlineSize = 0.0f;          // layout pixels; 0 = none
    u32 outlineRgba = 0xFF000000;
    f32 offsetX = 0.0f, offsetY = 0.0f;   // layout pixels, +y up, added to the anchor
};

// The caps, so a label can never ask for an unbounded buffer.
constexpr u32 kUiTextMeshMaxQuads = 8192;       // glyph quads in one mesh, outline passes included
constexpr u32 kUiTextMeshMaxChars = 2048;       // bytes of text read

struct UiTextMesh {
    std::vector<UiVertex> verts;     // x, y in plane units; u, v in the world atlas page; colour premultiplied
    std::vector<u32> indices;
    // One draw: a run of indices on one atlas page. Runs are in draw order (outline passes first, the fill last).
    struct Run { u32 indexOffset = 0, indexCount = 0; u16 page = 0; u32 generation = 0; };
    std::vector<Run> runs;
    u64 atlasEpoch = 0;
    f32 width = 0, height = 0;       // the laid-out text box, plane units (the outline is not counted)
    f32 minX = 0, minY = 0, maxX = 0, maxY = 0;   // the quads' bounds, plane units
    u32 lines = 0;
    bool incomplete = false;         // a glyph was not rasterised yet (frame budget, atlas full): build again next frame
    bool truncated = false;          // the quad cap was reached

    bool empty() const { return indices.empty(); }
    // True when any page this mesh names has been recycled or dropped since it was built.
    bool stale(const UiGlyphAtlas& atlas) const;
    // Stamps every page it names as used in `frame`.
    void touch(UiGlyphAtlas& atlas, u64 frame) const;
};

// Lays `text` out in the engine's world atlas and builds its mesh into `out` (cleared first). False when the engine has no
// usable face or the text is empty. `out.incomplete` is set when some glyphs are still being rasterised.
bool uiBuildTextMesh(UiTextEngine& engine, std::string_view text, const UiTextMeshStyle& style, UiTextMesh& out);

// Meshes by (text, style), rebuilt when stale. Entries unused for `keepFrames` frames are dropped, and at most `maxEntries`
// are kept, so a label that comes and goes frees its quads.
class UiTextMeshCache {
public:
    // The mesh for the text, building or rebuilding it as needed and touching its pages. Null when it cannot be built.
    // The pointer is valid until the next get() or trim().
    const UiTextMesh* get(UiTextEngine& engine, std::string_view text, const UiTextMeshStyle& style);
    void trim(u64 frame);
    void clear() { entries_.clear(); }
    usize size() const { return entries_.size(); }
    u64 builds() const { return builds_; }
    u64 rebuildsStale() const { return rebuildsStale_; }

    u32 keepFrames = 120;
    u32 maxEntries = 256;

private:
    struct Entry {
        UiTextMesh mesh;
        u64 lastFrame = 0;
    };
    std::unordered_map<u64, Entry> entries_;
    u64 builds_ = 0, rebuildsStale_ = 0;
};

// The hash of everything that shapes a mesh (the cache key).
u64 uiTextMeshKey(std::string_view text, const UiTextMeshStyle& style);

} // namespace aver::ui
