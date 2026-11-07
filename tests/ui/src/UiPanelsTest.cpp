// Headless test for the panel containers of the retained UI: Overlay, UniformGrid, WrapBox, SizeBox, ScaleBox,
// Border, Spacer and WidgetSwitcher. None is a widget kind: each is a layout mode or a per-widget property on a
// Panel (docs/GAME_UI.md), so this checks the layout maths, the file round trip and the palette presets.
#include "aver/core/Log.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiTree.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 0.05f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static UiWidget& mk(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name) {
    return *t.get(t.create(k, parent, name));
}

// A look-free container of a fixed size at the top-left, anchored by point (so its parent does not stretch it).
static UiWidget& box(UiTree& t, UiWidgetId parent, const char* name, UiLayoutMode layout, f32 w = -1, f32 h = -1) {
    UiWidget& c = mk(t, UiWidgetKind::Panel, parent, name);
    c.style = "clear";
    c.layout = layout;
    c.width = w;
    c.height = h;
    return c;
}

static UiWidget& sized(UiTree& t, UiWidgetId parent, const char* name, f32 w, f32 h) {
    return box(t, parent, name, UiLayoutMode::None, w, h);
}

static UiWidget& rootOf(UiTree& t, f32 w = 800, f32 h = 600) {
    UiWidget& r = box(t, 0, "Root", UiLayoutMode::None, w, h);
    r.inputMode = UiInputMode::Blocking;
    return r;
}

