#include "aver/ui/UiDrawList.hpp"

namespace aver::ui {

UiClip UiClip::intersect(const UiClip& o) const {
    UiClip r;
    r.left   = left   > o.left   ? left   : o.left;
    r.top    = top    > o.top    ? top    : o.top;
    r.right  = right  < o.right  ? right  : o.right;
    r.bottom = bottom < o.bottom ? bottom : o.bottom;
    return r;   // may be empty, which is a legal and meaningful result
}

u32 uiPremultiply(u32 rgba) {
    // Input is 0xAABBGGRR to match what a R8G8B8A8_UNORM vertex attribute reads on a little-endian
    // machine, so the byte order here is r,g,b,a ascending.
    const u32 r = (rgba      ) & 0xFF;
    const u32 g = (rgba >>  8) & 0xFF;
    const u32 b = (rgba >> 16) & 0xFF;
    const u32 a = (rgba >> 24) & 0xFF;
    // +127 rather than truncation: repeatedly premultiplying a mid-grey at alpha 128 with truncation
    // drifts darker, and a UI is full of half-transparent panels stacked on each other.
    const u32 pr = (r * a + 127) / 255;
    const u32 pg = (g * a + 127) / 255;
    const u32 pb = (b * a + 127) / 255;
    return pr | (pg << 8) | (pb << 16) | (a << 24);
}

void UiDrawList::clear() {
    verts_.clear();
    idx_.clear();
    for (auto& c : cmds_) c.clear();
    clipStack_.clear();
    layer_ = UiLayer::Content;
}

void UiDrawList::pushClip(const UiClip& c) {
    // Intersected with whatever is already in force, so a child can never draw outside its parent by
    // pushing a larger rect. That is a containment guarantee rather than a convention: a scrolled
    // list whose child pushed its own unclipped bounds would paint over the panel around it.
    clipStack_.push_back(clipStack_.empty() ? c : clipStack_.back().intersect(c));
}

void UiDrawList::popClip() {
    if (!clipStack_.empty()) clipStack_.pop_back();
}

usize UiDrawList::totalCommands() const {
    usize n = 0;
    for (const auto& c : cmds_) n += c.size();
    return n;
}

void UiDrawList::append(u32 indexCount, u64 texture) {
    auto& list = cmds_[static_cast<usize>(layer_)];
    const UiClip c = clip();
    // MERGED INTO THE PREVIOUS COMMAND when the texture and clip match. Without this a string of a
    // hundred glyphs is a hundred draw calls; with it, one. The check is cheap because it only ever
    // looks at the last command -- draws arrive in traversal order, so anything mergeable is adjacent.
    if (!list.empty()) {
        UiDrawCmd& back = list.back();
        if (back.texture == texture &&
            back.clip.left == c.left && back.clip.top == c.top &&
            back.clip.right == c.right && back.clip.bottom == c.bottom) {
            back.indexCount += indexCount;
            return;
        }
    }
    UiDrawCmd cmd;
    cmd.indexOffset = static_cast<u32>(idx_.size()) - indexCount;
    cmd.indexCount  = indexCount;
    cmd.texture     = texture;
    cmd.clip        = c;
    list.push_back(cmd);
}

void UiDrawList::addRect(f32 x, f32 y, f32 w, f32 h, u32 rgba) {
    addTexturedRect(x, y, w, h, 0, 0.0f, 0.0f, 1.0f, 1.0f, rgba);
}

void UiDrawList::addTexturedRect(f32 x, f32 y, f32 w, f32 h, u64 texture,
                                 f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba) {
    // A fully clipped or degenerate rect emits NOTHING, rather than emitting geometry the scissor
    // would discard. A scrolled-away list is the common case, not a rare one, and paying vertex
    // bandwidth for invisible quads is what makes a long list expensive for no reason.
    if (w <= 0.0f || h <= 0.0f) return;
    if (clip().empty()) return;

    const u32 c = uiPremultiply(rgba);
    const u32 base = static_cast<u32>(verts_.size());
    verts_.push_back(UiVertex{x,     y,     u0, v0, c});
    verts_.push_back(UiVertex{x + w, y,     u1, v0, c});
    verts_.push_back(UiVertex{x + w, y + h, u1, v1, c});
    verts_.push_back(UiVertex{x,     y + h, u0, v1, c});

    // Two triangles, wound consistently. The UI pipeline culls nothing, so the winding is a
    // convention rather than a correctness matter -- but a consistent one means a future
    // back-face-culled debug mode does not make half the UI vanish.
    idx_.push_back(base + 0); idx_.push_back(base + 1); idx_.push_back(base + 2);
    idx_.push_back(base + 0); idx_.push_back(base + 2); idx_.push_back(base + 3);
    append(6, texture);
}

} // namespace aver::ui
