// Headless test for the .octheme resource (G5 U1): the text form, the lookup order of Control.get_theme_*, type variations, and the
// built-in default theme.
#include "aver/core/Log.hpp"
#include "aver/ui/UiThemeSheet.hpp"

#include <string>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static const std::vector<std::string> kButtonChain = {"Button", "BaseButton", "Control"};

int main() {
    AVER_INFO("=== text form ===");
    {
        UiThemeSheet th;
        th.name = "Main Theme";
        th.defaultFont = "Fonts/ui font.ttf";
        th.defaultFontSize = 18;
        th.defaultBaseScale = 1.5f;
        th.variations["HeaderLabel"] = "Label";
        th.types["Label"].colors["font_color"] = 0xFF3080FF;
        th.types["Button"].constants["h_separation"] = 6;
        th.types["HeaderLabel"].fonts["font"] = "Fonts/bold.ttf";
        th.types["HeaderLabel"].fonts["empty"] = "";
        th.types["HeaderLabel"].fontSizes["font_size"] = 28;
        th.types["CheckBox"].icons["checked"] = "Textures/ok mark.png";
        UiThemeStyleBox fl;
        fl.box.kind = static_cast<i32>(UiStyleBoxKind::Flat);
        fl.box.bgColor = 0xFF2E2E2E;
        fl.box.borderL = 1; fl.box.borderT = 2; fl.box.borderR = 3; fl.box.borderB = 4;
        fl.box.borderColorL = 0xFF0000FF; fl.box.borderColorT = 0xFF00FF00; fl.box.borderColorR = 0xFFFF0000; fl.box.borderColorB = 0xFFFFFFFF;
        fl.box.radiusTL = 4; fl.box.radiusTR = 5; fl.box.radiusBR = 6; fl.box.radiusBL = 7;
        fl.box.cornerDetail = 12;
        fl.box.flags = kSbDrawCenter | kSbAntiAlias | kSbBorderBlend;
        fl.box.aaSize = 0.75f;
        fl.box.contentL = 8; fl.box.contentT = 4;
        fl.box.expandR = 2.5f;
        fl.box.skewX = 3;
        fl.box.shadowColor = 0x80102030; fl.box.shadowSize = 6; fl.box.shadowOffX = 1; fl.box.shadowOffY = 2;
        th.types["Button"].styleBoxes["normal"] = fl;
        UiThemeStyleBox tx;
        tx.box.kind = static_cast<i32>(UiStyleBoxKind::Texture);
        tx.texturePath = "Textures/panel frame.png";
        tx.box.texW = 64; tx.box.texH = 48;
        tx.box.regionX = 4; tx.box.regionY = 8; tx.box.regionW = 32; tx.box.regionH = 32;
        tx.box.patchL = 6; tx.box.patchT = 7; tx.box.patchR = 8; tx.box.patchB = 9;
        tx.box.axisH = 1; tx.box.axisV = 2;
        tx.box.modulate = 0x80FFFFFF;
        tx.box.flags = 0;
        th.types["Panel"].styleBoxes["panel"] = tx;
        UiThemeStyleBox ln;
        ln.box.kind = static_cast<i32>(UiStyleBoxKind::Line);
        ln.box.flags = kSbVertical;
        ln.box.lineColor = 0xFFAABBCC; ln.box.thickness = 3; ln.box.growBegin = 0; ln.box.growEnd = 4;
        th.types["VSeparator"].styleBoxes["separator"] = ln;
        UiThemeStyleBox em;
        em.box.kind = static_cast<i32>(UiStyleBoxKind::Empty);
        em.box.contentL = 5;
        th.types["Label"].styleBoxes["normal"] = em;

        const std::string text = uiThemeWrite(th);
        UiThemeSheet back;
        std::string err;
        check(uiThemeParse(text, back, &err), "the written text parses: " + err);
        check(uiThemeWrite(back) == text, "write(parse(write(x))) == write(x)");
        check(back.name == "Main Theme" && back.defaultFont == "Fonts/ui font.ttf" && back.defaultFontSize == 18 && back.defaultBaseScale == 1.5f, "the header values survive (with spaces)");
        check(back.variations.at("HeaderLabel") == "Label", "a variation keeps its base");
        check(back.types.at("Label").colors.at("font_color") == 0xFF3080FF, "colours survive");
        check(back.types.at("Button").constants.at("h_separation") == 6, "constants survive");
        check(back.types.at("HeaderLabel").fonts.at("empty").empty() && back.types.at("HeaderLabel").fonts.at("font") == "Fonts/bold.ttf", "fonts survive, an empty path too");
        check(back.types.at("CheckBox").icons.at("checked") == "Textures/ok mark.png", "icons survive");
        const UiStyleBox& b = back.types.at("Button").styleBoxes.at("normal").box;
        check(b.bgColor == 0xFF2E2E2E && b.borderL == 1 && b.borderB == 4 && b.borderColorT == 0xFF00FF00 && b.borderColorB == 0xFFFFFFFF, "a flat box: background and per-side borders and colours");
        check(b.radiusTL == 4 && b.radiusBL == 7 && b.cornerDetail == 12 && b.aaSize == 0.75f && b.flags == (kSbDrawCenter | kSbAntiAlias | kSbBorderBlend), "radii, detail, anti-aliasing, flags");
        check(b.contentL == 8 && b.contentT == 4 && b.contentR == -1 && b.expandR == 2.5f && b.skewX == 3, "content margins (unset stays unset), expand, skew");
        check(b.shadowColor == 0x80102030 && b.shadowSize == 6 && b.shadowOffX == 1 && b.shadowOffY == 2, "shadow");
        const UiThemeStyleBox& t = back.types.at("Panel").styleBoxes.at("panel");
        check(t.texturePath == "Textures/panel frame.png" && t.box.texW == 64 && t.box.regionW == 32 && t.box.patchB == 9 && t.box.axisH == 1 && t.box.axisV == 2 && t.box.modulate == 0x80FFFFFF && t.box.flags == 0,
              "a texture box: path with a space, size, region, margins, axes, modulate, no draw_center");
        const UiStyleBox& l = back.types.at("VSeparator").styleBoxes.at("separator").box;
        check(l.kind == static_cast<i32>(UiStyleBoxKind::Line) && l.lineColor == 0xFFAABBCC && l.thickness == 3 && l.growEnd == 4 && (l.flags & kSbVertical), "a line box");
        check(back.types.at("Label").styleBoxes.at("normal").box.kind == static_cast<i32>(UiStyleBoxKind::Empty), "an empty box");
        check(back.itemCount() == th.itemCount() && back.itemCount() == 10, "no item lost or invented");
    }

    AVER_INFO("=== reading: leniency and errors ===");
    {
        UiThemeSheet th;
        std::string err;
        check(uiThemeParse("OCTHEME 1\r\n# a comment\r\n\r\nCOLOR Label font_color #FF0000FF\r\nFUTURE thing 1 2 3\r\nCONSTANT Button margin 4\r\n", th, &err), "CRLF, comments, blank lines and unknown records are accepted");
        check(th.types["Label"].colors["font_color"] == 0xFF0000FF && th.types["Button"].constants["margin"] == 4, "and the rest is read");
        check(!uiThemeParse("COLOR Label font_color #FF0000FF\n", th, &err) && err.find("line 1") != std::string::npos, "a document without the header is refused, with a line number");
        check(!uiThemeParse("OCTHEME 1\nCOLOR Label font_color red\n", th, &err) && err.find("line 2") != std::string::npos, "a bad colour is refused with its line");
        check(!uiThemeParse("OCTHEME 1\nCONSTANT Button x notanumber\n", th, &err), "a bad constant is refused");
        check(!uiThemeParse("OCTHEME 1\nSTYLEBOX Button normal blob\n", th, &err), "an unknown box kind is refused");
        check(!uiThemeParse("OCTHEME 1\nSTYLEBOX Button normal flat bg=zzz\n", th, &err), "a bad box value is refused");
        const std::string keep = uiThemeWrite(th);
        UiThemeSheet other;
        check(!uiThemeParse("garbage", other, &err), "garbage is refused");
        check(uiThemeWrite(th) == keep, "a failed parse leaves nothing half-loaded in the output only when the caller keeps its own");
        check(uiThemeParse("OCTHEME 1\nSTYLEBOX Button normal flat future_key=1 bg=#00000000\n", th, &err), "an unknown box key is skipped");
        check(uiThemeParse("OCTHEME 1", th, &err) && th.itemCount() == 0, "an empty theme is a theme");
    }

    AVER_INFO("=== type list ===");
    {
        UiThemeSheet project, node;
        project.variations["FlatButton"] = "Button";
        node.variations["BigFlat"] = "FlatButton";
        const std::vector<const UiThemeSheet*> chain = {&node, &project};
        auto l = uiThemeTypeList(chain, kButtonChain, "");
        check(l == std::vector<std::string>({"Button", "BaseButton", "Control"}), "no variation: the class chain");
        l = uiThemeTypeList(chain, kButtonChain, "FlatButton");
        check(l == std::vector<std::string>({"FlatButton", "Button", "BaseButton", "Control"}), "a variation comes first, its base class follows");
        l = uiThemeTypeList(chain, kButtonChain, "BigFlat");
        check(l == std::vector<std::string>({"BigFlat", "FlatButton", "Button", "BaseButton", "Control"}), "a variation of a variation walks the whole chain");
        l = uiThemeTypeList(chain, kButtonChain, "Unknown");
        check(l == std::vector<std::string>({"Unknown", "Button", "BaseButton", "Control"}), "an undeclared variation is just an extra type");
        UiThemeSheet loop;
        loop.variations["A"] = "B"; loop.variations["B"] = "A";
        l = uiThemeTypeList({&loop}, kButtonChain, "A");
        check(l.size() == 5 && l[0] == "A" && l[1] == "B", "a cycle of variations ends");
    }

    AVER_INFO("=== lookup order ===");
    {
        UiThemeSheet def, project, parentTheme, nodeTheme;
        def.types["Button"].colors["font_color"] = 0xFF000001;
        def.types["Button"].colors["only_default"] = 0xFF00000D;
        def.types["Control"].colors["font_color"] = 0xFF00000C;
        project.types["Button"].colors["font_color"] = 0xFF000002;
        project.types["BaseButton"].colors["from_base"] = 0xFF000003;
        parentTheme.types["Control"].colors["font_color"] = 0xFF000004;   // a less specific type in a nearer theme
        nodeTheme.types["Button"].colors["other"] = 0xFF000005;
        const auto types = uiThemeTypeList({&nodeTheme, &parentTheme, &project, &def}, kButtonChain, "");
        u32 c = 0;
        const UiThemeSheet* from = uiThemeFindColor({&nodeTheme, &parentTheme, &project, &def}, types, "font_color", c);
        check(from == &parentTheme && c == 0xFF000004, "a nearer theme wins even with a less specific type");
        from = uiThemeFindColor({&nodeTheme, &project, &def}, types, "font_color", c);
        check(from == &project && c == 0xFF000002, "without it, the project theme beats the default");
        from = uiThemeFindColor({&nodeTheme, &project, &def}, types, "only_default", c);
        check(from == &def && c == 0xFF00000D, "and the default theme answers last");
        from = uiThemeFindColor({&nodeTheme, &project, &def}, types, "from_base", c);
        check(from == &project && c == 0xFF000003, "a base class item is found");
        from = uiThemeFindColor({&nodeTheme, &project, &def}, types, "nothing", c);
        check(from == nullptr, "a missing item answers null");

        UiThemeSheet variants;
        variants.variations["Danger"] = "Button";
        variants.types["Danger"].colors["font_color"] = 0xFF0000AA;
        variants.types["Button"].colors["font_color"] = 0xFF0000BB;
        const auto vt = uiThemeTypeList({&variants}, kButtonChain, "Danger");
        from = uiThemeFindColor({&variants}, vt, "font_color", c);
        check(c == 0xFF0000AA, "inside one theme the variation beats the class");
        const auto plain = uiThemeTypeList({&variants}, kButtonChain, "");
        uiThemeFindColor({&variants}, plain, "font_color", c);
        check(c == 0xFF0000BB, "and the class is used without it");

        i32 k = 0;
        def.types["Button"].constants["h_separation"] = 4;
        check(uiThemeFindConstant({&def}, types, "h_separation", k) == &def && k == 4, "constants use the same order");
        UiThemeStyleBox sb;
        project.types["Button"].styleBoxes["normal"].box.bgColor = 0xFF123456;
        check(uiThemeFindStyleBox({&project, &def}, types, "normal", sb) == &project && sb.box.bgColor == 0xFF123456, "style boxes too");
        std::string s;
        project.types["Button"].icons["icon"] = "a.png";
        check(uiThemeFindIcon({&project}, types, "icon", s) && s == "a.png", "icons too");
        project.types["Button"].fonts["font"] = "f.ttf";
        check(uiThemeFindFont({&project}, types, "font", s) && s == "f.ttf", "fonts too");
        project.types["Button"].fontSizes["font_size"] = 21;
        check(uiThemeFindFontSize({&project}, types, "font_size", k) && k == 21, "font sizes too");

        UiThemeSheet fontTheme;
        fontTheme.defaultFont = "d.ttf";
        fontTheme.defaultFontSize = 20;
        check(uiThemeDefaultFont({&nodeTheme, &fontTheme, &def}, s) && s == "d.ttf", "the default font is the nearest theme that set one");
        check(uiThemeDefaultFontSize({&nodeTheme, &fontTheme}, 16) == 20 && uiThemeDefaultFontSize({&nodeTheme}, 16) == 16, "the default size likewise, else the fallback");
        check(!uiThemeDefaultFont({&nodeTheme}, s), "none set: false");
    }

    AVER_INFO("=== the default theme ===");
    {
        const UiThemeSheet& d = uiGodotDefaultTheme();
        check(d.defaultFontSize == 16 && d.defaultBaseScale == 1.0f, "default font size 16, base scale 1");
        check(d.hasItem(UiThemeItemKind::StyleBox, "Button", "normal") && d.hasItem(UiThemeItemKind::StyleBox, "Button", "focus"), "buttons have their five styles");
        check(d.hasItem(UiThemeItemKind::StyleBox, "Panel", "panel") && d.hasItem(UiThemeItemKind::StyleBox, "PanelContainer", "panel"), "panels have theirs");
        check(d.types.at("BoxContainer").constants.at("separation") == 4 && d.types.at("GridContainer").constants.at("h_separation") == 4, "box and grid separation are 4");
        check(d.types.at("Label").constants.at("line_spacing") == 3, "label line spacing is 3");
        const UiStyleBox& n = d.types.at("Button").styleBoxes.at("normal").box;
        check(n.contentL == 8 && n.contentT == 4 && n.radiusTL == 3, "button content margins are 8 and 4");
        check(!(d.types.at("Button").styleBoxes.at("focus").box.flags & kSbDrawCenter), "the focus ring is hollow");
        UiThemeSheet back;
        std::string err;
        check(uiThemeParse(uiGodotDefaultThemeText(), back, &err) && back.itemCount() == d.itemCount(), "the default theme text parses back to the same sheet");
        check(uiThemeWrite(back) == uiGodotDefaultThemeText(), "and writes the same text");
    }

    if (g_failures == 0) AVER_INFO("UiThemeSheetTest: all checks passed");
    else AVER_ERROR("UiThemeSheetTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