int main() {
    AVER_INFO("=== Overlay ===");
    {
        UiTree t;
        const UiWidgetId rid = rootOf(t).id;
        const UiWidgetId ovId = box(t, rid, "Ov", UiLayoutMode::Overlay, 400, 300).id;
        UiWidget& a = sized(t, ovId, "A", 100, 50);
        UiWidget& b = sized(t, ovId, "B", 100, 50);
        b.alignH = UiAlign::Center;
        b.alignV = UiAlign::End;
        UiWidget& c = box(t, ovId, "C", UiLayoutMode::None);
        c.margin = UiInsets{10, 10, 10, 10};
        UiWidget& ov2 = box(t, rid, "Ov2", UiLayoutMode::Overlay);
        ov2.offsets.left = 500;
        sized(t, ov2.id, "D", 100, 50);
        sized(t, ov2.id, "E", 80, 70);
        t.layout(g_metrics);
        check(approx(a.rect.x, 0) && approx(a.rect.y, 0) && approx(a.rect.w, 100) && approx(a.rect.h, 50),
              "overlay: a sized child sits at the top-left");
        check(approx(b.rect.x, 150) && approx(b.rect.y, 250), "overlay: alignH/alignV place a child in the box");
        check(approx(c.rect.x, 10) && approx(c.rect.y, 10) && approx(c.rect.w, 380) && approx(c.rect.h, 280),
              "overlay: an unsized child fills the box inside its margin");
        check(a.rect.x == 0 && c.rect.x == 10, "overlay: children stack on the same box, not one after another");
        UiWidget& o2 = *t.get(ov2.id);
        check(approx(o2.rect.w, 100) && approx(o2.rect.h, 70), "overlay: an auto-sized box is the largest child in each axis");
    }

    AVER_INFO("=== Uniform grid ===");
    {
        UiTree t;
        const UiWidgetId rid = rootOf(t).id;
        UiWidget& g = box(t, rid, "G", UiLayoutMode::Grid);
        g.uniform = true;
        g.columns = 2;
        g.spacing = 10;
        const f32 sizes[4][2] = {{60, 20}, {100, 40}, {30, 10}, {20, 20}};
        UiWidget* k[4];
        for (int i = 0; i < 4; ++i) k[i] = &sized(t, g.id, ("K" + std::to_string(i)).c_str(), sizes[i][0], sizes[i][1]);
        t.layout(g_metrics);
        check(approx(g.rect.w, 210) && approx(g.rect.h, 90), "uniform grid: every cell is the largest cell (2 x 100 + gap, 2 x 40 + gap)");
        check(approx(k[1]->rect.x, 110) && approx(k[2]->rect.y, 50) && approx(k[3]->rect.x, 110) && approx(k[3]->rect.y, 50),
              "uniform grid: cells sit on the equal tracks");
        t.get(g.id)->width = 410;
        t.get(g.id)->height = 190;
        t.layout(g_metrics);
        check(approx(k[1]->rect.x, 210) && approx(k[2]->rect.y, 100), "uniform grid: spare space is shared equally");
        t.get(g.id)->uniform = false;
        t.get(g.id)->width = -1;
        t.get(g.id)->height = -1;
        t.layout(g_metrics);
        check(approx(t.get(g.id)->rect.w, 100 + 10 + 60 + 0.0f) , "a plain grid sizes its columns to their own content");
    }

    AVER_INFO("=== Wrap box ===");
    {
        UiTree t;
        const UiWidgetId rid = rootOf(t).id;
        UiWidget& w = box(t, rid, "W", UiLayoutMode::Wrap, 250);
        w.spacing = 10;
        w.spacingY = 5;
        UiWidget* k[5];
        for (int i = 0; i < 5; ++i) k[i] = &sized(t, w.id, ("K" + std::to_string(i)).c_str(), 100, 30);
        t.layout(g_metrics);
        check(approx(k[0]->rect.x, 0) && approx(k[1]->rect.x, 110) && approx(k[2]->rect.x, 0) && approx(k[2]->rect.y, 35),
              "wrap: children flow in rows and break at the width");
        check(approx(k[4]->rect.x, 0) && approx(k[4]->rect.y, 70), "wrap: the last child starts the third row");
        check(approx(t.get(w.id)->rect.h, 100), "wrap: an auto height is the rows plus the row gap");
        t.get(w.id)->justify = UiJustify::Center;
        t.layout(g_metrics);
        check(approx(k[0]->rect.x, 20) && approx(k[1]->rect.x, 130), "wrap: justify centres each row");
        t.get(w.id)->justify = UiJustify::SpaceBetween;
        t.layout(g_metrics);
        check(approx(k[0]->rect.x, 0) && approx(k[1]->rect.x, 150), "wrap: spacebetween pushes a row's ends apart");
    }
    {
        UiTree t;
        const UiWidgetId top = rootOf(t).id;
        const UiWidgetId rid = box(t, top, "Col", UiLayoutMode::VStack, 250, 400).id;
        UiWidget& w = box(t, rid, "W", UiLayoutMode::Wrap);
        w.spacing = 10;
        for (int i = 0; i < 5; ++i) sized(t, w.id, ("K" + std::to_string(i)).c_str(), 100, 30);
        t.layout(g_metrics);
        check(approx(t.get(w.id)->rect.w, 250) && approx(t.get(w.id)->rect.h, 30 * 3 + 10 * 2),
              "wrap: in a stack with no width, it learns the width it was given and re-measures its height");
    }

    AVER_INFO("=== Size box ===");
    {
        UiTree t;
        const UiWidgetId rid = rootOf(t).id;
        UiWidget& sb = box(t, rid, "Size", UiLayoutMode::Overlay, 200, 100);
        UiWidget& inner = box(t, sb.id, "Inner", UiLayoutMode::None);
        UiWidget& minb = box(t, rid, "Min", UiLayoutMode::Overlay);
        minb.minWidth = 300;
        minb.offsets.top = 150;
        sized(t, minb.id, "M", 100, 50);
        UiWidget& maxb = box(t, rid, "Max", UiLayoutMode::Overlay);
        maxb.maxWidth = 50;
        maxb.offsets.top = 300;
        sized(t, maxb.id, "X", 100, 50);
        t.layout(g_metrics);
        check(approx(t.get(sb.id)->rect.w, 200) && approx(inner.rect.w, 200) && approx(inner.rect.h, 100),
              "size box: the width/height override sizes the box and its child fills it");
        check(approx(t.get(minb.id)->rect.w, 300) && approx(t.get(minb.id)->rect.h, 50), "size box: minWidth grows an auto-sized box");
        check(approx(t.get(maxb.id)->rect.w, 50), "size box: maxWidth caps an auto-sized box");
    }

    AVER_INFO("=== Scale box ===");
    {
        const auto build = [](UiTree& t, f32 boxW, f32 boxH, UiScaleFit fit) -> std::pair<UiWidgetId, UiWidgetId> {
            const UiWidgetId rid = rootOf(t).id;
            UiWidget& sc = box(t, rid, "Scale", UiLayoutMode::Overlay, boxW, boxH);
            sc.scaleFit = fit;
            UiWidget& c = sized(t, sc.id, "C", 100, 100);
            return {sc.id, c.id};
        };
        {
            UiTree t;
            const auto ids = build(t, 400, 200, UiScaleFit::Contain);
            t.layout(g_metrics);
            const UiWidget& c = *t.get(ids.second);
            check(approx(c.rect.x, 100) && approx(c.rect.y, 0) && approx(c.rect.w, 200) && approx(c.rect.h, 200),
                  "scale box: contain doubles 100x100 into 400x200 and centres it");
            check(approx(c.scale, 2.0f), "scale box: the child was laid out at twice the design scale");
        }
        {
            UiTree t;
            const auto ids = build(t, 400, 200, UiScaleFit::Cover);
            t.layout(g_metrics);
            const UiWidget& c = *t.get(ids.second);
            check(approx(c.rect.x, 0) && approx(c.rect.y, -100) && approx(c.rect.w, 400) && approx(c.rect.h, 400), "scale box: cover fills the box and overflows");
        }
        {
            UiTree t;
            const auto ids = build(t, 50, 50, UiScaleFit::Down);
            t.layout(g_metrics);
            check(approx(t.get(ids.second)->rect.w, 50), "scale box: down shrinks content that does not fit");
        }
        {
            UiTree t;
            const auto ids = build(t, 400, 200, UiScaleFit::Down);
            t.layout(g_metrics);
            const UiWidget& c = *t.get(ids.second);
            check(approx(c.rect.w, 100) && approx(c.rect.x, 150) && approx(c.rect.y, 50), "scale box: down never enlarges (and still centres)");
        }
        {
            UiTree t;
            const auto ids = build(t, 400, 200, UiScaleFit::FitWidth);
            t.layout(g_metrics);
            check(approx(t.get(ids.second)->rect.w, 400) && approx(t.get(ids.second)->rect.h, 400), "scale box: fit width scales by the width ratio");
        }
        {
            // A fixed contentScale: the box sizes itself to the scaled content, and text scales with it.
            UiTree t;
            const UiWidgetId rid = rootOf(t).id;
            UiWidget& sc = box(t, rid, "Scale", UiLayoutMode::Overlay);
            sc.contentScale = 2.0f;
            UiWidget& c = sized(t, sc.id, "C", 100, 50);
            UiWidget& label = mk(t, UiWidgetKind::Text, sc.id, "Label");
            label.text = "Hello";
            label.alignH = UiAlign::Start;
            label.alignV = UiAlign::Start;
            t.layout(g_metrics);
            check(approx(t.get(sc.id)->rect.w, 200) && approx(t.get(sc.id)->rect.h, 100) && approx(c.rect.w, 200), "scale box: contentScale 2 doubles the box's auto size");

            UiTree base;
            const UiWidgetId brid = rootOf(base).id;
            UiWidget& bl = mk(base, UiWidgetKind::Text, brid, "Label");
            bl.text = "Hello";
            base.layout(g_metrics);
            check(approx(t.get(label.id)->rect.w, bl.rect.w * 2.0f, 0.5f), "scale box: text is laid out at the scaled size, not stretched");
        }
    }

    AVER_INFO("=== Border and spacer ===");
    {
        UiTree t;
        UiWidget& r = rootOf(t);
        UiWidget& b = box(t, r.id, "B", UiLayoutMode::None, 100, 100);
        UiDrawList none;
        UiDrawListPainter p0(none, nullptr);
        t.layout(g_metrics);
        t.draw(p0);
        check(none.empty(), "a clear-style panel draws nothing");
        t.get(b.id)->borderWidth = 2.0f;
        t.get(b.id)->borderColor = uiRgba(255, 0, 0);
        UiDrawList bordered;
        UiDrawListPainter p1(bordered, nullptr);
        t.draw(p1);
        check(!bordered.empty(), "borderWidth and borderColor draw a border on any panel");
    }
    {
        const std::vector<UiWidgetPreset>& presets = uiWidgetPresets();
        UiLayoutDoc doc = uiStarterLayout();
        bool allOk = true;
        for (usize i = 0; i < presets.size(); ++i) allOk = allOk && uiLayoutAddPreset(doc, 0, i) >= 0;
        check(allOk && doc.valid(), "every palette preset adds a valid widget");
        auto has = [&](const char* label) {
            for (const UiWidgetPreset& p : presets) if (std::string(p.label) == label) return true;
            return false;
        };
        check(has("Overlay") && has("Uniform Grid Panel") && has("Wrap Box") && has("Size Box") && has("Scale Box") &&
                  has("Border") && has("Spacer") && has("Widget Switcher"),
              "the presets are named like UMG's panels");
        const i32 sp = uiLayoutFind(doc, "Spacer");
        check(sp >= 0 && doc.nodes[static_cast<usize>(sp)].props.hit == UiHitMode::Never, "a spacer takes no input");
        UiTree t;
        check(uiInstantiateLayout(doc, t, 0) != 0, "the presets instantiate");
        t.layout(g_metrics);
    }

    AVER_INFO("=== Widget switcher ===");
    {
        UiTree t;
        const UiWidgetId rid = rootOf(t).id;
        UiWidget& sw = box(t, rid, "Sw", UiLayoutMode::Switcher, 200, 100);
        UiWidget* k[3];
        for (int i = 0; i < 3; ++i) {
            k[i] = &box(t, sw.id, ("K" + std::to_string(i)).c_str(), UiLayoutMode::None);
            k[i]->hit = UiHitMode::Always;
        }
        sw.activeIndex = 1;
        t.layout(g_metrics);
        check(approx(k[1]->rect.w, 200) && approx(k[1]->rect.h, 100), "switcher: the active child fills the box");
        check(t.hitTest(10, 10) == k[1]->id, "switcher: only the active child takes the pointer");
        check(!t.isVisible(k[0]->id) && t.isVisible(k[1]->id) && !t.isVisible(k[2]->id), "switcher: the others are not visible");
        t.get(sw.id)->activeIndex = 2;
        t.layout(g_metrics);
        check(t.hitTest(10, 10) == k[2]->id && t.isVisible(k[2]->id) && !t.isVisible(k[1]->id), "switcher: changing the index swaps the child");
        t.get(sw.id)->activeIndex = 7;
        t.layout(g_metrics);
        check(!t.isVisible(k[0]->id) && !t.isVisible(k[1]->id) && !t.isVisible(k[2]->id), "switcher: an index past the end shows nothing");

        k[0]->bgColor = uiRgba(10, 20, 30);
        t.get(sw.id)->activeIndex = 1;
        t.layout(g_metrics);
        UiDrawList l1;
        UiDrawListPainter p1(l1, nullptr);
        t.draw(p1);
        check(l1.empty(), "switcher: an inactive child is not drawn");
        t.get(sw.id)->activeIndex = 0;
        t.layout(g_metrics);
        UiDrawList l0;
        UiDrawListPainter p0(l0, nullptr);
        t.draw(p0);
        check(!l0.empty(), "switcher: the active child is drawn");

        // switching back off the mode shows every child again
        t.get(sw.id)->layout = UiLayoutMode::Overlay;
        t.layout(g_metrics);
        check(t.isVisible(k[1]->id) && t.isVisible(k[2]->id), "switcher: leaving the mode restores the children");
    }

    AVER_INFO("=== files ===");
    {
        UiLayoutDoc doc = uiStarterLayout();
        const i32 g = uiLayoutAddPreset(doc, 0, 4);   // Uniform Grid Panel
        const i32 s = uiLayoutAddPreset(doc, 0, 8);   // Scale Box
        const i32 w = uiLayoutAddPreset(doc, 0, 11);  // Widget Switcher
        doc.nodes[static_cast<usize>(w)].props.activeIndex = 2;
        doc.nodes[static_cast<usize>(s)].props.contentScale = 1.5f;
        doc.nodes[static_cast<usize>(s)].props.borderWidth = 3.0f;
        doc.nodes[static_cast<usize>(s)].props.borderColor = uiRgba(1, 2, 3, 4);
        (void)g;
        const std::string text = uiWriteLayout(doc);
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(text, back, &err), "a layout with the new modes parses: " + err);
        check(uiWriteLayout(back) == text, "and round-trips byte for byte");
        check(text.find("layout overlay") != std::string::npos && text.find("layout switcher") != std::string::npos &&
                  text.find("uniform true") != std::string::npos && text.find("scaleFit contain") != std::string::npos &&
                  text.find("activeIndex 2") != std::string::npos,
              "the modes and properties are written by name");
        const UiLayoutDoc starter = uiStarterLayout();
        check(uiWriteLayout(starter).find("uniform") == std::string::npos && uiWriteLayout(starter).find("scaleFit") == std::string::npos,
              "a layout that does not use them writes none of them");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all panel checks passed");
    return 0;
}
