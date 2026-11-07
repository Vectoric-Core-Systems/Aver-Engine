#pragma once
// The editor's colour table, metrics and type scale as plain data, so a theme is a file and the
// contrast of the shipped palette can be asserted without a window. ImGui-free on purpose: the half
// that turns this into ImGui style entries lives in SandboxApp.hpp (applyEditorColors/Metrics).
//
// Built-in defaults ARE the shipped look; `editor-theme.json` beside the binary only overrides.
// Missing file, unreadable file or malformed JSON -> the defaults, unchanged.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::editor {

struct Rgba { f32 r = 0, g = 0, b = 0, a = 1; };

struct EditorTheme {
    // One ladder of neutrals, deepest to lightest. Equal RGB: no tint.
    Rgba sunken  {19.0f / 255, 19.0f / 255, 19.0f / 255, 1};      // behind content: viewports, child frames
    Rgba panel   {28.0f / 255, 28.0f / 255, 28.0f / 255, 1};      // window bodies
    Rgba raised  {37.0f / 255, 37.0f / 255, 37.0f / 255, 1};      // title bars, menu bar, overlays, inactive tabs
    Rgba item    {46.0f / 255, 46.0f / 255, 46.0f / 255, 1};      // buttons, frames, headers at rest
    Rgba popup   {24.0f / 255, 24.0f / 255, 24.0f / 255, 250.0f / 255};
    Rgba line    {11.0f / 255, 11.0f / 255, 11.0f / 255, 1};      // borders and separators
    Rgba text    {230.0f / 255, 230.0f / 255, 230.0f / 255, 1};
    Rgba textDim {158.0f / 255, 158.0f / 255, 158.0f / 255, 1};   // >= 4.5:1 on panel, raised and item
    // Aver orange: selection, checkmarks, the active tab's top edge and Play. Nothing else.
    Rgba accent  {242.0f / 255, 101.0f / 255, 34.0f / 255, 1};
    // Viewport overlay boxes: `raised` at overlayAlpha with this low-alpha neutral 1px border.
    Rgba overlayBorder {1, 1, 1, 0.10f};

    f32 hoverLift      = 0.07f;   // hover = the item colour mixed this far towards white
    f32 activeLift     = 0.14f;   // pressed
    f32 selectionAlpha = 0.25f;   // selected rows: accent at this alpha
    f32 overlayAlpha   = 0.62f;

    // Metrics, unscaled (DPI scaling is applied after). FramePadding.y is NOT here: the --probe-rel
    // gates depend on the tab-bar height it sets.
    f32 rounding = 4, grabRounding = 3;
    f32 windowPaddingX = 10, windowPaddingY = 8;
    f32 framePaddingX = 8;
    f32 itemSpacingX = 8, itemSpacingY = 6;
    f32 scrollbarSize = 12, indentSpacing = 18;

    // Type scale, unscaled pixels.
    f32 fontHeader = 18.0f, fontBody = 16.0f, fontSmall = 13.5f;
};

// The derived neutrals.
Rgba mixToWhite(const Rgba& c, f32 t);
inline Rgba itemHot(const EditorTheme& t) { return mixToWhite(t.item, t.hoverLift); }
inline Rgba itemOn (const EditorTheme& t) { return mixToWhite(t.item, t.activeLift); }
inline Rgba withAlpha(Rgba c, f32 a) { c.a = a; return c; }
inline Rgba selection(const EditorTheme& t) { return withAlpha(t.accent, t.selectionAlpha); }

// WCAG 2.x relative luminance and contrast ratio (alpha ignored).
f64 relativeLuminance(const Rgba& c);
f64 contrastRatio(const Rgba& a, const Rgba& b);

// "#rrggbb" or "#rrggbbaa". False on anything else.
bool parseHexColour(std::string_view s, Rgba& out);

// Applies a theme document on top of `t`. Returns false, leaving `t` untouched, when the text is not
// a JSON object. Individual bad or out-of-range entries are skipped (and named in `why`, if given)
// while the rest apply -- a typo in one colour must not discard the theme.
bool applyThemeJson(std::string_view json, EditorTheme& t, std::string* why = nullptr);

// Reads `path` over the defaults. Returns false (out = defaults) when absent or malformed.
bool loadThemeFile(const std::string& path, EditorTheme& out, std::string* why = nullptr);

// The theme the editor draws with. Defaults until reloadEditorTheme runs.
const EditorTheme& editorTheme();
// Loads <dir>/editor-theme.json (or the defaults) as the current theme. Warns when a file was rejected.
void reloadEditorTheme(const std::string& dir);

} // namespace aver::editor
