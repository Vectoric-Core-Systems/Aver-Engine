// UiTextMesh: a text layout as glyph quads in a plane, and the cache that keeps them valid. See UiTextMesh.hpp.
#include "aver/ui/UiTextMesh.hpp"

#include "aver/ui/UiTextEffect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::ui {
namespace {

constexpr f32 kItalicSkew = 0.2f;

// One glyph quad in layout units (y down from the anchor), before the passes copy it.
struct Placed {
    u16 page = 0;
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    u32 color = 0xFFFFFFFF;       // straight, rich colour resolved
    f32 skew = 0, baseline = 0;   // synthetic italic: shear about this y
};

bool finiteAll(std::initializer_list<f32> v) {
    for (f32 f : v) if (!std::isfinite(f)) return false;
    return true;
}

u64 mix(u64 h, const void* p, usize n) {
    const u8* b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

template <class T>
u64 mixv(u64 h, T v) { return mix(h, &v, sizeof v); }

} // namespace

bool UiTextMesh::stale(const UiGlyphAtlas& atlas) const {
    if (atlas.epoch() != atlasEpoch) return true;
    for (const Run& r : runs)
        if (r.page >= atlas.pageCount() || atlas.pageGeneration(r.page) != r.generation) return true;
    return false;
}

void UiTextMesh::touch(UiGlyphAtlas& atlas, u64 frame) const {
    for (const Run& r : runs) atlas.touchPage(r.page, frame);
}

u64 uiTextMeshKey(std::string_view text, const UiTextMeshStyle& s) {
    u64 h = 0xCBF29CE484222325ull;
    h = mix(h, text.data(), text.size());
    h = mix(h, s.family.data(), s.family.size());
    h = mixv(h, s.px); h = mixv(h, s.pixelSize); h = mixv(h, s.width);
    h = mixv<u32>(h, (s.wrap ? 1u : 0u) | (s.ellipsis ? 2u : 0u) | (s.rich ? 4u : 0u) | (s.bold ? 8u : 0u) | (s.italic ? 16u : 0u));
    h = mixv(h, s.maxLines); h = mixv<u32>(h, static_cast<u32>(s.dir));
    h = mixv<u32>(h, static_cast<u32>(s.hAlign) | (static_cast<u32>(s.vAlign) << 8));
    h = mixv(h, s.lineSpacing); h = mixv(h, s.letterSpacing); h = mixv(h, s.spaceSpacing); h = mixv(h, s.lineExtra);
    h = mixv(h, s.rgba); h = mixv(h, s.outlineSize); h = mixv(h, s.outlineRgba);
    h = mixv(h, s.offsetX); h = mixv(h, s.offsetY);
    return h;
}

bool uiBuildTextMesh(UiTextEngine& engine, std::string_view text, const UiTextMeshStyle& st, UiTextMesh& out) {
    out = UiTextMesh{};
    if (!engine.usable() || text.empty()) return false;
    if (!finiteAll({st.px, st.pixelSize, st.width, st.lineSpacing, st.letterSpacing, st.spaceSpacing, st.lineExtra, st.outlineSize,
                    st.offsetX, st.offsetY}))
        return false;

    // The text read is capped, on a code point boundary.
    if (text.size() > kUiTextMeshMaxChars) {
        usize n = kUiTextMeshMaxChars;
        while (n > 0 && (static_cast<u8>(text[n]) & 0xC0) == 0x80) --n;
        text = text.substr(0, n);
    }
    const f32 px = std::clamp(st.px, 4.0f, 1024.0f);
    const f32 pixelSize = std::clamp(st.pixelSize, 1.0e-6f, 100.0f);

    UiTextStyle ts;
    ts.family = st.family;
    ts.px = engine.worldLayoutPx(px);
    const f32 k = ts.px / px;                  // layout units per requested pixel (1 unless the raster size differs)
    ts.scale = k;
    ts.bold = st.bold;
    ts.italic = st.italic;
    ts.dir = st.dir;
    ts.lineSpacing = st.lineSpacing > 0.0f ? std::clamp(st.lineSpacing, 0.1f, 10.0f) : 1.0f;
    ts.letterSpacing = std::clamp(st.letterSpacing, -64.0f, 256.0f) * k;
    ts.spaceSpacing = std::clamp(st.spaceSpacing, -64.0f, 256.0f) * k;
    ts.lineExtra = std::clamp(st.lineExtra, -256.0f, 1024.0f) * k;
    UiTextBox box;
    const f32 boxWidth = std::clamp(st.width, 0.0f, 65536.0f) * k;
    box.maxWidth = boxWidth;
    box.wrap = st.wrap && boxWidth > 0.0f;
    box.ellipsis = st.ellipsis;
    box.rich = st.rich;
    box.maxLines = st.maxLines;

    const std::shared_ptr<const UiTextLayout> layout = engine.layoutWorld(text, ts, box);
    if (!layout) return false;
    out.incomplete = layout->incomplete;
    UiGlyphAtlas& atlas = engine.worldAtlas();
    out.atlasEpoch = atlas.epoch();

    const f32 u = pixelSize / k;               // plane units per layout unit
    const f32 boxW = boxWidth > 0.0f ? boxWidth : layout->width;
    const f32 originX = st.hAlign == UiTextHAlign::Left ? 0.0f : (st.hAlign == UiTextHAlign::Center ? -boxW * 0.5f : -boxW);
    const f32 originY = st.vAlign == UiTextVAlign::Top ? 0.0f : (st.vAlign == UiTextVAlign::Center ? -layout->height * 0.5f : -layout->height);
    out.width = boxW * u;
    out.height = layout->height * u;
    out.lines = static_cast<u32>(layout->lines.size());

    // ---- place every glyph once ----
    const f32 pageSize = static_cast<f32>(atlas.pageSize());
    std::vector<Placed> placed;
    placed.reserve(layout->glyphs.size());
    for (const UiTextLine& line : layout->lines) {
        f32 lineX = 0.0f;
        if (st.hAlign == UiTextHAlign::Center) lineX = (boxW - line.width) * 0.5f;
        else if (st.hAlign == UiTextHAlign::Right) lineX = boxW - line.width;
        const f32 baselineY = originY + line.top + line.baseline;
        for (u32 gi = line.glyphBegin; gi < line.glyphEnd && gi < layout->glyphs.size(); ++gi) {
            const UiGlyphRef& g = layout->glyphs[gi];
            if (g.face == kUiFaceNone || g.face == kUiFaceBitmap || (g.flags & kGlyphBox)) continue;
            const UiAtlasGlyph* a = atlas.lookup(UiGlyphKey{g.face, g.rasterPx, g.glyph}, engine.frame());
            if (!a) { out.incomplete = true; continue; }
            if (a->w == 0 || a->h == 0 || a->page == kUiNoPage) continue;
            u32 color = st.rgba;
            if (g.color != 0 && g.color < layout->palette.size()) {
                const u32 rich = layout->palette[g.color];
                const u32 al = ((rich >> 24) * (st.rgba >> 24) + 127) / 255;
                color = (rich & 0x00FFFFFFu) | (al << 24);
            }
            Placed p;
            p.page = a->page;
            const f32 q = g.quad;
            p.x = originX + lineX + g.x + static_cast<f32>(a->offX) * q;
            p.y = baselineY + g.y + static_cast<f32>(a->offY) * q;
            p.w = static_cast<f32>(a->w) * q;
            p.h = static_cast<f32>(a->h) * q;
            p.u0 = static_cast<f32>(a->x) / pageSize;
            p.v0 = static_cast<f32>(a->y) / pageSize;
            p.u1 = static_cast<f32>(a->x + a->w) / pageSize;
            p.v1 = static_cast<f32>(a->y + a->h) / pageSize;
            p.color = color;
            p.skew = (g.flags & kGlyphSkew) ? kItalicSkew : 0.0f;
            p.baseline = baselineY + g.y;
            placed.push_back(p);
            if (placed.size() >= kUiTextMeshMaxQuads) { out.truncated = true; break; }
        }
        if (out.truncated) break;
    }
    if (placed.empty()) return true;   // blank text: an empty mesh

    // ---- the passes: outline copies first, the fill last ----
    UiTextFxStyle fx;
    fx.outlineSize = std::clamp(st.outlineSize, 0.0f, 32.0f) * k;
    fx.outlineRgba = st.outlineRgba;
    std::vector<UiTextFxPass> passes = uiTextFxPasses(fx, st.rgba);
    if (passes.size() * placed.size() > kUiTextMeshMaxQuads) {   // too many copies: the outline is dropped, not the text
        out.truncated = true;
        passes.assign(1, UiTextFxPass{0, 0, st.rgba, true});
    }

    // Glyphs ordered by page within a pass, so a pass is a few runs.
    std::vector<u32> order(placed.size());
    for (u32 i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return placed[a].page < placed[b].page; });

