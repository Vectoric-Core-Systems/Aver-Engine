// The .ocui layout editor tab. See the header for the two-line SandboxApp hook and for why the canvas
// maths are free functions.

#include "UiLayoutEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using namespace aver::ui;

namespace {

bool inRange(const UiLayoutDoc& d, i32 i) { return i >= 0 && static_cast<usize>(i) < d.nodes.size(); }

bool stretched(f32 lo, f32 hi) { return hi - lo > 1e-5f; }

// Widgets that can hold other widgets in a layout. Everything else is a leaf a designer drops
// things next to, not into.
bool isContainer(UiWidgetKind k) { return k == UiWidgetKind::Panel || k == UiWidgetKind::Scroll; }

UiWidgetId pickIn(const UiTree& t, const UiWidget& w, f32 x, f32 y) {
    if (!w.visible) return 0;
    const bool inside = w.rect.contains(x, y);
    if ((w.clip || w.kind == UiWidgetKind::Scroll || w.kind == UiWidgetKind::List) && !inside) return 0;
    for (usize i = w.children.size(); i-- > 0;)
        if (const UiWidget* k = t.get(w.children[i]))
            if (const UiWidgetId h = pickIn(t, *k, x, y)) return h;
    return inside ? w.id : 0;
}

} // namespace

// ---- canvas maths ---------------------------------------------------------------------------------

void uiEditorMoveBy(UiWidgetProps& p, f32 dx, f32 dy) {
    p.offsets.left += dx;
    if (stretched(p.anchors.minX, p.anchors.maxX)) p.offsets.right -= dx;
    p.offsets.top += dy;
    if (stretched(p.anchors.minY, p.anchors.maxY)) p.offsets.bottom -= dy;
}

void uiEditorMoveEdge(UiWidgetProps& p, UiEdge edge, f32 delta, f32 widthDesign, f32 heightDesign) {
    const bool horizontal = edge == UiEdge::Left || edge == UiEdge::Right;
    const bool stretch = horizontal ? stretched(p.anchors.minX, p.anchors.maxX) : stretched(p.anchors.minY, p.anchors.maxY);
    if (stretch) {
        switch (edge) {
            case UiEdge::Left: p.offsets.left += delta; break;
            case UiEdge::Right: p.offsets.right -= delta; break;
            case UiEdge::Top: p.offsets.top += delta; break;
            case UiEdge::Bottom: p.offsets.bottom -= delta; break;
        }
        return;
    }
    f32& size = horizontal ? p.width : p.height;
    f32& pos = horizontal ? p.offsets.left : p.offsets.top;
    const f32 pivot = horizontal ? p.pivot.x : p.pivot.y;
    if (size < 0.0f) size = horizontal ? widthDesign : heightDesign;
    const bool leading = edge == UiEdge::Left || edge == UiEdge::Top;
    const f32 newSize = leading ? size - delta : size + delta;
    if (newSize < 1.0f) return;
    // Position is anchor + offset - pivot * size: keep the opposite edge fixed as the size changes.
    pos += leading ? delta * (1.0f - pivot) : pivot * delta;
    size = newSize;
}

