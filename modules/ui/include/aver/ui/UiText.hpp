#pragma once
// Text measurement and the painter interface the widget tree draws through. The tree never touches
// UiDrawList directly: a test records calls, the editor preview paints with ImGui, and the game
// paints into a UiDrawList, all from the same tree.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiTextLayout.hpp"
#include "aver/ui/UiTypes.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

class UiTextEngine;

// Measures text at a pixel size. Layout needs this before anything is drawn.
class UiTextMetrics {
public:
    virtual ~UiTextMetrics() = default;
    virtual f32 textWidth(std::string_view text, f32 px) const = 0;   // widest line
    virtual f32 lineHeight(f32 px) const = 0;
    virtual f32 ascent(f32 px) const = 0;                              // baseline below the line top

    // The positioned text for a string: what the tree measures AND draws. The default builds a legacy
    // layout from the three virtuals above (uiWrapText for wrapping), so every existing implementer keeps
    // today's behaviour. UiEngineMetrics / UiDrawListPainter with an engine override it.
    virtual std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle& style,
                                                           const UiTextBox& box) const;
};

// Font-less estimate: fixed advance per code point. For headless tools and tests.
class UiEstimatedMetrics final : public UiTextMetrics {
public:
    f32 textWidth(std::string_view text, f32 px) const override;
    f32 lineHeight(f32 px) const override { return px * 1.25f; }
    f32 ascent(f32 px) const override { return px * 0.95f; }
};

// Metrics of a baked UiFont scaled to `px` (the font's own size is px = font.pixelSize).
class UiFontMetrics final : public UiTextMetrics {
public:
    explicit UiFontMetrics(const UiFont* font) : font_(font) {}
    f32 textWidth(std::string_view text, f32 px) const override;
    f32 lineHeight(f32 px) const override;
    f32 ascent(f32 px) const override;
private:
    f32 scaleFor(f32 px) const;
    const UiFont* font_;
};

// Splits `text` into lines no wider than `maxWidth`. Hard newlines always break; a word wider than
// the line is broken between characters. maxWidth <= 0 only splits on newlines.
std::vector<std::string> uiWrapText(const UiTextMetrics& m, std::string_view text, f32 px, f32 maxWidth);

// Number of UTF-8 code points.
usize uiUtf8Length(std::string_view s);
// Byte offset of the code point at `index` (clamped to the string length).
usize uiUtf8Offset(std::string_view s, usize index);
// Appends a code point as UTF-8.
void uiAppendUtf8(std::string& out, u32 cp);

// A localization text reference "@Table:Key[=Default]" (Table [A-Za-z0-9_]+, Key [A-Za-z0-9_.-]+, default = the rest
// after the first '='). Same grammar as aver::fmt::parseLocRef; Aver.UI is Core-only so it carries its own copy.
// False for anything else, including the "@@..." escape.
struct UiTextRef { std::string table, key, defaultText; bool hasDefault = false; };
bool uiParseTextRef(std::string_view s, UiTextRef& out);

// x of a line's left edge inside `area`. Start/End follow the paragraph direction.
inline f32 uiAlignedX(UiTextAlign a, const UiRect& area, f32 lineWidth, bool rtl) {
    if (a == UiTextAlign::Start) a = rtl ? UiTextAlign::Right : UiTextAlign::Left;
    else if (a == UiTextAlign::End) a = rtl ? UiTextAlign::Left : UiTextAlign::Right;
    if (a == UiTextAlign::Center) return area.x + (area.w - lineWidth) * 0.5f;
    if (a == UiTextAlign::Right) return area.right() - lineWidth;
    return area.x;
}

// What the tree draws with.
class UiPainter : public UiTextMetrics {
public:
    virtual void setLayer(UiLayer) {}
    virtual void pushClip(const UiRect& r) = 0;
    virtual void popClip() = 0;
    virtual void fillRect(const UiRect& r, u32 rgba) = 0;
    virtual void image(const UiRect& r, u64 texture, const UiUvRect& uv, u32 tint) = 0;
    // (x, baselineY) is the left end of the baseline, UiDrawList::addText's convention.
    virtual void text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) = 0;
    // Draws a layout's lines inside `area`, aligned per line (Left/Center/Right physical, Start/End by
    // the line's direction), optionally centring the block vertically. The default paints each line
    // with text() using the old block maths, so a painter that only implements text() is unchanged.
    virtual void drawText(const UiTextLayout& layout, const UiRect& area, UiTextAlign align, bool centreVertically,
                          u32 rgba);

    // Four filled edges inside `r`.
    void strokeRect(const UiRect& r, f32 thickness, u32 rgba);
};

// Paints into a UiDrawList. `font` may be null: text is then skipped and measured with estimates.
// With a UiTextEngine that has a TrueType face, all text goes through the engine and `font` is only
// the fallback face inside the engine's chain.
class UiDrawListPainter final : public UiPainter {
public:
    UiDrawListPainter(UiDrawList& list, const UiFont* font);
    UiDrawListPainter(UiDrawList& list, UiTextEngine* engine, const UiFont* bitmap);

    void setLayer(UiLayer l) override { list_.setLayer(l); }
    void pushClip(const UiRect& r) override;
    void popClip() override { list_.popClip(); }
    void fillRect(const UiRect& r, u32 rgba) override { list_.addRect(r.x, r.y, r.w, r.h, rgba); }
    void image(const UiRect& r, u64 texture, const UiUvRect& uv, u32 tint) override;
    void text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) override;

    void drawText(const UiTextLayout& layout, const UiRect& area, UiTextAlign align, bool centreVertically,
                  u32 rgba) override;

    f32 textWidth(std::string_view t, f32 px) const override;
    f32 lineHeight(f32 px) const override;
    f32 ascent(f32 px) const override;
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle& style,
                                                   const UiTextBox& box) const override;

private:
    bool engineOn() const;

    UiDrawList& list_;
    const UiFont* font_;
    UiTextEngine* engine_ = nullptr;
    UiFontMetrics fontMetrics_;
    UiEstimatedMetrics estimate_;
};

} // namespace aver::ui
