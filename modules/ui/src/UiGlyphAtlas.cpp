// UiGlyphAtlas: shelf packing and page-level LRU. See UiGlyphAtlas.hpp.
#include "aver/ui/UiGlyphAtlas.hpp"

#include <algorithm>
#include <cstring>

namespace aver::ui {

UiGlyphAtlas::UiGlyphAtlas() = default;
UiGlyphAtlas::UiGlyphAtlas(const Config& cfg) { configure(cfg); }

void UiGlyphAtlas::configure(const Config& cfg) {
    cfg_.pageSize = std::clamp<u32>(cfg.pageSize, 256, 2048);
    cfg_.maxPages = std::clamp<u32>(cfg.maxPages, 1, 64);
    pages_.clear();
    map_.clear();
}

const UiAtlasGlyph* UiGlyphAtlas::lookup(const UiGlyphKey& key, u64 frame) {
    const auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    if (it->second.page != kUiNoPage && it->second.page < pages_.size()) pages_[it->second.page].lastUsed = frame;
    return &it->second;
}

bool UiGlyphAtlas::place(Page& p, u32 w, u32 h, u16& outX, u16& outY) {
    const u32 size = cfg_.pageSize;
    // The first shelf that is tall enough without wasting more than 4 rows, and has room.
    for (Shelf& s : p.shelves) {
        if (s.h >= h && s.h <= h + 4 && static_cast<u32>(s.x) + w <= size) {
            outX = s.x;
            outY = s.y;
            s.x = static_cast<u16>(s.x + w);
            return true;
        }
    }
    if (p.nextY + h <= size) {
        Shelf s;
        s.y = static_cast<u16>(p.nextY);
        s.h = static_cast<u16>(h);
        s.x = static_cast<u16>(w);
        p.shelves.push_back(s);
        outX = 0;
        outY = s.y;
        p.nextY += h;
        return true;
    }
    return false;
}

void UiGlyphAtlas::resetPage(u32 index) {
    Page& p = pages_[index];
    std::fill(p.pixels.begin(), p.pixels.end(), static_cast<u8>(0));
    p.shelves.clear();
    p.nextY = 0;
    p.used = 0;
    p.dirty = true;
    ++p.generation;
    const u16 idx = static_cast<u16>(index);
    std::erase_if(map_, [idx](const auto& kv) { return kv.second.page == idx; });
}

const UiAtlasGlyph* UiGlyphAtlas::insert(const UiGlyphKey& key, const UiGlyphBitmap& bmp, u64 frame) {
    if (const auto it = map_.find(key); it != map_.end()) return &it->second;

    UiAtlasGlyph g;
    g.offX = bmp.offX;
    g.offY = bmp.offY;
    if (bmp.w == 0 || bmp.h == 0) {   // blank: remembered, no space
        return &(map_[key] = g);
    }
    const u32 w = bmp.w, h = bmp.h, size = cfg_.pageSize;
    if (w > size || h > size || bmp.pixels.size() < static_cast<usize>(w) * h) {
        ++overflow_;
        return nullptr;
    }

    u16 x = 0, y = 0;
    i32 target = -1;
    // 1. an existing page with room
    for (u32 i = 0; i < pages_.size() && target < 0; ++i)
        if (place(pages_[i], w, h, x, y)) target = static_cast<i32>(i);
    // 2. a new page while under the cap
    if (target < 0 && pages_.size() < cfg_.maxPages) {
        Page p;
        p.pixels.assign(static_cast<usize>(size) * size, 0);
        pages_.push_back(std::move(p));
        const u32 i = static_cast<u32>(pages_.size() - 1);
        if (place(pages_[i], w, h, x, y)) target = static_cast<i32>(i);
    }
    // 3. recycle the least-recently-used page not touched this frame
    if (target < 0) {
        i32 victim = -1;
        for (u32 i = 0; i < pages_.size(); ++i) {
            if (pages_[i].lastUsed >= frame) continue;
            if (victim < 0 || pages_[i].lastUsed < pages_[static_cast<u32>(victim)].lastUsed) victim = static_cast<i32>(i);
        }
        if (victim >= 0) {
            resetPage(static_cast<u32>(victim));
            ++evicted_;
            if (place(pages_[static_cast<u32>(victim)], w, h, x, y)) target = victim;
        }
    }
    if (target < 0) {
        ++overflow_;
        return nullptr;
    }

    Page& p = pages_[static_cast<u32>(target)];
    for (u32 row = 0; row < h; ++row)
        std::memcpy(&p.pixels[static_cast<usize>(y + row) * size + x], &bmp.pixels[static_cast<usize>(row) * w], w);
    p.used += w * h;
    p.dirty = true;
    p.lastUsed = frame;

    g.page = static_cast<u16>(target);
    g.x = x;
    g.y = y;
    g.w = static_cast<u16>(w);
    g.h = static_cast<u16>(h);
    return &(map_[key] = g);
}

void UiGlyphAtlas::clear() {
    map_.clear();
    for (u32 i = 0; i < pages_.size(); ++i) {
        Page& p = pages_[i];
        std::fill(p.pixels.begin(), p.pixels.end(), static_cast<u8>(0));
        p.shelves.clear();
        p.nextY = 0;
        p.used = 0;
        p.lastUsed = 0;
        p.dirty = true;
        ++p.generation;
    }
}

const u8* UiGlyphAtlas::pagePixels(u32 page) const { return page < pages_.size() ? pages_[page].pixels.data() : nullptr; }
bool UiGlyphAtlas::pageDirty(u32 page) const { return page < pages_.size() && pages_[page].dirty; }
u32 UiGlyphAtlas::pageGeneration(u32 page) const { return page < pages_.size() ? pages_[page].generation : 0; }
void UiGlyphAtlas::markUploaded(u32 page) { if (page < pages_.size()) pages_[page].dirty = false; }
u64 UiGlyphAtlas::pageLastUsed(u32 page) const { return page < pages_.size() ? pages_[page].lastUsed : 0; }
u32 UiGlyphAtlas::pageUsedPixels(u32 page) const { return page < pages_.size() ? pages_[page].used : 0; }

} // namespace aver::ui
