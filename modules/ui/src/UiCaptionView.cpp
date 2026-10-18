// The caption overlay: one passive Overlay root with a row per line. See UiAccess.hpp.
#include "aver/ui/UiAccess.hpp"

#include "aver/ui/UiTree.hpp"

#include <algorithm>

namespace aver::ui {

namespace {

constexpr f32 kBaseDp = 24.0f;
constexpr f32 kWidthFraction = 0.7f;
constexpr f32 kPadX = 12.0f, kPadY = 6.0f;

UiWidget* make(UiTree& t, UiWidgetKind kind, UiWidgetId parent, const char* name) {
    UiWidget* w = t.get(t.create(kind, parent, name));
    w->hit = UiHitMode::Never;
    w->focusable = false;
    return w;
}

} // namespace

void UiCaptionView::ensureRoot(UiTree& tree) {
    if (root_ && tree.get(root_)) return;
    root_ = 0;
    rows_.clear();
    UiWidget* r = make(tree, UiWidgetKind::Panel, 0, kRootName);
    root_ = r->id;
    r->style = "clear";
    r->inputMode = UiInputMode::Passive;
    r->layer = UiLayer::Overlay;
    r->layout = UiLayoutMode::VStack;
    r->spacing = 4;
    r->zOrder = 90;
}

UiCaptionView::Row UiCaptionView::makeRow(UiTree& tree) {
    Row row;
    UiWidget* p = make(tree, UiWidgetKind::Panel, root_, "Row");
    row.panel = p->id;
    p->layout = UiLayoutMode::VStack;
    p->spacing = 2;
    p->padding = UiInsets{kPadX, kPadY, kPadX, kPadY};
    UiWidget* h = make(tree, UiWidgetKind::Panel, row.panel, "Head");
    row.head = h->id;
    h->style = "clear";
    h->layout = UiLayoutMode::HStack;
    h->spacing = 6;
    row.pre = make(tree, UiWidgetKind::Text, row.head, "Pre")->id;
    UiWidget* n = make(tree, UiWidgetKind::Text, row.head, "Speaker");
    row.name = n->id;
    n->bold = true;
    row.post = make(tree, UiWidgetKind::Text, row.head, "Post")->id;
    UiWidget* b = make(tree, UiWidgetKind::Text, row.panel, "Body");
    row.body = b->id;
    b->wrap = true;
    return row;
}

void UiCaptionView::update(UiTree& tree, const std::vector<UiCaptionLine>& lines, const UiCaptionStyle& style) {
    if (lines.empty()) {
        if (root_) {
            if (tree.get(root_)) tree.setVisible(root_, false);
            else { root_ = 0; rows_.clear(); }
        }
        return;
    }
    ensureRoot(tree);
    UiWidget* root = tree.get(root_);
    root->visible = true;
    // Bottom or top centre, 70 percent of the width, 6 percent in from the edge.
    const f32 y = style.top ? 0.06f : 0.94f;
    root->anchors = UiAnchors{(1.0f - kWidthFraction) * 0.5f, y, 1.0f - (1.0f - kWidthFraction) * 0.5f, y};
    root->offsets = UiInsets{};
    root->pivot = UiVec2{0.5f, style.top ? 0.0f : 1.0f};

    const f32 scale = tree.scale() > 0.0f ? tree.scale() : 1.0f;
    const f32 designW = tree.viewport().w / scale * kWidthFraction;
    const f32 textW = std::max(40.0f, designW - 2.0f * kPadX);
    const f32 bodyPx = kBaseDp * style.sizeScale;
    const f32 bgOpacity = style.highContrast ? std::max(0.85f, style.bgOpacity) : style.bgOpacity;
    const u32 bgAlpha = std::max<u32>(1u, static_cast<u32>(std::clamp(bgOpacity, 0.0f, 1.0f) * 255.0f + 0.5f));   // alpha 0 would mean "unset"

    while (rows_.size() < lines.size()) rows_.push_back(makeRow(tree));
    for (usize i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        UiWidget* panel = tree.get(row.panel);
        if (!panel) continue;
        if (i >= lines.size()) { panel->visible = false; continue; }
        const UiCaptionLine& l = lines[i];
        const u32 colour = style.highContrast ? 0xFFFFFFFFu : l.rgba;
        panel->visible = true;
        panel->opacity = std::clamp(l.alpha, 0.0f, 1.0f);
        panel->bgColor = uiRgba(0, 0, 0, bgAlpha);

        UiWidget* head = tree.get(row.head);
        head->visible = !l.speaker.empty() || l.dir != 0;
        UiWidget* pre = tree.get(row.pre);
        UiWidget* name = tree.get(row.name);
        UiWidget* post = tree.get(row.post);
        pre->text = l.dir == -1 ? "<" : (l.dir == 2 ? "v" : "");
        post->text = l.dir == 1 ? ">" : "";
        name->text = l.speaker;
        for (UiWidget* t : {pre, name, post}) {
            t->textColor = colour;
            t->fontSize = bodyPx * 0.8f;
        }
        UiWidget* body = tree.get(row.body);
        body->text = l.text;
        body->textColor = colour;
        body->fontSize = bodyPx;
        body->width = textW;
    }
}

} // namespace aver::ui
