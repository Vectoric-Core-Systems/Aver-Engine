// Headless test for the .ocui layout asset: the property table, round trip, errors, forward
// compatibility, the structural edits and instantiate/capture. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// True when parsing `text` fails and the error mentions `expect`.
static bool failsWith(const std::string& text, const std::string& expect) {
    UiLayoutDoc d;
    std::string err;
    if (uiParseLayout(text, d, &err)) return false;
    if (err.find(expect) == std::string::npos) AVER_ERROR("    error was: {}", err);
    return err.find(expect) != std::string::npos;
}

static std::vector<std::string> tokens(const UiPropDesc& d, const UiWidgetProps& p) {
    std::vector<std::string> t;
    d.get(p, t);
    return t;
}

int main() {
    AVER_INFO("=== tokens and numbers ===");
    {
        std::vector<std::string> t;
        check(uiTokenize("a \"b c\" \"d\\\"e\" f\\g \"\"", t) && t.size() == 5 && t[0] == "a" && t[1] == "b c" &&
                  t[2] == "d\"e" && t[3] == "f\\g" && t[4].empty(),
              "quotes keep spaces, escapes work, an empty string is a token");
        check(!uiTokenize("\"open", t), "an unterminated quote is refused");
        check(uiQuote("plain") == "plain" && uiQuote("a b") == "\"a b\"" && uiQuote("") == "\"\"" &&
                  uiQuote("x\ny") == "\"x\\ny\"",
              "quoting only when needed");
        bool exact = true;
        for (const f32 v : {0.0f, 0.1f, 1.0f / 3.0f, -12.5f, 1e-7f, 123456.789f, 3.4e38f, 1920.0f, 0.3f})
            exact = exact && uiFormatFloat(v) != "" && [&] { f32 r = 0; return uiParseFloat(uiFormatFloat(v), r) && r == v; }();
        check(exact, "floats print to text that reads back bit-exact");
        check(uiFormatFloat(0.5f) == "0.5" && uiFormatFloat(2.0f) == "2", "and stay short");
        f32 f = 0;
        check(!uiParseFloat("1x", f) && !uiParseFloat("", f) && !uiParseFloat("nan", f), "bad floats are refused");
        i32 i = 0;
        check(uiParseInt("-7", i) && i == -7 && !uiParseInt("7.5", i), "ints parse whole");
    }

    AVER_INFO("=== the property table ===");
    {
        bool allRound = true;
        for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k) {
            const UiWidgetProps def = uiDefaultProps(static_cast<UiWidgetKind>(k));
            for (const UiPropDesc& d : uiProperties()) {
                UiWidgetProps fresh;
                const std::vector<std::string> a = tokens(d, def);
                if (!d.set(fresh, a) || tokens(d, fresh) != a) {
                    AVER_ERROR("    property '{}' does not round trip on kind {}", d.key, k);
                    allRound = false;
                }
            }
        }
        check(allRound, "every property's get and set agree, for every kind's defaults");

        bool unique = true;
        const std::vector<UiPropDesc>& all = uiProperties();
        for (usize a = 0; a < all.size(); ++a)
            for (usize b = a + 1; b < all.size(); ++b)
                if (std::string(all[a].key) == all[b].key) unique = false;
        check(unique && all.size() > 50, "property names are unique");

        UiWidgetProps p;
        check(uiSetProp(p, "WIDTH", "120.5") && p.width == 120.5f, "keys are case-insensitive");
        check(!uiSetProp(p, "width", "abc") && p.width == 120.5f, "a bad value leaves the widget unchanged");
        check(!uiSetProp(p, "noSuchThing", "1"), "an unknown key is refused");
        check(uiSetProp(p, "anchors", "0.5 0.25") && p.anchors.minX == 0.5f && p.anchors.maxX == 0.5f && p.anchors.maxY == 0.25f,
              "two numbers make a point anchor");
        check(uiSetProp(p, "margin", "4") && p.margin == UiInsets{4, 4, 4, 4}, "one number is uniform insets");
        check(uiSetProp(p, "margin", "4 8") && p.margin == UiInsets{4, 8, 4, 8}, "two are horizontal and vertical");
        check(uiSetProp(p, "bgColor", "#11223344") && uiAlphaOf(p.bgColor) == 0x44 && (p.bgColor & 0xFF) == 0x11, "colours are #RRGGBBAA");
        check(uiSetProp(p, "bgColor", "#112233") && uiAlphaOf(p.bgColor) == 0xFF, "alpha defaults to opaque");
        check(uiSetProp(p, "bgColor", "none") && p.bgColor == 0, "none clears a colour");
        check(uiSetProp(p, "layout", "HStack") && p.layout == UiLayoutMode::HStack, "enums by name");
        check(!uiSetProp(p, "layout", "diagonal"), "an unknown enum name is refused");
        check(uiSetProp(p, "visible", "false") && !p.visible && uiSetProp(p, "visible", "1") && p.visible, "bools");
        std::string text;
        check(uiGetProp(p, "layout", text) && text == "hstack", "get returns the lower-case name");
        p.items = {"a b", "c"};
        check(uiGetProp(p, "items", text) && text == "\"a b\" c", "a list is quoted per item");
    }

    AVER_INFO("=== round trip ===");
    {
        UiLayoutDoc d = uiStarterLayout();
        std::string why;
        check(d.valid(&why), "the starter layout is valid: " + why);
        const std::string text = uiWriteLayout(d);
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(text, back, &err), "the starter parses back: " + err);
        check(uiWriteLayout(back) == text, "write(parse(write(doc))) == write(doc)");
        check(back.nodes.size() == d.nodes.size() && back.name == "NewLayout" && back.theme == "dark", "the shape survives");
        check(text.find("OCUI 1") == 0, "the header comes first");
        check(text.find("SET Window layout vstack") != std::string::npos, "properties are written by name");
        check(text.find("SET PlayButton focusable") == std::string::npos, "values equal to the kind's default are not written");
    }
    {
        UiLayoutDoc d = uiStarterLayout();
        const i32 win = uiLayoutFind(d, "Window");
        for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k) uiLayoutAdd(d, win, static_cast<UiWidgetKind>(k));
        const i32 btn = uiLayoutFind(d, "PlayButton");
        UiWidgetProps& p = d.nodes[static_cast<usize>(btn)].props;
        p.text = "He said \"hi\" \\ and\nleft";
        p.opacity = 0.3f;
        p.width = 1.0f / 3.0f;
        p.offsets = UiInsets{-12.5f, 1e-7f, 3.4e38f, 0.0f};
        p.bgColor = uiRgba(1, 2, 3, 4);
        p.textColor = uiRgba(255, 0, 128, 255);
        p.style = "danger";
        p.fillW = 2.5f;
        p.alignV = UiAlign::End;
        p.navUp = "Title";
        p.tabIndex = -1;
        const i32 choice = uiLayoutFind(d, "Choice");
        d.nodes[static_cast<usize>(choice)].props.items = {"a b", "c\"d", "", "e"};
        d.nodes[static_cast<usize>(choice)].props.selected = 2;
        const i32 root = 0;
        d.nodes[static_cast<usize>(root)].props.layer = UiLayer::Overlay;
        d.nodes[static_cast<usize>(root)].props.zOrder = -3;
        d.nodes[static_cast<usize>(root)].props.modal = true;
        d.nodes[static_cast<usize>(root)].props.cancelCommand = "close it";
        d.dpi.mode = UiScaleMode::Blend;
        d.dpi.refWidth = 2560;
        d.dpi.refHeight = 1440;
        d.theme = "light";
        d.name = "Everything Here";

        const std::string text = uiWriteLayout(d);
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(text, back, &err), "a layout using every kind and awkward values parses: " + err);
        check(uiWriteLayout(back) == text, "and writes identically");
        const UiWidgetProps& q = back.nodes[static_cast<usize>(uiLayoutFind(back, "PlayButton"))].props;
        check(q.text == p.text && q.opacity == p.opacity && q.width == p.width && q.offsets == p.offsets &&
                  q.bgColor == p.bgColor && q.textColor == p.textColor && q.fillW == p.fillW && q.alignV == UiAlign::End &&
                  q.navUp == "Title" && q.tabIndex == -1 && q.style == "danger",
              "awkward text, fractions, extremes and colours come back exactly");
        const UiWidgetProps& c = back.nodes[static_cast<usize>(uiLayoutFind(back, "Choice"))].props;
        check(c.items == std::vector<std::string>({"a b", "c\"d", "", "e"}) && c.selected == 2, "lists with spaces, quotes and empties");
        check(back.dpi.mode == UiScaleMode::Blend && back.dpi.refWidth == 2560 && back.theme == "light" && back.name == "Everything Here",
              "document settings survive");
        const UiWidgetProps& r = back.nodes[0].props;
        check(r.layer == UiLayer::Overlay && r.zOrder == -3 && r.modal && r.cancelCommand == "close it", "root settings survive");
    }
    {
        UiLayoutDoc d = uiStarterLayout();
        UiTree tree;
        const UiWidgetId id = uiInstantiateLayout(d, tree);
        check(id != 0 && tree.size() == d.nodes.size(), "instantiating creates one widget per node");
        check(tree.find("PlayButton") != 0 && tree.get(tree.find("PlayButton"))->text == "Play", "with their properties");
        check(tree.get(tree.find("PlayButton"))->parent == tree.find("Window"), "and the hierarchy");
        UiLayoutDoc captured;
        check(uiCaptureLayout(tree, id, captured), "capturing a subtree");
        captured.name = d.name;
        check(uiWriteLayout(captured) == uiWriteLayout(d), "capture reproduces the document");
        UiTree other;
        const UiWidgetId host = other.create(UiWidgetKind::Panel, 0, "Host");
        const UiWidgetId sub = uiInstantiateLayout(d, other, host);
        check(other.get(sub)->parent == host, "a layout can be instantiated under an existing widget");
        UiLayoutDoc bad;
        check(uiInstantiateLayout(bad, other) == 0, "an invalid document instantiates nothing");
    }

    AVER_INFO("=== errors ===");
    {
        check(failsWith("WIDGET Root panel -\n", "missing OCUI header"), "no header");
        check(failsWith("OCUI 9\nWIDGET Root panel -\n", "unsupported layout version"), "a newer version");
        check(failsWith("OCUI 1\nWIDGET Root bogus -\n", "unknown widget kind"), "an unknown kind");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nWIDGET Root panel Root\n", "duplicate widget name"), "a duplicate name");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nWIDGET A panel Nope\n", "not defined above"), "an undefined parent");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nWIDGET B panel -\n", "second root"), "two roots");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nSET Root width abc\n", "bad value"), "a bad value");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nSET Root text \"open\n", "unterminated quote"), "an unterminated quote");
        check(failsWith("OCUI 1\nNAME x\n", "root"), "no widgets");
        check(failsWith("OCUI 1\nSET Ghost width 3\n", "unknown widget"), "SET on a widget that does not exist");
        check(failsWith("OCUI 1\nWIDGET Root panel -\nSET Root width 3\nSET Root\n", "line 4"), "errors name the line");
        UiLayoutDoc keep = uiStarterLayout();
        const std::string before = uiWriteLayout(keep);
        std::string err;
        check(!uiParseLayout("OCUI 1\nWIDGET Root bogus -\n", keep, &err) && uiWriteLayout(keep) == before, "a failed parse leaves the output untouched");
    }

    AVER_INFO("=== forward compatibility ===");
    {
        const std::string text =
            "# a comment\r\n\r\nOCUI 1\r\nFUTURE thing 1 2 3\r\nNAME Later\r\nWIDGET Root panel -\r\n"
            "SET Root futureProperty 5\r\nSET Root width 50\r\n   # indented comment\r\nWIDGET Kid text Root\r\nSET Kid text \"hi there\"\r\n";
        UiLayoutDoc d;
        std::string err;
        check(uiParseLayout(text, d, &err), "unknown records and properties are skipped: " + err);
        check(d.nodes.size() == 2 && d.nodes[0].props.width == 50 && d.nodes[1].props.text == "hi there" && d.name == "Later",
              "and the rest loads, CRLF included");
    }

    AVER_INFO("=== structural edits ===");
    {
        UiLayoutDoc d = uiStarterLayout();
        const auto names = [&](i32 parent) {
            std::vector<std::string> out;
            for (const i32 c : uiLayoutChildren(d, parent)) out.push_back(d.nodes[static_cast<usize>(c)].props.name);
            return out;
        };
        std::string why;
        const i32 win = uiLayoutFind(d, "Window");
        check(win == 1 && names(win) == std::vector<std::string>({"Title", "PlayButton", "SettingsButton", "QuitButton"}),
              "the starter's hierarchy");

        const i32 extra = uiLayoutAdd(d, win, UiWidgetKind::Button, "Extra");
        check(extra > 0 && d.valid(&why) && names(win).back() == "Extra", "add puts a child last, in pre-order");
        check(d.nodes[static_cast<usize>(extra)].props.text == "Button", "new widgets get sample content");
        check(d.nodes[static_cast<usize>(uiLayoutAdd(d, win, UiWidgetKind::Button, "Extra"))].props.name == "Extra_2", "names stay unique");
        check(uiLayoutAdd(d, 99, UiWidgetKind::Text) == -1, "a bad parent adds nothing");

        const i32 moved = uiLayoutMoveSibling(d, uiLayoutFind(d, "Extra"), -1);
        check(moved > 0 && d.valid(&why) && names(win)[3] == "Extra" && names(win)[4] == "QuitButton", "move sibling swaps order");
        check(uiLayoutMoveSibling(d, uiLayoutFind(d, "Title"), -1) == -1, "the first sibling cannot move earlier");
        check(uiLayoutMoveSibling(d, 0, 1) == -1, "the root has no siblings");

        const i32 re = uiLayoutReparent(d, uiLayoutFind(d, "Extra"), 0, -1);
        check(re > 0 && d.valid(&why) && d.nodes[static_cast<usize>(re)].parent == 0, "reparent moves a subtree");
        check(uiLayoutReparent(d, uiLayoutFind(d, "Window"), uiLayoutFind(d, "PlayButton")) == -1, "a move into its own subtree is refused");
        check(uiLayoutReparent(d, 0, 1) == -1, "the root cannot be moved");
        const i32 first = uiLayoutReparent(d, uiLayoutFind(d, "Extra"), uiLayoutFind(d, "Window"), 0);
        check(first > 0 && names(uiLayoutFind(d, "Window"))[0] == "Extra", "reparent can place at a position");

        const usize before = d.nodes.size();
        const i32 dup = uiLayoutDuplicate(d, uiLayoutFind(d, "Window"));
        check(dup > 0 && d.valid(&why) && d.nodes.size() > before, "duplicate copies a subtree");
        check(d.nodes[static_cast<usize>(dup)].props.name == "Window_2" && d.nodes[static_cast<usize>(dup)].parent == 0,
              "the copy sits beside the original with a fresh name");
        check(uiLayoutFind(d, "Title_2") > 0 && d.nodes[static_cast<usize>(uiLayoutFind(d, "Title_2"))].parent == dup, "its children come too");
        check(names(0).size() == 2 && names(0)[0] == "Window" && names(0)[1] == "Window_2", "the copy follows the original");
        check(uiLayoutDuplicate(d, 0) == -1, "the root cannot be duplicated");

        check(uiLayoutRename(d, uiLayoutFind(d, "Window_2"), "Dialog") && uiLayoutFind(d, "Dialog") > 0, "rename");
        check(!uiLayoutRename(d, uiLayoutFind(d, "Dialog"), "Window") && !uiLayoutRename(d, 1, ""), "a taken or empty name is refused");

        const usize beforeDelete = d.nodes.size();
        const i32 parent = uiLayoutDelete(d, uiLayoutFind(d, "Dialog"));
        check(parent == 0 && d.valid(&why) && d.nodes.size() < beforeDelete && uiLayoutFind(d, "Title_2") < 0, "delete removes the whole subtree");
        check(uiLayoutDelete(d, 0) == -1, "the root cannot be deleted");
        check(uiUniqueWidgetName(d, "Window") == "Window_2" && uiUniqueWidgetName(d, "") == "Widget", "unique names");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all layout asset checks passed");
    return 0;
}