    out.verts.reserve(passes.size() * placed.size() * 4);
    out.indices.reserve(passes.size() * placed.size() * 6);
    f32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    const f32 offX = st.offsetX * pixelSize, offY = st.offsetY * pixelSize;
    for (const UiTextFxPass& pass : passes) {
        for (u32 oi = 0; oi < order.size(); ++oi) {
            const Placed& p = placed[order[oi]];
            const u32 col = uiPremultiply(pass.fill ? p.color : pass.rgba);
            const f32 lx = p.x + pass.dx, ly = p.y + pass.dy;
            const f32 sTop = p.skew * (p.baseline - ly), sBot = p.skew * (p.baseline - (ly + p.h));
            const f32 xs[4] = {lx + sTop, lx + p.w + sTop, lx + p.w + sBot, lx + sBot};
            const f32 ys[4] = {ly, ly, ly + p.h, ly + p.h};
            const f32 us[4] = {p.u0, p.u1, p.u1, p.u0};
            const f32 vs[4] = {p.v0, p.v0, p.v1, p.v1};
            const u32 base = static_cast<u32>(out.verts.size());
            for (int c = 0; c < 4; ++c) {
                const f32 X = xs[c] * u + offX, Y = -ys[c] * u + offY;
                out.verts.push_back(UiVertex{X, Y, us[c], vs[c], col});
                if (pass.fill) { minX = std::min(minX, X); maxX = std::max(maxX, X); minY = std::min(minY, Y); maxY = std::max(maxY, Y); }
            }
            // Winding: y is up here, so (0 1 2) is clockwise seen from +z; the shader draws both sides.
            const u32 idx[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            out.indices.insert(out.indices.end(), idx, idx + 6);
            if (!out.runs.empty() && out.runs.back().page == p.page && out.runs.back().indexOffset + out.runs.back().indexCount == out.indices.size() - 6)
                out.runs.back().indexCount += 6;
            else
                out.runs.push_back(UiTextMesh::Run{static_cast<u32>(out.indices.size()) - 6, 6, p.page, atlas.pageGeneration(p.page)});
        }
    }
    out.minX = minX; out.maxX = maxX; out.minY = minY; out.maxY = maxY;
    return true;
}

const UiTextMesh* UiTextMeshCache::get(UiTextEngine& engine, std::string_view text, const UiTextMeshStyle& style) {
    const u64 key = uiTextMeshKey(text, style);
    UiGlyphAtlas& atlas = engine.worldAtlas();
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        Entry& e = it->second;
        e.lastFrame = engine.frame();
        if (!e.mesh.incomplete && !e.mesh.stale(atlas)) {
            e.mesh.touch(atlas, engine.frame());
            return &e.mesh;
        }
        if (e.mesh.stale(atlas)) ++rebuildsStale_;
    }
    if (it == entries_.end() && entries_.size() >= maxEntries) {   // full: the least recently used one goes
        auto oldest = entries_.begin();
        for (auto i = entries_.begin(); i != entries_.end(); ++i)
            if (i->second.lastFrame < oldest->second.lastFrame) oldest = i;
        entries_.erase(oldest);
    }
    Entry& e = entries_[key];
    e.lastFrame = engine.frame();
    ++builds_;
    if (!uiBuildTextMesh(engine, text, style, e.mesh)) {
        entries_.erase(key);
        return nullptr;
    }
    e.mesh.touch(atlas, engine.frame());
    return &e.mesh;
}

void UiTextMeshCache::trim(u64 frame) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (frame > it->second.lastFrame && frame - it->second.lastFrame > keepFrames) it = entries_.erase(it);
        else ++it;
    }
}

} // namespace aver::ui
