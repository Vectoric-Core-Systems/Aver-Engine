#pragma once
// Style boxes (docs/plans/fabric/G5_PARITY.md, U1): the panel, button and frame backgrounds of a converted Godot
// Control tree. A style box is a plain value; drawing one is CPU tessellation into triangles that go to the draw list
// through UiDrawList::addMesh, so Aver.Render.UI draws them with the shader it already has (no new pipeline).
//
// Four kinds, Godot's: Flat (background, per-side borders, per-corner radii, shadow, anti-aliasing, skew), Texture
// (9-patch with stretch / tile / tile-fit axes and a region), Line, Empty. Colours are STRAIGHT 0xAABBGGRR; sizes are
// pixels. Nothing here touches the widget tree's own drawing.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiTypes.hpp"

#include <vector>

namespace aver::ui {

enum class UiStyleBoxKind : i32 { Empty = 0, Flat = 1, Texture = 2, Line = 3 };

// UiStyleBox::flags
inline constexpr u32 kSbDrawCenter = 1;    // default on in Godot: both Flat and Texture fill the middle
inline constexpr u32 kSbAntiAlias  = 2;
inline constexpr u32 kSbBorderBlend = 4;   // Flat: colours blend round corners instead of splitting at the 45-degree point
inline constexpr u32 kSbVertical   = 8;    // Line

// How a 9-patch axis fills its middle (Godot's AxisStretchMode).
enum class UiAxisStretch : i32 { Stretch = 0, Tile = 1, TileFit = 2 };

// One struct for all four kinds so the C seam can pass it by pointer (aver_ui_stylebox_*). Every field is 32 bits
// except `texture`; the layout is mirrored by scripting/csharp/Aver.Fabric/Nodes/UiNative.cs and pinned by
// UiStyleAbiTest. Fields a kind does not use are ignored.
struct UiStyleBox {
    u64 texture = 0;                     // Texture: the engine texture handle (0 = none, nothing is drawn)
    i32 kind = static_cast<i32>(UiStyleBoxKind::Flat);
    u32 flags = kSbDrawCenter;
    f32 contentL = -1, contentT = -1, contentR = -1, contentB = -1;   // < 0 = unset (the kind's default margin)
    f32 expandL = 0, expandT = 0, expandR = 0, expandB = 0;           // the box is drawn this much larger
    // Flat
    u32 bgColor = 0xFF999999;            // Godot's default (0.6, 0.6, 0.6); the border default is (0.8, 0.8, 0.8)
    u32 borderColorL = 0xFFCCCCCC, borderColorT = 0xFFCCCCCC, borderColorR = 0xFFCCCCCC, borderColorB = 0xFFCCCCCC;
    f32 borderL = 0, borderT = 0, borderR = 0, borderB = 0;
    f32 radiusTL = 0, radiusTR = 0, radiusBR = 0, radiusBL = 0;
    i32 cornerDetail = 8;                // segments per rounded corner (1..)
    f32 aaSize = 1.0f;                   // width of the anti-aliasing fringe
    f32 skewX = 0, skewY = 0;            // total lean in pixels across the box
    u32 shadowColor = 0x99000000;        // Godot's default (0, 0, 0, 0.6); size 0 draws no shadow
    f32 shadowSize = 0;
    f32 shadowOffX = 0, shadowOffY = 0;
    // Texture
    f32 texW = 0, texH = 0;              // the texture's size in pixels
    f32 regionX = 0, regionY = 0, regionW = 0, regionH = 0;   // w or h <= 0 = the whole texture
    f32 patchL = 0, patchT = 0, patchR = 0, patchB = 0;       // texture margins
    i32 axisH = 0, axisV = 0;            // UiAxisStretch
    u32 modulate = 0xFFFFFFFF;
    // Line
    u32 lineColor = 0xFF999999;
    f32 thickness = 1;
    f32 growBegin = 1, growEnd = 1;
};
static_assert(sizeof(UiStyleBox) == 200, "UiStyleBox is mirrored field for field by the C# struct");

struct UiMesh {
    std::vector<UiVertex> verts;   // STRAIGHT colours
    std::vector<u32> idx;
    u64 texture = 0;
};

// One cell of a 9-patch: where it goes and which part of the texture (uv 0..1) it shows.
struct UiPatchSlice {
    UiRect dst;
    f32 u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// ---- geometry --------------------------------------------------------------------------------------------

// The rectangle the box is actually drawn over: `rect` grown by the expand margins.
UiRect uiStyleBoxDrawRect(const UiStyleBox& sb, const UiRect& rect);

// Godot's content margins: the explicit one, else the kind's default (Flat: the border width; Texture: the texture
// margin; Line and Empty: 0).
UiInsets uiStyleBoxMargins(const UiStyleBox& sb);

// Margins summed: the least size a container must give the box's content.
UiVec2 uiStyleBoxMinimumSize(const UiStyleBox& sb);

// `rect` inside the content margins (what a child of a PanelContainer gets).
UiRect uiStyleBoxContentRect(const UiStyleBox& sb, const UiRect& rect);

// The cells of a 9-patch over `rect` (already grown by the expand margins). The margins shrink together when the
// destination is smaller than their sum; Tile and TileFit axes cut the middle into several cells. draw_center off
// drops the middle cell(s).
std::vector<UiPatchSlice> uiNinePatchSlices(const UiStyleBox& sb, const UiRect& rect);

// Appends the triangles of the whole box (shadow, fill, border, anti-aliasing fringe; or the 9-patch cells; or the
// line). The mesh's texture is the box's texture for the 9-patch and 0 otherwise.
void uiStyleBoxMesh(const UiStyleBox& sb, const UiRect& rect, UiMesh& out);

// Tessellates and hands the result to the draw list.
void uiStyleBoxDraw(UiDrawList& dl, const UiStyleBox& sb, const UiRect& rect);

// The outline of a rounded rectangle, clockwise from the start of the top-left arc: used by the tessellator and
// exposed for tests. Radii are (rx, ry) per corner TL, TR, BR, BL; each rounded corner contributes detail+1 points and
// a square one contributes one.
std::vector<UiVec2> uiRoundedOutline(const UiRect& r, const f32 rx[4], const f32 ry[4], i32 detail);

} // namespace aver::ui
