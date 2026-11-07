// Headless test for the .ocui layout asset: the property table, round trip, errors, forward
// compatibility, the structural edits and instantiate/capture. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"

#include <cmath>
#include <map>
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

    AVER_INFO("=== v2: the format ===");
    {
        // A canonical v2 file reads and writes back byte for byte.
        const std::string canonical =
            "OCUI 2\n"
            "NAME Main\n"
            "THEME dark\n"
            "SCALE height 1920 1080\n"
            "\n"
            "WIDGET Root panel -\n"
            "\n"
            "WIDGET Label text Root\n"
            "SET Label text Title\n"
            "\n"
            "WIDGET Body panel Root\n"
            "\n"
            "USE Row1 UI/Row.ocui Root\n"
            "SET Row1 width 300\n"
            "OVERRIDE Row1 Title \"Hello World\"\n"
            "\n"
            "EXPOSE Title Label text\n"
            "SLOT Content Body\n"
            "\n"
            "ANIM Pulse Row1 opacity 0 1 0.5 0.4\n"
            "\n"
            "BIND Row1 visible player.alive\n"
            "\n"
            "EVENT Row1 clicked command play\n";
        UiLayoutDoc d;
        std::string err;
        check(uiParseLayout(canonical, d, &err), "a v2 file parses: " + err);
        check(uiWriteLayout(d) == canonical, "and writes back byte for byte");
        check(d.version == 2 && d.nodes.size() == 4 && d.nodes[3].use && d.nodes[3].use->path == "UI/Row.ocui" &&
                  d.nodes[3].use->overrides.size() == 1 && d.nodes[3].use->overrides[0].values[0] == "Hello World",
              "the USE keeps its path and OVERRIDE");
        check(d.exposed.size() == 1 && d.slots.size() == 1 && d.anims.size() == 1 && d.anims[0].keys.size() == 4 &&
                  d.binds.size() == 1 && d.events.size() == 1 && d.events[0].target == "play",
              "EXPOSE, SLOT, ANIM, BIND and EVENT are kept");
        check(d.valid(&err), "it is valid");

        // v1 stays exactly v1.
        const std::string v1 = uiWriteLayout(uiStarterLayout());
        UiLayoutDoc d1;
        check(uiParseLayout(v1, d1) && d1.version == 1 && uiWriteLayout(d1) == v1 && v1.find("OCUI 1") == 0,
              "a v1 document round-trips unchanged and stays version 1");
        const std::string v1withV2Records = "OCUI 1\nWIDGET Root panel -\nUSE X a.ocui Root\nANIM A Root opacity\nEXPOSE T Root text\n";
        UiLayoutDoc d1b;
        check(uiParseLayout(v1withV2Records, d1b) && d1b.nodes.size() == 1 && d1b.anims.empty() && d1b.exposed.empty() &&
                  uiWriteLayout(d1b) == "OCUI 1\nTHEME dark\nSCALE height 1920 1080\n\nWIDGET Root panel -\n",
              "v2 records in a v1 file stay unknown records, as before");
        UiLayoutDoc built = uiStarterLayout();
        uiLayoutAddUse(built, 0, "UI/Row.ocui");
        check(built.usesV2() && uiWriteLayout(built).find("OCUI 2") == 0, "using a v2 record writes the header as version 2");
        uiLayoutDelete(built, uiLayoutFind(built, "Row"));
        check(!built.usesV2() && uiWriteLayout(built) == v1, "and removing it goes back to version 1");

        check(failsWith("OCUI 2\nWIDGET Root panel -\nUSE X a.ocui -\n", "cannot be the root"), "a USE cannot be the root");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nOVERRIDE Root T v\n", "not a USE"), "OVERRIDE needs a USE");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nEXPOSE A Root text\nEXPOSE A Root text\n", "duplicate EXPOSE"), "a duplicate EXPOSE name");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nEXPOSE A Ghost text\n", "unknown widget"), "an EXPOSE of a missing widget");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nSLOT A Ghost\n", "unknown widget"), "a SLOT of a missing widget");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nANIM A Root\n", "ANIM needs"), "ANIM arity");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nBIND Root text\n", "BIND needs"), "BIND arity");
        check(failsWith("OCUI 2\nWIDGET Root panel -\nEVENT Root clicked command\n", "EVENT needs"), "EVENT arity");
        check(failsWith("OCUI 3\nWIDGET Root panel -\n", "unsupported layout version"), "version 3 is still refused");

        // Structural edits keep the records honest.
        UiLayoutDoc e;
        uiParseLayout(canonical, e);
        uiLayoutRename(e, uiLayoutFind(e, "Label"), "Caption");
        check(e.exposed[0].widget == "Caption" && e.valid(&err), "renaming a widget updates the EXPOSE that names it");
        uiLayoutRename(e, uiLayoutFind(e, "Row1"), "Line");
        check(e.anims[0].widget == "Line" && e.binds[0].widget == "Line" && e.events[0].widget == "Line", "and the ANIM, BIND and EVENT");
        uiLayoutDelete(e, uiLayoutFind(e, "Caption"));
        check(e.exposed.empty() && e.valid(&err), "deleting a widget drops its EXPOSE");
        uiLayoutDelete(e, uiLayoutFind(e, "Line"));
        check(e.anims.empty() && e.binds.empty() && e.events.empty(), "and its ANIM, BIND and EVENT");
        check(uiLayoutAddUse(e, 0, "") == -1 && uiLayoutAddUse(e, 9, "a.ocui") == -1, "a USE needs a path and a parent");
        const i32 u1 = uiLayoutAddUse(e, 0, "UI\\Row.ocui");
        const i32 u2 = uiLayoutAddUse(e, 0, "UI/Row.ocui");
        check(e.nodes[static_cast<usize>(u1)].props.name == "Row" && e.nodes[static_cast<usize>(u2)].props.name == "Row_2", "a USE is named after its file");
        check(uiLayoutSetOverride(e, u2, "Title", {"x"}) && uiLayoutSetOverride(e, u2, "Title", {"y"}) &&
                  e.nodes[static_cast<usize>(u2)].use->overrides.size() == 1 && e.nodes[static_cast<usize>(u2)].use->overrides[0].values[0] == "y",
              "an OVERRIDE is replaced, not added twice");
        check(uiLayoutClearOverride(e, u2, "Title") && !uiLayoutClearOverride(e, u2, "Title") && !uiLayoutSetOverride(e, 0, "T", {"x"}),
              "and cleared; a non-USE takes none");
        const i32 dup = uiLayoutDuplicate(e, u2);
        check(dup > 0 && e.nodes[static_cast<usize>(dup)].use && e.nodes[static_cast<usize>(dup)].use->path == "UI/Row.ocui", "duplicating a USE keeps the reference");
    }

    AVER_INFO("=== v2: nested layouts ===");
    {
        std::map<std::string, std::string> files;
        files["UI/Row.ocui"] =
            "OCUI 2\n"
            "WIDGET RowRoot panel -\n"
            "SET RowRoot layout hstack\n"
            "SET RowRoot defaultFocus Label\n"
            "WIDGET Label text RowRoot\n"
            "SET Label text Default\n"
            "SET Label navDown Body\n"
            "WIDGET Body panel RowRoot\n"
            "EXPOSE Title Label text\n"
            "SLOT Content Body\n";
        const UiLayoutLoader loader = [&](const std::string& path, std::string& text) {
            const auto it = files.find(path);
            if (it == files.end()) return false;
            text = it->second;
            return true;
        };

        UiLayoutDoc host = uiStarterLayout();
        const i32 r1 = uiLayoutAddUse(host, 0, "UI/Row.ocui", "Row1");
        const i32 r2 = uiLayoutAddUse(host, 0, "UI/Row.ocui", "Row2");
        host.nodes[static_cast<usize>(r1)].props.width = 300;
        uiLayoutSetOverride(host, r1, "Title", {"Hello"});
        const i32 extra = uiLayoutAdd(host, r1, UiWidgetKind::Button, "Extra");
        host.nodes[static_cast<usize>(extra)].props.slot = "Content";
        (void)uiLayoutAdd(host, uiLayoutFind(host, "Row2"), UiWidgetKind::Button, "Loose");
        std::string why;
        check(host.valid(&why), "the host is valid: " + why);

        UiTree t;
        const UiInstantiateResult res = uiInstantiateLayoutEx(host, t, 0, loader);
        check(res.root != 0 && res.errors.empty(), "a layout with nested layouts instantiates");
        check(res.nodeIds.size() == host.nodes.size() && res.nodeIds[static_cast<usize>(r1)] == t.find("Row1"), "each document node has its widget; a USE has the instance root");
        const UiWidget* row1 = t.get(t.find("Row1"));
        check(row1 && row1->layout == UiLayoutMode::HStack && row1->width == 300, "the instance root is the used layout's root with the instance's placement laid over it");
        const UiWidget* label = t.get(t.find("Row1/Label"));
        check(label && label->text == "Hello", "an OVERRIDE sets the exposed property");
        const UiWidget* label2 = t.get(t.find("Row2/Label"));
        check(label2 && label2->text == "Default", "another instance keeps the default");
        const UiWidgetId body = t.find("Row1/Body");
        const UiWidget* ex = t.get(t.find("Extra"));
        check(body && ex && ex->parent == body, "a child with a slot goes into that SLOT");
        const UiWidget* lo = t.get(t.find("Loose"));
        check(lo && lo->parent == t.find("Row2"), "a child with no slot goes under the instance root");
        check(label && label->navDown == "Row1/Body" && row1->defaultFocus == "Row1/Label", "names the used layout refers to follow the prefix");
        check(t.find("Row1") != 0 && t.get(t.find("Row1"))->parent == res.root, "the instance sits under its parent");

        // an unknown slot or override is reported, not fatal
        host.nodes[static_cast<usize>(extra)].props.slot = "Nope";
        uiLayoutSetOverride(host, uiLayoutFind(host, "Row2"), "Ghost", {"x"});
        UiTree t2;
        const UiInstantiateResult res2 = uiInstantiateLayoutEx(host, t2, 0, loader);
        check(res2.root != 0 && res2.errors.size() == 2, "an unknown slot and an unknown override are errors that do not stop the build");
        check(t2.get(t2.find("Extra"))->parent == t2.find("Row1"), "an unknown slot puts the child under the instance root");

        // exposed properties for the inspector
        std::vector<UiExposedProp> props;
        check(uiLayoutExposedProps(host, r1, loader, props, &why) && props.size() == 1 && props[0].name == "Title" &&
                  props[0].overridden && props[0].value[0] == "Hello",
              "the exposed properties list their overrides");
        check(uiLayoutExposedProps(host, uiLayoutFind(host, "Row2"), loader, props, &why) && props[0].value[0] == "Default",
              "or the used layout's own value");
        check(!uiLayoutExposedProps(host, 0, loader, props, &why), "a non-USE has none");

        // round trip with a slot and an override
        const std::string text = uiWriteLayout(host);
        UiLayoutDoc back;
        check(uiParseLayout(text, back, &why) && uiWriteLayout(back) == text, "a host with USEs round-trips");

        // a file that cannot be found
        UiLayoutDoc missing = uiStarterLayout();
        uiLayoutAddUse(missing, 0, "UI/Gone.ocui", "Gone");
        UiTree t3;
        const UiInstantiateResult res3 = uiInstantiateLayoutEx(missing, t3, 0, loader);
        check(res3.root != 0 && res3.errors.size() == 1 && res3.errors[0].find("cannot find") != std::string::npos,
              "a missing file is an error, not a failure");
        check(t3.find("Gone") != 0 && t3.find("Gone/Missing") != 0 && t3.get(t3.find("Gone/Missing"))->text.find("Gone.ocui") != std::string::npos,
              "and leaves a placeholder that names it");
        UiTree t4;
        check(uiInstantiateLayout(missing, t4, 0) != 0 && t4.find("Gone") != 0, "with no loader at all a USE is a placeholder too");
        files["UI/Bad.ocui"] = "not a layout";
        UiLayoutDoc bad = uiStarterLayout();
        uiLayoutAddUse(bad, 0, "UI/Bad.ocui", "Bad");
        UiTree t5;
        const UiInstantiateResult res5 = uiInstantiateLayoutEx(bad, t5, 0, loader);
        check(res5.root != 0 && res5.errors.size() == 1 && t5.find("Bad") != 0, "an unreadable file is a placeholder too");
    }

    AVER_INFO("=== v2: cycles ===");
    {
        std::map<std::string, std::string> files;
        files["A.ocui"] = "OCUI 2\nWIDGET RootA panel -\nUSE ToB B.ocui RootA\n";
        files["B.ocui"] = "OCUI 2\nWIDGET RootB panel -\nUSE ToA A.ocui RootB\n";
        files["Self.ocui"] = "OCUI 2\nWIDGET RootS panel -\nUSE Again Self.ocui RootS\n";
        const UiLayoutLoader loader = [&](const std::string& path, std::string& text) {
            const auto it = files.find(path);
            if (it == files.end()) return false;
            text = it->second;
            return true;
        };
        UiLayoutDoc a;
        uiParseLayout(files["A.ocui"], a);
        std::string err;
        check(!uiLayoutCheckUses(a, "A.ocui", loader, &err) && err.find("cycle") != std::string::npos && err.find("B.ocui") != std::string::npos,
              "A uses B uses A is found without building: " + err);
        UiTree t;
        const UiInstantiateResult r = uiInstantiateLayoutEx(a, t, 0, loader, "A.ocui");
        check(r.root == 0 && !r.errors.empty() && r.errors[0].find("cycle") != std::string::npos, "and instantiate fails with a clear error");
        check(t.sortedRoots().empty(), "leaving no half-built widgets");
        UiTree t2;
        check(uiInstantiateLayoutEx(a, t2, 0, loader).root == 0, "even when the document's own path is not known");
        UiLayoutDoc s;
        uiParseLayout(files["Self.ocui"], s);
        UiTree t3;
        check(uiInstantiateLayoutEx(s, t3, 0, loader, "self.ocui").root == 0 && !uiLayoutCheckUses(s, "Self.ocui", loader), "a layout that uses itself is a cycle (paths compare without case or slash style)");
        check(uiInstantiateLayout(s, t3, 0) != 0, "and without a loader it is just a placeholder");

        // the same layout used twice side by side is not a cycle
        files["Leaf.ocui"] = "OCUI 2\nWIDGET LeafRoot panel -\n";
        files["Twice.ocui"] = "OCUI 2\nWIDGET T panel -\nUSE One Leaf.ocui T\nUSE Two Leaf.ocui T\n";
        UiLayoutDoc tw;
        uiParseLayout(files["Twice.ocui"], tw);
        UiTree t4;
        check(uiLayoutCheckUses(tw, "Twice.ocui", loader) && uiInstantiateLayoutEx(tw, t4, 0, loader, "Twice.ocui").root != 0, "the same layout twice is fine");

        // depth
        for (int i = 0; i < 20; ++i)
            files["D" + std::to_string(i) + ".ocui"] = "OCUI 2\nWIDGET R panel -\nUSE N D" + std::to_string(i + 1) + ".ocui R\n";
        files["D20.ocui"] = "OCUI 2\nWIDGET R panel -\n";
        UiLayoutDoc deep;
        uiParseLayout(files["D0.ocui"], deep);
        UiTree t5;
        const UiInstantiateResult rd = uiInstantiateLayoutEx(deep, t5, 0, loader);
        check(rd.root == 0 && !rd.errors.empty() && rd.errors[0].find("deep") != std::string::npos, "nesting past 16 levels fails with an error");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all layout asset checks passed");
    return 0;
}
