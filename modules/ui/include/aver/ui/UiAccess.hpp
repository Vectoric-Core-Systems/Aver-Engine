#pragma once
// Accessibility for the retained UI: the tree-wide options (text scale, high contrast), the spoken form of a
// widget (uiAccessibleText), and the caption overlay (UiCaptionView). Core-only like the rest of Aver.UI.
// docs/ACCESSIBILITY.md sections 2.2, 2.4, 2.7.
#include "aver/ui/UiWidget.hpp"

#include <string>
#include <vector>

namespace aver::ui {

class UiTree;

// Set with UiTree::setAccess. The default is today's behaviour exactly.
struct UiAccess {
    f32 textScale = 1.0f;         // multiplies font px only; row heights, padding and widths stay on the UI scale
    bool highContrast = false;    // the "contrast" theme, ignoring per-widget colour overrides
    bool operator==(const UiAccess& o) const { return textScale == o.textScale && highContrast == o.highContrast; }
};

// What a screen reader would say for a widget: "name, role, state or value", joined with ", ". Parts are
// localization references with English defaults (@Access:role.button=button), resolved through the tree's resolver.
// A masked field's content is never read. Empty for an unknown widget.
std::string uiAccessibleText(const UiTree& tree, UiWidgetId id);

// ---- captions ---------------------------------------------------------------------------------------------

struct UiCaptionLine {
    std::string speaker;     // raw text or reference; empty = none shown
    std::string text;        // raw text or reference
    u32 rgba = 0xFFFFFFFFu;  // UI packing (uiRgba): 0xAABBGGRR
    i32 dir = 0;             // -1 left, 0 none, 1 right, 2 behind
    f32 alpha = 1.0f;
};

struct UiCaptionStyle {
    f32 sizeScale = 1.0f;    // caption size preset multiplier on the 24 dp base
    f32 bgOpacity = 0.6f;
    bool top = false;        // false = bottom of the screen
    bool highContrast = false;   // background opacity floors at 0.85, text white
};

// One passive Overlay root ("Aver.Captions") with a row per line. Rows are recycled, so an unchanged line set causes
// no widget churn; an empty list hides the root. Text stays as raw references: the tree localizes it.
class UiCaptionView {
public:
    static constexpr const char* kRootName = "Aver.Captions";

    void update(UiTree& tree, const std::vector<UiCaptionLine>& lines, const UiCaptionStyle& style);
    UiWidgetId root() const { return root_; }
    usize rowCount() const { return rows_.size(); }

private:
    struct Row { UiWidgetId panel = 0, head = 0, pre = 0, name = 0, post = 0, body = 0; };
    void ensureRoot(UiTree& tree);
    Row makeRow(UiTree& tree);

    UiWidgetId root_ = 0;
    std::vector<Row> rows_;
};

} // namespace aver::ui