const char* uiAnchorPresetName(UiAnchorPreset p) {
    static const char* const names[] = {
        "Top left", "Top center", "Top right", "Middle left", "Center", "Middle right",
        "Bottom left", "Bottom center", "Bottom right",
        "Stretch top", "Stretch bottom", "Stretch left", "Stretch right",
        "Stretch horizontal", "Stretch vertical", "Stretch all",
    };
    const usize i = static_cast<usize>(p);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

void uiEditorApplyAnchorPreset(UiWidgetProps& p, UiAnchorPreset preset, const UiRect& parent, const UiRect& rect) {
    UiAnchors a{};
    UiVec2 pivot{};
    const auto point = [&](f32 x, f32 y) { a = UiAnchors{x, y, x, y}; pivot = {x, y}; };
    switch (preset) {
        case UiAnchorPreset::TopLeft: point(0, 0); break;
        case UiAnchorPreset::TopCenter: point(0.5f, 0); break;
        case UiAnchorPreset::TopRight: point(1, 0); break;
        case UiAnchorPreset::MiddleLeft: point(0, 0.5f); break;
        case UiAnchorPreset::Center: point(0.5f, 0.5f); break;
        case UiAnchorPreset::MiddleRight: point(1, 0.5f); break;
        case UiAnchorPreset::BottomLeft: point(0, 1); break;
        case UiAnchorPreset::BottomCenter: point(0.5f, 1); break;
        case UiAnchorPreset::BottomRight: point(1, 1); break;
        case UiAnchorPreset::StretchTop: a = UiAnchors{0, 0, 1, 0}; pivot = {0.5f, 0}; break;
        case UiAnchorPreset::StretchBottom: a = UiAnchors{0, 1, 1, 1}; pivot = {0.5f, 1}; break;
        case UiAnchorPreset::StretchLeft: a = UiAnchors{0, 0, 0, 1}; pivot = {0, 0.5f}; break;
        case UiAnchorPreset::StretchRight: a = UiAnchors{1, 0, 1, 1}; pivot = {1, 0.5f}; break;
        case UiAnchorPreset::StretchHorizontal: a = UiAnchors{0, 0.5f, 1, 0.5f}; pivot = {0.5f, 0.5f}; break;
        case UiAnchorPreset::StretchVertical: a = UiAnchors{0.5f, 0, 0.5f, 1}; pivot = {0.5f, 0.5f}; break;
        case UiAnchorPreset::StretchAll: a = UiAnchors{0, 0, 1, 1}; pivot = {0.5f, 0.5f}; break;
        case UiAnchorPreset::Count: return;
    }
    p.anchors = a;
    p.pivot = pivot;

    const f32 ax0 = parent.x + a.minX * parent.w, ax1 = parent.x + a.maxX * parent.w;
    const f32 ay0 = parent.y + a.minY * parent.h, ay1 = parent.y + a.maxY * parent.h;
    if (stretched(a.minX, a.maxX)) {
        p.offsets.left = rect.x - ax0;
        p.offsets.right = ax1 - rect.right();
    } else {
        p.width = rect.w;
        p.offsets.left = rect.x + pivot.x * rect.w - ax0;
    }
    if (stretched(a.minY, a.maxY)) {
        p.offsets.top = rect.y - ay0;
        p.offsets.bottom = ay1 - rect.bottom();
    } else {
        p.height = rect.h;
        p.offsets.top = rect.y + pivot.y * rect.h - ay0;
    }
}

UiWidgetId uiEditorPick(const UiTree& tree, f32 x, f32 y) {
    const std::vector<UiWidgetId> order = tree.sortedRoots();
    for (usize i = order.size(); i-- > 0;)
        if (const UiWidget* r = tree.get(order[i]))
            if (const UiWidgetId h = pickIn(tree, *r, x, y)) return h;
    return 0;
}

std::string uiStarterLayoutText() { return uiWriteLayout(uiStarterLayout()); }

// ================================================================================== the tab =======

UiLayoutEditor::UiLayoutEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void UiLayoutEditor::loadFromDisk() {
    std::string text;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "cannot read " + path_;
        return;
    }
    UiLayoutDoc d;
    std::string why;
    if (!uiParseLayout(text, d, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    doc_ = std::move(d);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selected_ = 0;
    history_.clear();
    rebuildPreview();
}

std::string UiLayoutEditor::title() const {
    // No manual dirty marker: the host adds ImGuiWindowFlags_UnsavedDocument for a dirty editor.
    return std::filesystem::path(path_).filename().string() + "###ui:" + path_;
}

bool UiLayoutEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    std::string invalid;
    if (!doc_.valid(&invalid)) {
        if (why) *why = invalid;
        return false;
    }
    if (!writeFileTextAtomic(path_, uiWriteLayout(doc_))) {
        if (why) *why = "cannot write " + path_;
        return false;
    }
    dirty_ = false;
    return true;
}

void UiLayoutEditor::onFileChanged() {
    // A dirty tab keeps its edits; a clean one reloads (see BtEditor::onFileChanged).
    if (dirty_) {
        AVER_WARN("[UiLayoutEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void UiLayoutEditor::select(i32 index) { selected_ = inRange(doc_, index) ? index : 0; }

void UiLayoutEditor::pushUndo() { history_.push(doc_); }

void UiLayoutEditor::undo() {
    if (!history_.undo(doc_)) return;
    dirty_ = true;
    if (!inRange(doc_, selected_)) selected_ = 0;
    rebuildPreview();
}

void UiLayoutEditor::redo() {
    if (!history_.redo(doc_)) return;
    dirty_ = true;
    if (!inRange(doc_, selected_)) selected_ = 0;
    rebuildPreview();
}

// ---- edits ------------------------------------------------------------------------------------------

i32 UiLayoutEditor::addWidget(UiWidgetKind kind) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 added = uiLayoutAdd(doc_, inRange(doc_, selected_) ? selected_ : 0, kind);
    if (added < 0) { history_.cancelPush(); return -1; }
    selected_ = added;
    dirty_ = true;
    rebuildPreview();
    return added;
}

void UiLayoutEditor::deleteSelected() {
    if (!loaded_ || selected_ == 0) return;
    pushUndo();
    const i32 next = uiLayoutDelete(doc_, selected_);
    if (next < 0) { history_.cancelPush(); return; }
    selected_ = next;
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::duplicateSelected() {
    if (!loaded_ || selected_ == 0) return;
    pushUndo();
    const i32 copy = uiLayoutDuplicate(doc_, selected_);
    if (copy < 0) { history_.cancelPush(); return; }
    // A copy exactly on top of the original reads as nothing happening: nudge it, where position matters.
    const i32 parent = doc_.nodes[static_cast<usize>(copy)].parent;
    if (parent >= 0 && doc_.nodes[static_cast<usize>(parent)].props.layout == UiLayoutMode::None)
        uiEditorMoveBy(doc_.nodes[static_cast<usize>(copy)].props, 16.0f, 16.0f);
    selected_ = copy;
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::reparentSelected(i32 newParent) {
    if (!loaded_) return;
    pushUndo();
    const i32 moved = uiLayoutReparent(doc_, selected_, newParent, -1);
    if (moved < 0) { history_.cancelPush(); return; }
    selected_ = moved;
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::moveSelected(i32 delta) {
    if (!loaded_) return;
    pushUndo();
    const i32 moved = uiLayoutMoveSibling(doc_, selected_, delta);
    if (moved < 0) { history_.cancelPush(); return; }
    selected_ = moved;
    dirty_ = true;
    rebuildPreview();
}

bool UiLayoutEditor::renameSelected(const std::string& name) {
    if (!loaded_ || !inRange(doc_, selected_) || name.empty()) return false;
    const std::string old = doc_.nodes[static_cast<usize>(selected_)].props.name;
    if (old == name) return true;
    const i32 existing = uiLayoutFind(doc_, name);
    if (existing >= 0 && existing != selected_) return false;
    pushUndo();
    uiLayoutRename(doc_, selected_, name);
    // References by name follow the rename.
    for (UiLayoutNode& n : doc_.nodes) {
        for (std::string* ref : {&n.props.navUp, &n.props.navDown, &n.props.navLeft, &n.props.navRight, &n.props.defaultFocus})
            if (*ref == old) *ref = name;
    }
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::setProperty(i32 index, std::string_view key, std::string_view text) {
    if (!loaded_ || !inRange(doc_, index)) return false;
    UiWidgetProps& p = doc_.nodes[static_cast<usize>(index)].props;
    std::string before;
    if (!uiGetProp(p, key, before)) return false;
    pushUndo();
    if (!uiSetProp(p, key, text)) { history_.cancelPush(); return false; }
    std::string after;
    uiGetProp(p, key, after);
    if (after == before) { history_.cancelPush(); return true; }   // no change, no undo entry
    dirty_ = true;
    rebuildPreview();
    return true;
}

void UiLayoutEditor::setTheme(const std::string& name) {
    if (!loaded_ || doc_.theme == name) return;
    pushUndo();
    doc_.theme = name;
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::setScaling(const UiDpi& dpi) {
    if (!loaded_) return;
    if (doc_.dpi.mode == dpi.mode && doc_.dpi.refWidth == dpi.refWidth && doc_.dpi.refHeight == dpi.refHeight) return;
    pushUndo();
    doc_.dpi.mode = dpi.mode;
    doc_.dpi.refWidth = dpi.refWidth;
    doc_.dpi.refHeight = dpi.refHeight;
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::applyAnchorPreset(UiAnchorPreset preset) {
    if (!loaded_ || !inRange(doc_, selected_)) return;
    const UiRect rect = designRect(selected_);
    const UiRect parent = designRectOfParent(selected_);
    pushUndo();
    uiEditorApplyAnchorPreset(doc_.nodes[static_cast<usize>(selected_)].props, preset, parent, rect);
    dirty_ = true;
    rebuildPreview();
}

i32 UiLayoutEditor::placeWidgetAt(UiWidgetKind kind, f32 px, f32 py) {
    if (!loaded_) return -1;
    i32 parent = nodeOf(uiEditorPick(preview_, px, py));
    if (parent < 0) parent = 0;
    while (parent > 0 && !isContainer(doc_.nodes[static_cast<usize>(parent)].props.kind))
        parent = doc_.nodes[static_cast<usize>(parent)].parent;
    pushUndo();
    const i32 added = uiLayoutAdd(doc_, parent, kind);
    if (added < 0) { history_.cancelPush(); return -1; }
    const UiWidgetProps& pp = doc_.nodes[static_cast<usize>(parent)].props;
    if (pp.layout == UiLayoutMode::None) {
        const UiWidgetId pid = previewWidget(parent);
        const UiRect content = preview_.contentRect(pid);
        const f32 s = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
        UiWidgetProps& c = doc_.nodes[static_cast<usize>(added)].props;
        c.offsets.left = (px - content.x) / s;
        c.offsets.top = (py - content.y) / s;
    }
    selected_ = added;
    dirty_ = true;
    rebuildPreview();
    return added;
}

void UiLayoutEditor::dragSelected(f32 dx, f32 dy, bool begin) {
    if (!loaded_ || selected_ <= 0 || !inRange(doc_, selected_)) return;
    if (begin) { pushUndo(); dragUndoPushed_ = true; dragAccumX_ = dragAccumY_ = 0.0f; }
    const f32 s = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
    UiLayoutNode& node = doc_.nodes[static_cast<usize>(selected_)];
    const UiWidgetProps& parent = doc_.nodes[static_cast<usize>(node.parent)].props;

    if (parent.layout == UiLayoutMode::None) {
        uiEditorMoveBy(node.props, dx / s, dy / s);
        dirty_ = true;
        rebuildPreview();
        return;
    }

    // Inside a stack or grid the position is not the widget's to choose: dragging reorders instead.
    dragAccumX_ += dx;
    dragAccumY_ += dy;
    const UiRect r = previewRect(selected_);
    const bool vertical = parent.layout == UiLayoutMode::VStack;
    const bool grid = parent.layout == UiLayoutMode::Grid;
    f32& acc = (vertical || (grid && std::fabs(dragAccumY_) > std::fabs(dragAccumX_))) ? dragAccumY_ : dragAccumX_;
    const f32 extent = std::max(1.0f, (&acc == &dragAccumY_) ? r.h : r.w);
    const i32 steps = (grid && &acc == &dragAccumY_) ? std::max(1, parent.columns) : 1;
    while (std::fabs(acc) > extent * 0.6f) {
        const i32 dir = acc > 0.0f ? 1 : -1;
        i32 moved = selected_;
        for (i32 k = 0; k < steps && moved >= 0; ++k) {
            const i32 next = uiLayoutMoveSibling(doc_, moved, dir);
            if (next < 0) break;
            moved = next;
        }
        if (moved == selected_) { acc = 0.0f; break; }
        selected_ = moved;
        acc -= static_cast<f32>(dir) * extent;
        dirty_ = true;
        rebuildPreview();
    }
}

void UiLayoutEditor::dragSelectedEdge(UiEdge edge, f32 delta, bool begin) {
    if (!loaded_ || !inRange(doc_, selected_)) return;
    if (begin) { pushUndo(); dragUndoPushed_ = true; }
    const f32 s = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
    const UiRect r = designRect(selected_);
    uiEditorMoveEdge(doc_.nodes[static_cast<usize>(selected_)].props, edge, delta / s, r.w, r.h);
    dirty_ = true;
    rebuildPreview();
}

// ---- the preview -------------------------------------------------------------------------------------------

void UiLayoutEditor::setPreviewSize(f32 w, f32 h) {
    previewW_ = std::max(64.0f, w);
    previewH_ = std::max(64.0f, h);
    preview_.setViewport({0, 0, previewW_, previewH_});
    preview_.layout(metrics_ ? *metrics_ : static_cast<const UiTextMetrics&>(estimate_));
}

void UiLayoutEditor::layoutPreview(const UiTextMetrics& m) { preview_.layout(m); }

void UiLayoutEditor::rebuildPreview() {
    ++structureVersion_;
    preview_.clear();
    preview_.setTheme(uiBuiltinTheme(doc_.theme));
    preview_.setDpi(doc_.dpi);
    preview_.setViewport({0, 0, previewW_, previewH_});
    previewIds_.assign(doc_.nodes.size(), 0);
    for (usize i = 0; i < doc_.nodes.size(); ++i) {
        const UiLayoutNode& n = doc_.nodes[i];
        const UiWidgetId par = n.parent < 0 ? 0 : previewIds_[static_cast<usize>(n.parent)];
        previewIds_[i] = preview_.createFrom(n.props, par);
    }
    preview_.layout(metrics_ ? *metrics_ : static_cast<const UiTextMetrics&>(estimate_));
}

UiWidgetId UiLayoutEditor::previewWidget(i32 index) const {
    return (index >= 0 && static_cast<usize>(index) < previewIds_.size()) ? previewIds_[static_cast<usize>(index)] : 0;
}

UiRect UiLayoutEditor::previewRect(i32 index) const {
    const UiWidget* w = preview_.get(previewWidget(index));
    return w ? w->rect : UiRect{};
}

UiRect UiLayoutEditor::designRect(i32 index) const {
    const UiRect r = previewRect(index);
    const f32 s = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
    return {r.x / s, r.y / s, r.w / s, r.h / s};
}

UiRect UiLayoutEditor::designRectOfParent(i32 index) const {
    const f32 s = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
    UiRect r{0, 0, previewW_, previewH_};
    if (inRange(doc_, index) && doc_.nodes[static_cast<usize>(index)].parent >= 0)
        r = preview_.contentRect(previewWidget(doc_.nodes[static_cast<usize>(index)].parent));
    return {r.x / s, r.y / s, r.w / s, r.h / s};
}

i32 UiLayoutEditor::nodeOf(UiWidgetId id) const {
    if (id == 0) return -1;
    for (usize i = 0; i < previewIds_.size(); ++i)
        if (previewIds_[i] == id) return static_cast<i32>(i);
    return -1;
}

#if AVER_WITH_IMGUI

// =============================================================================== drawing ==========

namespace {

constexpr const char* kPrefLeftSplit = "uiLayoutEditor.leftSplit";
constexpr const char* kPrefInspectorSplit = "uiLayoutEditor.inspectorSplit";
constexpr f32 kDefaultLeftFraction = 0.22f;
constexpr f32 kDefaultCanvasFraction = 0.70f;

// ImGui's font as the preview's metrics. Sizes here are PREVIEW pixels; the painter scales to screen.
class PreviewMetrics : public UiTextMetrics {
public:
    f32 textWidth(std::string_view t, f32 px) const override {
        if (t.empty()) return 0.0f;
        return ImGui::GetFont()->CalcTextSizeA(px, 1.0e9f, 0.0f, t.data(), t.data() + t.size()).x;
    }
    f32 lineHeight(f32 px) const override { return px * 1.25f; }
    f32 ascent(f32 px) const override { return px * 0.95f; }
};

// Paints a tree into an ImDrawList at a zoom and origin. The draw list's colour packing is the UI's.
class PreviewPainter final : public UiPainter {
public:
    PreviewPainter(ImDrawList* dl, ImVec2 origin, f32 zoom) : dl_(dl), origin_(origin), zoom_(zoom) {}

    void pushClip(const UiRect& r) override { dl_->PushClipRect(at(r.x, r.y), at(r.right(), r.bottom()), true); }
    void popClip() override { dl_->PopClipRect(); }
    void fillRect(const UiRect& r, u32 rgba) override {
        if (r.empty()) return;
        dl_->AddRectFilled(at(r.x, r.y), at(r.right(), r.bottom()), rgba);
    }
    void image(const UiRect& r, u64 texture, const UiUvRect&, u32 tint) override {
        (void)texture;
        if (r.empty()) return;
        dl_->AddRectFilled(at(r.x, r.y), at(r.right(), r.bottom()), uiWithOpacity(tint, 0.6f));
    }
    void text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) override {
        if (s.empty()) return;
        // The tree's baseline is lineTop + ascent; ImGui wants the top of the glyph box.
        const f32 top = baselineY - ascent(px) + px * 0.12f;
        dl_->AddText(ImGui::GetFont(), px * zoom_, at(x, top), rgba, s.data(), s.data() + s.size());
    }
    f32 textWidth(std::string_view t, f32 px) const override { return metrics_.textWidth(t, px); }
    f32 lineHeight(f32 px) const override { return metrics_.lineHeight(px); }
    f32 ascent(f32 px) const override { return metrics_.ascent(px); }

private:
    ImVec2 at(f32 x, f32 y) const { return ImVec2(origin_.x + x * zoom_, origin_.y + y * zoom_); }
    ImDrawList* dl_;
    ImVec2 origin_;
    f32 zoom_;
    PreviewMetrics metrics_;
};

const char* const kKindLabels[] = {"Panel", "Text", "Image", "Button", "Toggle", "Slider",
                                   "Choice", "List", "Scroll", "Text input", "Progress bar", "Key bind"};

ImVec4 colourVec(u32 rgba) {
    return ImVec4(static_cast<f32>(rgba & 0xFF) / 255.0f, static_cast<f32>((rgba >> 8) & 0xFF) / 255.0f,
                  static_cast<f32>((rgba >> 16) & 0xFF) / 255.0f, static_cast<f32>(rgba >> 24) / 255.0f);
}

u32 colourPack(const f32 c[4]) {
    const auto b = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return uiRgba(b(c[0]), b(c[1]), b(c[2]), b(c[3]));
}

u32 parseColourToken(const std::string& t) {
    if (t == "none" || t.size() < 7) return 0;
    u32 v[4] = {0, 0, 0, 255};
    for (usize i = 0; i < (t.size() - 1) / 2 && i < 4; ++i)
        v[i] = static_cast<u32>(std::strtoul(t.substr(1 + i * 2, 2).c_str(), nullptr, 16));
    return uiRgba(v[0], v[1], v[2], v[3]);
}

std::string colourText(u32 rgba) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, rgba >> 24);
    return buf;
}

} // namespace

void UiLayoutEditor::propEditBegin() {
    if (propEditing_) return;
    pushUndo();
    propEditing_ = true;
    propEdited_ = false;
}

void UiLayoutEditor::propEditEnd() {
    if (!propEditing_) return;
    if (!propEdited_) history_.cancelPush();
    propEditing_ = false;
}

bool UiLayoutEditor::drawProperty(const UiPropDesc& d) {
    UiWidgetProps& p = doc_.nodes[static_cast<usize>(selected_)].props;
    std::vector<std::string> tokens;
    d.get(p, tokens);

    bool changed = false;
    std::string newText;
    bool immediate = false;    // a click rather than a drag: the edit begins and ends this frame
    const auto fl = [](const std::string& s) { f32 v = 0; uiParseFloat(s, v); return v; };

    ImGui::PushID(d.key);
    ImGui::TextUnformatted(d.key);
    ImGui::SetNextItemWidth(-1);
    switch (d.type) {
        case UiPropType::Float: {
            f32 v = fl(tokens[0]);
            const f32 speed = (d.maxV > d.minV) ? std::max(0.001f, (d.maxV - d.minV) / 400.0f) : 0.5f;
            if (d.maxV > d.minV) changed = ImGui::DragFloat("##v", &v, speed, d.minV, d.maxV, "%.3f");
            else changed = ImGui::DragFloat("##v", &v, 0.01f);
            if (changed) newText = uiFormatFloat(v);
            break;
        }
        case UiPropType::Int: {
            int v = std::atoi(tokens[0].c_str());
            changed = (d.maxV > d.minV) ? ImGui::DragInt("##v", &v, 0.2f, static_cast<int>(d.minV), static_cast<int>(d.maxV))
                                        : ImGui::DragInt("##v", &v);
            if (changed) newText = std::to_string(v);
            break;
        }
        case UiPropType::Bool: {
            bool v = tokens[0] == "true";
            changed = ImGui::Checkbox("##v", &v);
            immediate = true;
            if (changed) newText = v ? "true" : "false";
            break;
        }
        case UiPropType::String: {
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s", tokens.empty() ? "" : tokens[0].c_str());
            if (std::strcmp(d.key, "text") == 0)
                changed = ImGui::InputTextMultiline("##v", buf, sizeof buf, ImVec2(-1, ImGui::GetTextLineHeight() * 3.5f));
            else
                changed = ImGui::InputText("##v", buf, sizeof buf);
            if (changed) newText = uiQuote(buf);
            break;
        }
        case UiPropType::Color: {
            const u32 cur = parseColourToken(tokens[0]);
            f32 c[4];
            const ImVec4 cv = colourVec(cur);
            c[0] = cv.x; c[1] = cv.y; c[2] = cv.z; c[3] = cv.w;
            if (cur == 0) { c[0] = c[1] = c[2] = 1.0f; c[3] = 1.0f; }
            changed = ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_AlphaBar);
            if (changed) newText = colourText(colourPack(c));
            immediate = true;
            if (cur != 0 && ImGui::SmallButton("clear")) { changed = true; newText = "none"; }
            break;
        }
        case UiPropType::Enum: {
            const std::string current = tokens[0];
            if (ImGui::BeginCombo("##v", current.c_str())) {
                for (const char* name : d.enumNames) {
                    if (ImGui::Selectable(name, current == name)) { changed = true; newText = name; }
                }
                ImGui::EndCombo();
            }
            immediate = true;
            break;
        }
        case UiPropType::Insets:
        case UiPropType::Anchors:
        case UiPropType::Uv: {
            f32 v[4] = {fl(tokens[0]), fl(tokens[1]), fl(tokens[2]), fl(tokens[3])};
            changed = ImGui::DragFloat4("##v", v, d.type == UiPropType::Insets ? 0.5f : 0.01f);
            if (changed) newText = uiFormatFloat(v[0]) + " " + uiFormatFloat(v[1]) + " " + uiFormatFloat(v[2]) + " " + uiFormatFloat(v[3]);
            break;
        }
        case UiPropType::Vec2: {
            f32 v[2] = {fl(tokens[0]), fl(tokens[1])};
            changed = ImGui::DragFloat2("##v", v, 0.01f);
            if (changed) newText = uiFormatFloat(v[0]) + " " + uiFormatFloat(v[1]);
            break;
        }
        case UiPropType::StringList: {
            std::string joined;
            for (usize i = 0; i < tokens.size(); ++i) { if (i) joined += '\n'; joined += tokens[i]; }
            char buf[1024];
            std::snprintf(buf, sizeof buf, "%s", joined.c_str());
            changed = ImGui::InputTextMultiline("##v", buf, sizeof buf, ImVec2(-1, ImGui::GetTextLineHeight() * 4.5f));
            if (changed) {
                std::string line;
                for (const char* c = buf;; ++c) {
                    if (*c == '\n' || *c == '\0') {
                        if (!line.empty() || *c == '\n') newText += (newText.empty() ? "" : " ") + uiQuote(line);
                        line.clear();
                        if (*c == '\0') break;
                    } else {
                        line.push_back(*c);
                    }
                }
            }
            break;
        }
    }

    if (ImGui::IsItemActivated()) propEditBegin();
    if (changed) {
        propEditBegin();
        if (uiSetProp(p, d.key, newText)) {
            propEdited_ = true;
            dirty_ = true;
            rebuildPreview();
        }
    }
    if (immediate && changed) propEditEnd();
    if (ImGui::IsItemDeactivated()) propEditEnd();
    ImGui::PopID();
    return changed;
}

void UiLayoutEditor::drawInspector() {
    if (!inRange(doc_, selected_)) { ImGui::TextUnformatted("Nothing selected"); return; }
    // Re-fetched after every call that can renumber the nodes: a reference would dangle.
    const auto cur = [this]() -> UiWidgetProps& { return doc_.nodes[static_cast<usize>(selected_)].props; };

    char name[128];
    std::snprintf(name, sizeof name, "%s", cur().name.c_str());
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", name, sizeof name);
    if (ImGui::IsItemDeactivatedAfterEdit() && name != cur().name) {
        if (!renameSelected(name)) ImGui::OpenPopup("##nametaken");
    }
    if (ImGui::BeginPopup("##nametaken")) { ImGui::TextUnformatted("That name is empty or already used."); ImGui::EndPopup(); }
    ImGui::TextDisabled("%s", uiKindName(cur().kind));

    if (selected_ != 0) {
        const i32 parent = doc_.nodes[static_cast<usize>(selected_)].parent;
        if (ImGui::BeginCombo("Attach to", doc_.nodes[static_cast<usize>(parent)].props.name.c_str())) {
            for (i32 i = 0; i < static_cast<i32>(doc_.nodes.size()); ++i) {
                // Never offer this widget or one of its descendants: that is the cycle guard, shown.
                bool inside = false;
                for (i32 w = i; w >= 0; w = doc_.nodes[static_cast<usize>(w)].parent) if (w == selected_) { inside = true; break; }
                if (inside) continue;
                if (ImGui::Selectable(doc_.nodes[static_cast<usize>(i)].props.name.c_str(), i == parent)) reparentSelected(i);
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Anchor preset", "choose...")) {
            for (u32 a = 0; a < static_cast<u32>(UiAnchorPreset::Count); ++a)
                if (ImGui::Selectable(uiAnchorPresetName(static_cast<UiAnchorPreset>(a)))) applyAnchorPreset(static_cast<UiAnchorPreset>(a));
            ImGui::EndCombo();
        }
    } else {
        ImGui::Separator();
        ImGui::TextUnformatted("Document");
        if (ImGui::BeginCombo("Theme", doc_.theme.c_str())) {
            for (const char* const* t = uiBuiltinThemeNames(); *t; ++t)
                if (ImGui::Selectable(*t, doc_.theme == *t)) setTheme(*t);
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Scaling", uiScaleModeName(doc_.dpi.mode))) {
            for (u32 m = 0; m < 5; ++m) {
                if (ImGui::Selectable(uiScaleModeName(static_cast<UiScaleMode>(m)), static_cast<u32>(doc_.dpi.mode) == m)) {
                    UiDpi d = doc_.dpi;
                    d.mode = static_cast<UiScaleMode>(m);
                    setScaling(d);
                }
            }
            ImGui::EndCombo();
        }
        f32 ref[2] = {doc_.dpi.refWidth, doc_.dpi.refHeight};
        if (ImGui::DragFloat2("Reference size", ref, 1.0f, 64.0f, 8192.0f, "%.0f") && ref[0] > 0 && ref[1] > 0) {
            UiDpi d = doc_.dpi;
            d.refWidth = ref[0];
            d.refHeight = ref[1];
            setScaling(d);
        }
    }

    const char* const groups[] = {"Placement", "Layout", "Appearance", "Content", "Value", "Navigation", "Root"};
    for (const char* g : groups) {
        std::vector<const UiPropDesc*> shown;
        for (const UiPropDesc& d : uiProperties()) {
            if (std::strcmp(d.group, g) != 0) continue;
            if (!(d.kindMask & uiKindBit(cur().kind))) continue;
            if (d.rootOnly && selected_ != 0) continue;
            shown.push_back(&d);
        }
        if (shown.empty()) continue;
        const bool open = std::strcmp(g, "Placement") == 0 || std::strcmp(g, "Content") == 0 || std::strcmp(g, "Value") == 0;
        if (ImGui::CollapsingHeader(g, open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            for (const UiPropDesc* d : shown) drawProperty(*d);
    }
}

void UiLayoutEditor::drawHierarchyRow(i32 index) {
    const std::vector<i32> children = uiLayoutChildren(doc_, index);
    const UiWidgetProps& p = doc_.nodes[static_cast<usize>(index)].props;
    const u32 version = structureVersion_;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected_ == index) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(index);
    const bool open = ImGui::TreeNodeEx("##row", flags, "%s  (%s)", p.name.c_str(), uiKindName(p.kind));
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(index);
    if (index != 0 && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("AVER_UI_NODE", &index, sizeof index);
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_NODE")) {
            const i32 src = *static_cast<const i32*>(pl->Data);
            select(src);
            reparentSelected(index);
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_KIND")) {
            select(index);
            addWidget(*static_cast<const UiWidgetKind*>(pl->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##rowmenu")) {
        select(index);
        if (ImGui::MenuItem("Duplicate", nullptr, false, index != 0)) duplicateSelected();
        if (ImGui::MenuItem("Delete", nullptr, false, index != 0)) deleteSelected();
        ImGui::EndPopup();
    }
    ImGui::PopID();

    // An edit above renumbers the nodes, so `children` is stale: close this row and let the next
    // frame draw the new shape.
    if (version != structureVersion_) {
        if (open && !children.empty()) ImGui::TreePop();
        return;
    }
    if (open && !children.empty()) {
        for (const i32 c : children) drawHierarchyRow(c);
        ImGui::TreePop();
    }
}

void UiLayoutEditor::drawHierarchy() { drawHierarchyRow(0); }

void UiLayoutEditor::drawPalette() {
    ImGui::TextDisabled("Drag onto the canvas or the hierarchy");
    for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k) {
        const UiWidgetKind kind = static_cast<UiWidgetKind>(k);
        ImGui::Selectable(kKindLabels[k]);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) addWidget(kind);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("AVER_UI_KIND", &kind, sizeof kind);
            ImGui::Text("New %s", kKindLabels[k]);
            ImGui::EndDragDropSource();
        }
    }
}

void UiLayoutEditor::drawToolbar() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (ImGui::Button("Save") || (focused && keybinds().pressed(CommandId::AssetSave, io))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[UiLayoutEditor] save failed for '{}': {}", path_, why);
    }
    if (focused && !io.WantTextInput) {
        if (canUndo() && keybinds().pressed(CommandId::EditUndo, io)) undo();
        if (canRedo() && keybinds().pressed(CommandId::EditRedo, io)) redo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", "Add child")) {
        for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k)
            if (ImGui::Selectable(kKindLabels[k])) addWidget(static_cast<UiWidgetKind>(k));
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ == 0);
    if (ImGui::Button("Duplicate")) duplicateSelected();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ <= 0);
    if (ImGui::Button("Up")) moveSelected(-1);
    ImGui::SameLine();
    if (ImGui::Button("Down")) moveSelected(1);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    char sizeLabel[32];
    std::snprintf(sizeLabel, sizeof sizeLabel, "%.0fx%.0f", previewW_, previewH_);
    if (ImGui::BeginCombo("##size", sizeLabel)) {
        const f32 sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 720}, {800, 600}, {1080, 1920}};
        for (const auto& s : sizes) {
            char l[32];
            std::snprintf(l, sizeof l, "%.0fx%.0f", s[0], s[1]);
            if (ImGui::Selectable(l)) setPreviewSize(s[0], s[1]);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    const char* zoomLabels[] = {"Fit", "25%", "50%", "75%", "100%", "150%", "200%"};
    const i32 zoomValues[] = {0, 25, 50, 75, 100, 150, 200};
    const char* current = "Fit";
    for (int i = 0; i < 7; ++i) if (zoomValues[i] == zoomPercent_) current = zoomLabels[i];
    if (ImGui::BeginCombo("##zoom", current)) {
        for (int i = 0; i < 7; ++i) if (ImGui::Selectable(zoomLabels[i], zoomValues[i] == zoomPercent_)) zoomPercent_ = zoomValues[i];
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Interact", &interact_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run the preview like a player: hover, click, drag sliders, navigate.");
    if (interact_ && !lastEvent_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", lastEvent_.c_str()); }
}

void UiLayoutEditor::drawCanvas() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    if (avail.x < 16.0f || avail.y < 16.0f) return;

    ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    const f32 fit = std::min((avail.x - 24.0f) / previewW_, (avail.y - 24.0f) / previewH_);
    const f32 zoom = std::clamp(zoomPercent_ > 0 ? static_cast<f32>(zoomPercent_) / 100.0f : fit, 0.05f, 4.0f);
    const ImVec2 origin(cursor.x + std::max(12.0f, (avail.x - previewW_ * zoom) * 0.5f),
                        cursor.y + std::max(12.0f, (avail.y - previewH_ * zoom) * 0.5f));
    const auto toPreview = [&](ImVec2 s) { return ImVec2((s.x - origin.x) / zoom, (s.y - origin.y) / zoom); };
    const auto toScreen = [&](f32 x, f32 y) { return ImVec2(origin.x + x * zoom, origin.y + y * zoom); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), true);
    dl->AddRectFilled(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), IM_COL32(28, 29, 33, 255));
    dl->AddRectFilled(toScreen(0, 0), toScreen(previewW_, previewH_), IM_COL32(54, 58, 66, 255));

    PreviewMetrics metrics;
    metrics_ = &metrics;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = toPreview(io.MousePos);

    if (interact_) {
        UiInputFrame in;
        in.pointerX = mouse.x;
        in.pointerY = mouse.y;
        in.pointerValid = hovered || active;
        in.buttons = (io.MouseDown[0] ? 1u : 0u) | (io.MouseDown[1] ? 2u : 0u);
        in.wheel = hovered ? io.MouseWheel : 0.0f;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) in.nav |= uiNavBit(UiNav::Up);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) in.nav |= uiNavBit(UiNav::Down);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { in.nav |= uiNavBit(UiNav::Left); in.editKeys.push_back(UiEditKey::Left); }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { in.nav |= uiNavBit(UiNav::Right); in.editKeys.push_back(UiEditKey::Right); }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter)) { in.nav |= uiNavBit(UiNav::Accept); in.editKeys.push_back(UiEditKey::Enter); }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) in.nav |= uiNavBit(UiNav::Cancel);
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) in.nav |= uiNavBit(io.KeyShift ? UiNav::PrevTab : UiNav::NextTab);
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i) in.chars.push_back(io.InputQueueCharacters[i]);
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) in.editKeys.push_back(UiEditKey::Backspace);
        preview_.update(io.DeltaTime, in, metrics);
        UiEvent e;
        while (preview_.pollEvent(e)) {
            if (e.type == UiEventType::FocusGained || e.type == UiEventType::FocusLost) continue;
            const UiWidget* w = preview_.get(e.widget);
            lastEvent_ = std::string("event: ") + (w ? w->name : "?") + " type " + std::to_string(static_cast<int>(e.type)) +
                         (e.text.empty() ? "" : " '" + e.text + "'");
        }
    } else {
        preview_.layout(metrics);
    }

    PreviewPainter painter(dl, origin, zoom);
    preview_.draw(painter);
    dl->AddRect(toScreen(0, 0), toScreen(previewW_, previewH_), IM_COL32(120, 126, 140, 255));

    if (!interact_ && loaded_) {
        // Hover and selection outlines, and the handles on the selection.
        const ImU32 accent = IM_COL32(86, 156, 255, 255);
        const UiWidgetId hoverId = hovered ? uiEditorPick(preview_, mouse.x, mouse.y) : 0;
        if (hoverId && nodeOf(hoverId) != selected_) {
            const UiWidget* h = preview_.get(hoverId);
            dl->AddRect(toScreen(h->rect.x, h->rect.y), toScreen(h->rect.right(), h->rect.bottom()), IM_COL32(86, 156, 255, 110));
        }
        const UiRect sel = previewRect(selected_);
        struct Handle { f32 x, y; i32 bits; ImGuiMouseCursor cursor; };
        Handle handles[8];
        int hn = 0;
        if (!sel.empty() && selected_ != 0) {
            const f32 cx = sel.x + sel.w * 0.5f, cy = sel.y + sel.h * 0.5f;
            handles[hn++] = {sel.x, sel.y, 1 | 4, ImGuiMouseCursor_ResizeNWSE};
            handles[hn++] = {cx, sel.y, 4, ImGuiMouseCursor_ResizeNS};
            handles[hn++] = {sel.right(), sel.y, 2 | 4, ImGuiMouseCursor_ResizeNESW};
            handles[hn++] = {sel.x, cy, 1, ImGuiMouseCursor_ResizeEW};
            handles[hn++] = {sel.right(), cy, 2, ImGuiMouseCursor_ResizeEW};
            handles[hn++] = {sel.x, sel.bottom(), 1 | 8, ImGuiMouseCursor_ResizeNESW};
            handles[hn++] = {cx, sel.bottom(), 8, ImGuiMouseCursor_ResizeNS};
            handles[hn++] = {sel.right(), sel.bottom(), 2 | 8, ImGuiMouseCursor_ResizeNWSE};
        }
        if (!sel.empty()) {
            dl->AddRect(toScreen(sel.x, sel.y), toScreen(sel.right(), sel.bottom()), accent, 0.0f, 0, 2.0f);
            for (int i = 0; i < hn; ++i) {
                const ImVec2 c = toScreen(handles[i].x, handles[i].y);
                dl->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), accent);
            }
        }

        // The anchor of a point-anchored selection, in its parent.
        if (selected_ > 0) {
            const UiWidgetProps& sp = doc_.nodes[static_cast<usize>(selected_)].props;
            const UiRect pc = preview_.contentRect(previewWidget(doc_.nodes[static_cast<usize>(selected_)].parent));
            const ImVec2 a = toScreen(pc.x + sp.anchors.minX * pc.w, pc.y + sp.anchors.minY * pc.h);
            dl->AddQuadFilled(ImVec2(a.x, a.y - 6), ImVec2(a.x + 6, a.y), ImVec2(a.x, a.y + 6), ImVec2(a.x - 6, a.y), IM_COL32(255, 200, 80, 255));
        }

        // Pointer handling.
        int cursorHandle = -1;
        for (int i = 0; i < hn; ++i) {
            const ImVec2 c = toScreen(handles[i].x, handles[i].y);
            if (std::fabs(io.MousePos.x - c.x) <= 6.0f && std::fabs(io.MousePos.y - c.y) <= 6.0f) cursorHandle = i;
        }
        if (hovered && cursorHandle >= 0 && canvasDrag_ == 0) ImGui::SetMouseCursor(handles[cursorHandle].cursor);

        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
            canvasDragStarted_ = false;
            if (cursorHandle >= 0) {
                canvasDrag_ = handles[cursorHandle].bits;
            } else {
                const i32 picked = nodeOf(uiEditorPick(preview_, mouse.x, mouse.y));
                if (picked >= 0) select(picked);
                canvasDrag_ = (picked >= 0 && picked != 0) ? 16 : 0;
            }
        }
        if (active && canvasDrag_ != 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            const f32 dx = io.MouseDelta.x / zoom, dy = io.MouseDelta.y / zoom;   // preview px
            const bool first = !canvasDragStarted_;
            canvasDragStarted_ = true;
            if (canvasDrag_ & 16) {
                dragSelected(dx, dy, first);
            } else {
                bool begin = first;
                if (canvasDrag_ & 1) { dragSelectedEdge(UiEdge::Left, dx, begin); begin = false; }
                if (canvasDrag_ & 2) { dragSelectedEdge(UiEdge::Right, dx, begin); begin = false; }
                if (canvasDrag_ & 4) { dragSelectedEdge(UiEdge::Top, dy, begin); begin = false; }
                if (canvasDrag_ & 8) { dragSelectedEdge(UiEdge::Bottom, dy, begin); }
            }
        }
        if (!active) { canvasDrag_ = 0; canvasDragStarted_ = false; }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_KIND")) {
                placeWidgetAt(*static_cast<const UiWidgetKind*>(pl->Data), mouse.x, mouse.y);
            }
            ImGui::EndDragDropTarget();
        }

        // Keyboard: delete, duplicate, nudge.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && selected_ > 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelected();
            else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelected();
            else {
                const f32 step = (io.KeyShift ? 10.0f : 1.0f) * zoom * preview_.scale();
                f32 dx = 0, dy = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx = -step;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx = step;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy = -step;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy = step;
                if (dx != 0.0f || dy != 0.0f) dragSelected(dx / zoom, dy / zoom, true);
            }
        }
    }

    dl->PopClipRect();
    metrics_ = nullptr;
}

void UiLayoutEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    drawToolbar();
    ImGui::Separator();

    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 leftW = splitPaneWidth(splitLeft_, kPrefLeftSplit, kDefaultLeftFraction, avail, 150.0f * dpi, 400.0f * dpi);
    if (ImGui::BeginChild("##uileft", ImVec2(leftW, 0), true)) {
        if (ImGui::BeginChild("##uipalette", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.4f), false)) drawPalette();
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::BeginChild("##uihier", ImVec2(0, 0), false)) drawHierarchy();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    drawSplitHandle(splitLeft_, "##uisplit1", kPrefLeftSplit, avail, 150.0f * dpi, 400.0f * dpi, 6.0f * dpi);

    const f32 rest = ImGui::GetContentRegionAvail().x;
    const f32 canvasW = splitPaneWidth(splitInspector_, kPrefInspectorSplit, kDefaultCanvasFraction, rest, 240.0f * dpi, 220.0f * dpi);
    if (ImGui::BeginChild("##uicanvas", ImVec2(canvasW, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        drawCanvas();
    ImGui::EndChild();
    drawSplitHandle(splitInspector_, "##uisplit2", kPrefInspectorSplit, rest, 240.0f * dpi, 220.0f * dpi, 6.0f * dpi);
    if (ImGui::BeginChild("##uiinspector", ImVec2(0, 0), true)) drawInspector();
    ImGui::EndChild();
}

void UiLayoutEditor::resetLayout() {
    resetSplitPane(splitLeft_, kPrefLeftSplit, kDefaultLeftFraction);
    resetSplitPane(splitInspector_, kPrefInspectorSplit, kDefaultCanvasFraction);
}

#else   // AVER_WITH_IMGUI

// The headless build still gets load/save/undo, the edits and the preview; only the window is absent.
void UiLayoutEditor::draw(Engine& e) { (void)e; }
void UiLayoutEditor::resetLayout() {}

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeUiLayoutEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocui") return nullptr;
    return std::make_unique<UiLayoutEditor>(path);
}

} // namespace aver::editor
