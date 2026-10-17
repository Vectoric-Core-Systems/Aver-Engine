#pragma once
// The game UI's text engine: fonts and fallback chains, runtime TrueType rasterisation into a paged
// atlas, kerning, bidi, Arabic shaping, line breaking, ellipsis and rich text, all behind the existing
// UiTextMetrics / UiPainter seam (UiText.hpp). Core-only: no GPU and no file I/O. Fonts arrive as byte
// blobs from a UiFontLoader; the atlas pages are uploaded by Aver.Render.UI.
//
// FRAME FLOW (docs/GAME_UI_TEXT.md):
//   beginFrame()   resets the per-frame glyph budget (the host does it once, beside aver_ui_begin_frame)
//   layout()       resolves faces, shapes, breaks, positions, and rasterises up to `glyphBudget` NEW
//                  glyphs into the atlas; a layout that could not finish is returned but not cached and
//                  completes on a later frame
//   emitLine()     writes glyph quads into a UiDrawList; it never rasterises, a glyph missing from the
//                  atlas is skipped (that is the "incomplete" case above)
//   the renderer uploads dirty pages from its prePass, before the frame's UI is drawn.
// Single-threaded: call it from the game thread only.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiFont.hpp"
#include "aver/ui/UiFontSet.hpp"
#include "aver/ui/UiGlyphAtlas.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTextLayout.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::ui {

struct UiTextConfig {
    u32 pageSize = 512;          // atlas page edge, 256..2048
    u32 maxPages = 16;           // atlas pages, 1..64
    u32 glyphBudget = 256;       // new rasterisations per frame (prewarm ignores it)
    f32 coverageGamma = 1.0f;    // applied by the renderer when it expands A8 to RGBA8
    f32 legacyPx = 16.0f;        // the size aver_ui_text / aver_ui_text_width draw at
    u32 cacheEntries = 1024;     // cached layouts
};

struct UiTextStats {
    u32 pages = 0, glyphsResident = 0, rasterThisFrame = 0;
    u64 rasterTotal = 0, evictedPages = 0, overflow = 0, layoutHits = 0, layoutMisses = 0;
    u32 cacheEntries = 0;
    u64 incompleteLayouts = 0;
    u32 faces = 0, uploadBytesLastFrame = 0;
};

class UiTextEngine {
public:
    UiTextEngine();

    // Applies the configuration; a new page size or page cap drops every glyph and layout.
    void configure(const UiTextConfig& cfg);
    const UiTextConfig& config() const { return cfg_; }

    // ---- fonts ----
    // Parses and loads an .ocfonts document. False (with `err`) when it does not parse or names no family.
    bool loadFontSet(std::string_view ocfonts, const UiFontLoader& loader, std::string* err = nullptr);
    bool loadFontSet(const UiFontSetDoc& doc, const UiFontLoader& loader, std::string* err = nullptr);
    // The lent baked font (BITMAP legacy). Borrowed.
    void setBitmapFont(const UiFont* font);
    void setLanguage(std::string_view bcp47);
    UiFontSet& fontSet() { return fonts_; }
    const UiFontSet& fontSet() const { return fonts_; }
    UiGlyphAtlas& atlas() { return atlas_; }
    const UiGlyphAtlas& atlas() const { return atlas_; }
    // True when a TrueType face is open: callers keep today's bitmap behaviour when it is not.
    bool usable() const { return fonts_.hasTrueType(); }
    bool hasGlyph(std::string_view family, u32 codePoint);

    // ---- frame ----
    void beginFrame();
    u64 frame() const { return frame_; }
    // Dirty atlas pages: a host that builds text after the renderer's prePass (the layout editor preview)
    // asks for another frame while this is non-zero.
    u32 pendingUploads() const;

    // ---- layout ----
    std::shared_ptr<const UiTextLayout> layout(std::string_view text, const UiTextStyle& style, const UiTextBox& box);
    // Like layout() but ignores the per-frame budget and always completes (loading screens).
    void prewarm(std::string_view text, const UiTextStyle& style, const UiTextBox& box = UiTextBox{});
    // Writes one line's glyph quads. (originX, baselineY) is the line's left edge on the baseline.
    void emitLine(UiDrawList& list, const UiTextLayout& layout, usize line, f32 originX, f32 baselineY, u32 rgba);

    // Default-family metrics (what the three legacy UiTextMetrics virtuals answer).
    f32 textWidth(std::string_view text, f32 px);
    f32 lineHeight(f32 px);
    f32 ascent(f32 px);

    UiTextStats stats() const;
    // Drops cached layouts (the atlas is kept).
    void clearLayouts();

private:
    struct Entry {
        std::shared_ptr<const UiTextLayout> layout;
        u64 lastFrame = 0;
        u64 evictionsSeen = 0;
    };

    struct Build;
    std::shared_ptr<UiTextLayout> build(std::string_view text, const UiTextStyle& style, const UiTextBox& box,
                                        bool ignoreBudget);
    // Rasterises every glyph of `l` that is not resident; false when any could not be (budget, overflow).
    bool ensureGlyphs(const UiTextLayout& l, bool ignoreBudget);
    bool ensureGlyph(u16 face, u32 glyph, u32 rasterPx, bool ignoreBudget);
    u64 keyFor(std::string_view text, const UiTextStyle& style, const UiTextBox& box) const;
    void trimCache();

    UiTextConfig cfg_;
    UiFontSet fonts_;
    UiGlyphAtlas atlas_;
    u64 frame_ = 1;
    u32 rasterThisFrame_ = 0;
    u64 rasterTotal_ = 0, hits_ = 0, misses_ = 0, incomplete_ = 0;
    std::unordered_map<u64, Entry> cache_;
    bool loggedCaps_ = false;
};

// The three legacy metrics virtuals, answered by an engine (default family, no styling), plus layoutText.
// Lets code that only knows UiTextMetrics (UiTree::layout, tests, the editor preview) use the engine.
class UiEngineMetrics final : public UiTextMetrics {
public:
    explicit UiEngineMetrics(UiTextEngine& engine) : engine_(engine) {}
    f32 textWidth(std::string_view text, f32 px) const override { return engine_.textWidth(text, px); }
    f32 lineHeight(f32 px) const override { return engine_.lineHeight(px); }
    f32 ascent(f32 px) const override { return engine_.ascent(px); }
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle& style,
                                                   const UiTextBox& box) const override {
        return engine_.usable() ? engine_.layout(text, style, box) : UiTextMetrics::layoutText(text, style, box);
    }

private:
    UiTextEngine& engine_;
};

} // namespace aver::ui
