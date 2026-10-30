#pragma once
// Aver.UI style ABI: the pieces of control rendering the managed UI layer asks the engine for. Style
// boxes are tessellated here and drawn into the same draw list as the HUD (ui_abi.h), so the renderer needs nothing new;
// the stretch arithmetic and the built-in default theme text have one implementation, in C++. Part of the Aver.UI.Abi
// library; plain C, int32 / float / pointers only.
#include "aver/ui/ui_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_UI_STYLE_ABI_VERSION 1

// Style box kinds (aver::ui::UiStyleBoxKind). Mirror Aver.Fabric.UiSbKind.
#define AVER_UI_SB_EMPTY   0
#define AVER_UI_SB_FLAT    1
#define AVER_UI_SB_TEXTURE 2
#define AVER_UI_SB_LINE    3

// UiStyleBox::flags. Mirror Aver.Fabric.UiSbFlags.
#define AVER_UI_SBF_DRAW_CENTER  1
#define AVER_UI_SBF_ANTI_ALIAS   2
#define AVER_UI_SBF_BORDER_BLEND 4
#define AVER_UI_SBF_VERTICAL     8

// 9-patch axis modes (aver::ui::UiAxisStretch). Mirror Aver.Fabric.UiAxisStretch.
#define AVER_UI_AXIS_STRETCH  0
#define AVER_UI_AXIS_TILE     1
#define AVER_UI_AXIS_TILE_FIT 2

// Stretch settings (aver::ui::UiStretchMode / UiStretchAspect / UiStretchScaleMode). Mirror Aver.Fabric.UiStretch*.
#define AVER_UI_STRETCH_DISABLED     0
#define AVER_UI_STRETCH_CANVAS_ITEMS 1
#define AVER_UI_STRETCH_VIEWPORT     2
#define AVER_UI_ASPECT_IGNORE      0
#define AVER_UI_ASPECT_KEEP        1
#define AVER_UI_ASPECT_KEEP_WIDTH  2
#define AVER_UI_ASPECT_KEEP_HEIGHT 3
#define AVER_UI_ASPECT_EXPAND      4
#define AVER_UI_SCALEMODE_FRACTIONAL 0
#define AVER_UI_SCALEMODE_INTEGER    1

// ---- the version and the struct ---------------------------------------------------------------------------------

AVER_UI_API int32_t aver_ui_style_abi_version(void);

// sizeof(aver::ui::UiStyleBox): the managed mirror refuses to run when its own size differs. The struct is passed as an
// opaque pointer; its fields are documented by aver/ui/UiStyleBox.hpp and mirrored by Aver.Fabric.UiStyleBoxStruct.
AVER_UI_API int32_t aver_ui_stylebox_struct_size(void);

// Fills `outStyleBox` (a UiStyleBox) with the defaults of `kind` (its usual property defaults).
AVER_UI_API void aver_ui_stylebox_defaults(int32_t kind, void* outStyleBox);

// ---- style boxes ---------------------------------------------------------------------------------------------------

// Draws the box over (x, y, w, h) into the HUD draw list, on the current layer and clip. Nothing for a texture box with no
// texture handle.
AVER_UI_API void aver_ui_stylebox_draw(const void* styleBox, float x, float y, float w, float h);

// The triangles the draw would add: outXYUV holds 4 floats per vertex (x, y, u, v), outRgba the straight colours, outIdx
// triangle indices. Writes {vertexCount, indexCount, texture low 32 bits, texture high 32 bits} to outCounts (4 ints) and
// returns 1; returns 0 (counts still written) when a buffer is too small.
AVER_UI_API int32_t aver_ui_stylebox_mesh(const void* styleBox, float x, float y, float w, float h, float* outXYUV, uint32_t* outRgba,
                                          int32_t maxVerts, uint32_t* outIdx, int32_t maxIdx, int32_t* outCounts);

// Writes minimum size (w, h) and content margins (l, t, r, b) to out6.
AVER_UI_API void aver_ui_stylebox_metrics(const void* styleBox, float* out6);

// The cells of a 9-patch over (x, y, w, h) already grown by the expand margins: 8 floats each (x, y, w, h, u0, v0, u1, v1).
// Returns the cell count (all of them, even past maxCells).
AVER_UI_API int32_t aver_ui_ninepatch_slices(const void* styleBox, float x, float y, float w, float h, float* outCells, int32_t maxCells);

// ---- stretch -------------------------------------------------------------------------------------------------------

// in6 = {mode, aspect, scaleMode, baseW, baseH, scale}; out11 = {screenX, screenY, screenW, screenH, contentW, contentH,
// renderW, renderH, scaleX, scaleY, letterboxed}. The content-scale rules (aver/ui/UiStretch.hpp).
AVER_UI_API void aver_ui_stretch_compute(const float* in6, float windowW, float windowH, float* out11);

// ---- the default theme -----------------------------------------------------------------------------------------------

// The .octheme text of the built-in default theme. Copies at most cap - 1 bytes plus a terminator when outText
// is not null; returns the full length.
AVER_UI_API int32_t aver_ui_theme_default_text(void* outText, int32_t cap);

// Parses an .octheme document. 1 when valid; otherwise 0 and the reason (cap bytes at most) in errOut when not null.
AVER_UI_API int32_t aver_ui_theme_validate(const char* text, void* errOut, int32_t cap);

#ifdef __cplusplus
} // extern "C"
#endif
