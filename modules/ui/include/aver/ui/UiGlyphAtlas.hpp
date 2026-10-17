#pragma once
// The CPU side of the glyph atlas: shelf-packed A8 pages with page-level LRU recycling.
//
// Pages are small (512 x 512 by default) because the RHI can copy a whole buffer to a texture but not a
// sub-region, so a dirty page uploads whole. At most `maxPages` pages exist (16 = 16 MB of A8, 64 MB of
// RGBA8 on the GPU). When a page is needed and the cap is reached, the least-recently-used page that was
// NOT touched this frame is recycled whole: every glyph on it is forgotten and the page re-uploads. If
// every page was touched this frame, insert fails for that glyph (counted as overflow) and the layout
// that wanted it is not cached, so it retries next frame.
//
// A glyph's page appears in a draw list as the texture token `kUiAtlasTokenBit | page`; the renderer maps
// it to the page's GPU texture. rhi::TextureHandle is 32-bit, so a token can never collide with a real one.
#include "aver/core/Types.hpp"
#include "aver/ui/UiFontFace.hpp"

#include <unordered_map>
#include <vector>

namespace aver::ui {

constexpr u64 kUiAtlasTokenBit = 1ull << 63;
constexpr u16 kUiNoPage = 0xFFFF;

struct UiGlyphKey {
    u16 face = 0;
    u16 rasterPx = 0;
    u32 glyph = 0;
    bool operator==(const UiGlyphKey& o) const { return face == o.face && rasterPx == o.rasterPx && glyph == o.glyph; }
};

struct UiGlyphKeyHash {
    usize operator()(const UiGlyphKey& k) const {
        u64 v = (static_cast<u64>(k.face) << 48) ^ (static_cast<u64>(k.rasterPx) << 32) ^ k.glyph;
        v *= 0x9E3779B97F4A7C15ull;
        return static_cast<usize>(v ^ (v >> 29));
    }
};

// Where a glyph's bitmap (gutter included) is. offY is down from the baseline. page == kUiNoPage with
// w == h == 0 is a blank glyph: remembered so it is not rasterised again, drawn as nothing.
struct UiAtlasGlyph {
    u16 page = kUiNoPage;
    u16 x = 0, y = 0, w = 0, h = 0;
    i16 offX = 0, offY = 0;
};

class UiGlyphAtlas {
public:
    struct Config {
        u32 pageSize = 512;    // 256..2048
        u32 maxPages = 16;     // 1..64
    };

    UiGlyphAtlas();
    explicit UiGlyphAtlas(const Config& cfg);

    // Applies a new configuration (clamped) and drops every glyph and page.
    void configure(const Config& cfg);
    const Config& config() const { return cfg_; }

    // The glyph if resident; stamps its page as used in `frame`.
    const UiAtlasGlyph* lookup(const UiGlyphKey& key, u64 frame);
    // Packs a rasterised glyph, recycling the least-recently-used page not used this frame when the page
    // cap is reached. Null when no room could be made (counted in overflow()), or the bitmap is larger
    // than a page. A blank bitmap (w == 0) is recorded without taking space.
    const UiAtlasGlyph* insert(const UiGlyphKey& key, const UiGlyphBitmap& bmp, u64 frame);

    // Forgets every glyph and zeroes every page; pages stay allocated and are marked dirty.
    void clear();

    u32 pageCount() const { return static_cast<u32>(pages_.size()); }
    u32 pageSize() const { return cfg_.pageSize; }
    // A8 pixels of a page, pageSize * pageSize bytes; null for a bad index.
    const u8* pagePixels(u32 page) const;
    bool pageDirty(u32 page) const;
    // Bumps whenever a page's contents were replaced wholesale (recycle, clear): a texture older than
    // this generation shows the wrong glyphs.
    u32 pageGeneration(u32 page) const;
    void markUploaded(u32 page);
    u64 pageLastUsed(u32 page) const;
    // Pixels taken by glyph rectangles on a page, for the editor's occupancy bar.
    u32 pageUsedPixels(u32 page) const;

    u32 glyphsResident() const { return static_cast<u32>(map_.size()); }
    u64 evictedPages() const { return evicted_; }
    u64 overflow() const { return overflow_; }
    // Bytes the renderer copied to the GPU in its last prePass: written there, read for the stats.
    void setUploadBytes(u32 bytes) { uploadBytes_ = bytes; }
    u32 uploadBytes() const { return uploadBytes_; }
    // Coverage gamma the renderer applies when it expands A8 to RGBA8 (1 = linear coverage). Set by the engine.
    void setCoverageGamma(f32 g) { gamma_ = g; }
    f32 coverageGamma() const { return gamma_; }

private:
    struct Shelf {
        u16 y = 0, h = 0, x = 0;
    };
    struct Page {
        std::vector<u8> pixels;
        std::vector<Shelf> shelves;
        u32 nextY = 0;
        u64 lastUsed = 0;
        u32 generation = 0;
        u32 used = 0;
        bool dirty = false;
    };

    bool place(Page& p, u32 w, u32 h, u16& outX, u16& outY);
    void resetPage(u32 index);

    Config cfg_;
    std::vector<Page> pages_;
    std::unordered_map<UiGlyphKey, UiAtlasGlyph, UiGlyphKeyHash> map_;
    u64 evicted_ = 0, overflow_ = 0;
    u32 uploadBytes_ = 0;
    f32 gamma_ = 1.0f;
};

} // namespace aver::ui
