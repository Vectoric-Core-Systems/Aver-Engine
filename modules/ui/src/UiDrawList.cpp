// UiDrawList: clip stack, colour premultiplication, and quad emission with command merging.
#include "aver/ui/UiDrawList.hpp"

namespace aver::ui {

// Returns the intersection of this clip and another; may be empty.
UiClip UiClip::intersect(const UiClip& o) const {
    UiClip r;
    r.left   = left   > o.left   ? left   : o.left;
    r.top    = top    > o.top    ? top    : o.top;
    r.right  = right  < o.right  ? right  : o.right;
    r.bottom = bottom < o.bottom ? bottom : o.bottom;
    return r;
}

// Converts straight RGBA to premultiplied. Both are packed 0xAABBGGRR.
u32 uiPremultiply(u32 rgba) {
    const u32 r = (rgba      ) & 0xFF;
    const u32 g = (rgba >>  8) & 0xFF;
    const u32 b = (rgba >> 16) & 0xFF;
    const u32 a = (rgba >> 24) & 0xFF;
    const u32 pr = (r * a + 127) / 255;
    const u32 pg = (g * a + 127) / 255;
    const u32 pb = (b * a + 127) / 255;
    return pr | (pg << 8) | (pb << 16) | (a << 24);
}

// Drops all geometry and commands and resets the layer and clip stack.
void UiDrawList::clear() {
    verts_.clear();
    idx_.clear();
    for (auto& c : cmds_) c.clear();
    clipStack_.clear();
    layer_ = UiLayer::Content;
}

// Pushes a clip rect, intersected with the one already in force.
void UiDrawList::pushClip(const UiClip& c) {
    clipStack_.push_back(clipStack_.empty() ? c : clipStack_.back().intersect(c));
}

// Pops the innermost clip rect.
void UiDrawList::popClip() {
    if (!clipStack_.empty()) clipStack_.pop_back();
}

// Returns the command count across every layer.
usize UiDrawList::totalCommands() const {
    usize n = 0;
    for (const auto& c : cmds_) n += c.size();
    return n;
}

// Appends `indexCount` indices to the current layer, merging into the previous command when the
// texture and clip match.
void UiDrawList::append(u32 indexCount, u64 texture) {
    auto& list = cmds_[static_cast<usize>(layer_)];
    const UiClip c = clip();
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

// Appends a solid rectangle. `rgba` is straight and is premultiplied on the way in.
void UiDrawList::addRect(f32 x, f32 y, f32 w, f32 h, u32 rgba) {
    addTexturedRect(x, y, w, h, 0, 0.0f, 0.0f, 1.0f, 1.0f, rgba);
}

// Appends a textured rectangle with explicit UVs. A degenerate or fully clipped rect emits nothing.
void UiDrawList::addTexturedRect(f32 x, f32 y, f32 w, f32 h, u64 texture,
                                 f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (clip().empty()) return;

    const u32 c = uiPremultiply(rgba);
    const u32 base = static_cast<u32>(verts_.size());
    verts_.push_back(UiVertex{x,     y,     u0, v0, c});
    verts_.push_back(UiVertex{x + w, y,     u1, v0, c});
    verts_.push_back(UiVertex{x + w, y + h, u1, v1, c});
    verts_.push_back(UiVertex{x,     y + h, u0, v1, c});

    idx_.push_back(base + 0); idx_.push_back(base + 1); idx_.push_back(base + 2);
    idx_.push_back(base + 0); idx_.push_back(base + 2); idx_.push_back(base + 3);
    append(6, texture);
}

} // namespace aver::ui
