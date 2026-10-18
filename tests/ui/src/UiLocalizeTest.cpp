// Headless test for localized widget text (docs/plans/wave2/DATA_TABLES_LOCALIZATION.md 4.3): UiTree::shown(),
// the resolver and its epoch, layout and draw using the resolved string, typed text and events staying raw,
// and the .ocui round trip keeping references byte-exact. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiScreens.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTree.hpp"

#include <map>
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

static UiEstimatedMetrics g_metrics;

static UiWidget& mk(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name) {
    return *t.get(t.create(k, parent, name));
}

static UiInputFrame at(f32 x, f32 y, u32 buttons = 0) {
    UiInputFrame f;
    f.pointerX = x;
    f.pointerY = y;
    f.buttons = buttons;
    return f;
}

static void step(UiTree& t, const UiInputFrame& in) { t.update(0.016f, in, g_metrics); }

static std::vector<UiEvent> drain(UiTree& t) {
    std::vector<UiEvent> out;
    UiEvent e;
    while (t.pollEvent(e)) out.push_back(e);
    return out;
}

// Records every text the tree asks to draw.
class RecordingPainter final : public UiPainter {
public:
    std::vector<std::string> texts;
    void pushClip(const UiRect&) override {}
    void popClip() override {}
    void fillRect(const UiRect&, u32) override {}
    void image(const UiRect&, u64, const UiUvRect&, u32) override {}
    void text(f32, f32, std::string_view s, f32, u32) override { texts.emplace_back(s); }
    f32 textWidth(std::string_view s, f32 px) const override { return g_metrics.textWidth(s, px); }
    f32 lineHeight(f32 px) const override { return g_metrics.lineHeight(px); }
    f32 ascent(f32 px) const override { return g_metrics.ascent(px); }
    bool has(const std::string& s) const {
        for (const std::string& t : texts) if (t == s) return true;
        return false;
    }
};

// A fake culture: a table of "Table:Key" -> text, switchable, with an epoch the tree polls.
struct FakeLoc {
    std::map<std::string, std::string> en, de;
    bool german = false;
    u32 epoch = 1;
    void set(bool g) { german = g; ++epoch; }
    bool resolve(std::string_view raw, std::string& out) const {
        UiTextRef r;
        if (!uiParseTextRef(raw, r)) return false;
        const auto& m = german ? de : en;
        const auto it = m.find(r.table + ":" + r.key);
        out = it != m.end() ? it->second : (r.hasDefault ? r.defaultText : "[" + r.table + ":" + r.key + "]");
        return true;
    }
};

// The same table of cases tests/formats/src/OcStringsTest.cpp feeds fmt::parseLocRef (copied: Aver.UI does not link Formats).
struct RefCase { const char* s; bool ok; const char* table; const char* key; const char* def; bool hasDef; };
static const RefCase kRefCases[] = {
    {"@UI:Play", true, "UI", "Play", "", false},
    {"@UI:Menu.Play", true, "UI", "Menu.Play", "", false},
    {"@UI:Menu.Play=Play", true, "UI", "Menu.Play", "Play", true},
    {"@UI:k=a=b", true, "UI", "k", "a=b", true},
    {"@UI:k=", true, "UI", "k", "", true},
    {"@Items_2:sword-name=A sword: sharp", true, "Items_2", "sword-name", "A sword: sharp", true},
    {"@@UI:Play", false, "", "", "", false},
    {"@mention", false, "", "", "", false},
    {"@", false, "", "", "", false},
    {"@:k", false, "", "", "", false},
    {"@UI:", false, "", "", "", false},
    {"@UI:=x", false, "", "", "", false},
    {"@U I:k", false, "", "", "", false},
    {"@UI:k k", false, "", "", "", false},
    {"@UI.x:k", false, "", "", "", false},
    {"UI:k", false, "", "", "", false},
    {"", false, "", "", "", false},
};

int main() {
    AVER_INFO("=== reference grammar ===");
    for (const RefCase& c : kRefCases) {
        UiTextRef r;
        const bool ok = uiParseTextRef(c.s, r);
        const bool same = ok == c.ok && (!ok || (r.table == c.table && r.key == c.key && r.defaultText == c.def && r.hasDefault == c.hasDef));
        check(same, std::string("uiParseTextRef \"") + c.s + "\"");
    }

    AVER_INFO("=== shown() ===");
    {
        UiTree t;
        check(t.shown("Play") == "Play" && t.shown("") == "", "a literal passes through");
        check(t.shown("@@mention") == "@mention", "@@ unescapes to a single @");
        check(t.shown("@mention") == "@mention" && t.shown("@:") == "@:", "a malformed @ string is literal");
        check(t.shown("@UI:Play=Start") == "Start", "no resolver: the inline default");
        check(t.shown("@UI:Play") == "[UI:Play]", "no resolver and no default: the bracket form");
        check(t.shown("@UI:Play=") == "", "an empty inline default shows nothing");

        FakeLoc loc;
        loc.en["UI:Play"] = "Play";
        loc.de["UI:Play"] = "Spielen";
        t.setTextResolver([&](std::string_view raw, std::string& out) { return loc.resolve(raw, out); }, [&] { return loc.epoch; });
        check(t.shown("@UI:Play=Fallback") == "Play", "a resolver answer beats the inline default");
        check(t.shown("@UI:Missing=Fallback") == "Fallback", "the resolver's own fallback is used when it has no key");
        loc.set(true);
        step(t, at(0, 0));
        check(t.shown("@UI:Play=Fallback") == "Spielen", "an epoch change drops the cache");
    }

    AVER_INFO("=== layout, draw ===");
    {
        UiTree t;
        t.setViewport({0, 0, 1920, 1080});
        FakeLoc loc;
        loc.en["UI:Play"] = "Play";
        loc.de["UI:Play"] = "Spielen und Gewinnen";
        t.setTextResolver([&](std::string_view raw, std::string& out) { return loc.resolve(raw, out); }, [&] { return loc.epoch; });

        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.style = "clear";
        root.inputMode = UiInputMode::Blocking;
        root.layout = UiLayoutMode::VStack;
        const UiWidgetId rid = root.id;
        const UiWidgetId btn = mk(t, UiWidgetKind::Button, rid, "Btn").id;
        t.get(btn)->text = "@UI:Play";
        t.get(btn)->alignH = UiAlign::Start;
        const UiWidgetId txt = mk(t, UiWidgetKind::Text, rid, "Txt").id;
        t.get(txt)->text = "@UI:Play";
        const UiWidgetId tog = mk(t, UiWidgetKind::Toggle, rid, "Tog").id;
        t.get(tog)->text = "@UI:Play";
        UiWidget& ch = mk(t, UiWidgetKind::Choice, rid, "Ch");
        ch.items = {"@UI:Play", "@UI:Other=Other"};
        const UiWidgetId chId = ch.id;
        UiWidget& li = mk(t, UiWidgetKind::List, rid, "Li");
        li.items = {"@UI:Play", "plain"};
        UiWidget& in = mk(t, UiWidgetKind::TextInput, rid, "In");
        in.placeholder = "@UI:Play";
        const UiWidgetId inId = in.id;

        step(t, at(0, 0));
        const f32 wEn = t.get(btn)->rect.w;
        loc.set(true);
        step(t, at(0, 0));
        const f32 wDe = t.get(btn)->rect.w;
        check(wDe > wEn, "a longer translation widens an auto-sized button");

        RecordingPainter p;
        t.layout(p);
        t.draw(p);
        check(p.has("Spielen und Gewinnen"), "button, text and toggle draw the resolved text");
        bool raw = false;
        for (const std::string& s : p.texts) if (s.find("@UI:") != std::string::npos) raw = true;
        check(!raw, "no reference reaches the painter");
        usize hits = 0;
        for (const std::string& s : p.texts) if (s == "Spielen und Gewinnen") ++hits;
        check(hits >= 6, "button, text, toggle, choice, list row and placeholder all resolve (" + std::to_string(hits) + ")");
        check(p.has("plain"), "a literal list row is untouched");

        loc.set(false);
        RecordingPainter q;
        t.layout(q);
        t.draw(q);
        check(q.has("Play") && !q.has("Spielen und Gewinnen"), "switching back flips everything without touching a widget");
        check(t.get(btn)->text == "@UI:Play", "the stored text stays the raw reference");

        // typed text is never resolved
        t.setText(inId, "@UI:Play");
        t.setFocus(inId);
        RecordingPainter r;
        t.layout(r);
        t.draw(r);
        check(r.has("@UI:Play"), "TextInput contents are drawn as typed");

        // events keep the raw item
        t.setSelected(chId, 0);
        drain(t);
        UiInputFrame f = at(0, 0);
        step(t, f);
        const UiRect cr = t.get(chId)->rect;
        step(t, at(cr.right() - 4, cr.y + cr.h * 0.5f, 1));
        step(t, at(cr.right() - 4, cr.y + cr.h * 0.5f, 0));
        bool rawEvent = false;
        for (const UiEvent& e : drain(t)) if (e.type == UiEventType::ValueChanged && e.text == "@UI:Other=Other") rawEvent = true;
        check(rawEvent, "a Choice event carries the raw item, index is the identity");
    }

    AVER_INFO("=== .ocui round trip ===");
    {
        UiLayoutDoc d = uiStarterLayout();
        const i32 btn = uiLayoutFind(d, "PlayButton");
        d.nodes[static_cast<usize>(btn)].props.text = "@UI:menu.play=Play = now";
        d.nodes[static_cast<usize>(btn)].props.items = {"@UI:a=A", "@@literal"};
        d.nodes[static_cast<usize>(btn)].props.placeholder = "@UI:ph";
        const std::string text = uiWriteLayout(d);
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(text, back, &err), "a layout with references parses: " + err);
        const UiWidgetProps& p = back.nodes[static_cast<usize>(uiLayoutFind(back, "PlayButton"))].props;
        check(p.text == "@UI:menu.play=Play = now" && p.items.size() == 2 && p.items[0] == "@UI:a=A" && p.items[1] == "@@literal" &&
              p.placeholder == "@UI:ph", "references survive byte-exact");
        check(uiWriteLayout(back) == text, "write(parse(write(doc))) == write(doc)");
    }

    AVER_INFO("=== built-in screens ===");
    {
        UiSettingsModel model = uiDefaultSettingsModel();
        UiRebindModel rebind;
        rebind.rows.push_back({"Jump", 0, "Jump", "Space", true});

        auto textOf = [](UiTree& t, UiWidgetId root, const char* name) { return t.shown(t.get(t.find(name, root))->text); };
        {
            UiTree t;
            UiSettingsScreen screen(t, model, &rebind);
            const UiWidgetId root = screen.build();
            check(textOf(t, root, "Back") == "Back" && textOf(t, root, "Title") == "Settings", "no resolver: the screen reads in English");
            check(textOf(t, root, "tab.0") == "Graphics" && textOf(t, root, "tab.2") == "Accessibility" && textOf(t, root, "tab.5") == "Controls", "tab names show their inline defaults");
            check(t.shown(model.find("graphics.quality")->choices[0]) == "Low", "choices show their inline defaults");
        }
        {
            UiTree t;
            FakeLoc loc;
            loc.german = true;
            loc.de["UI:common.back"] = "Zurueck";
            loc.de["UI:settings.title"] = "Einstellungen";
            loc.de["UI:settings.tab.graphics"] = "Grafik";
            loc.de["UI:settings.quality.low"] = "Niedrig";
            t.setTextResolver([&](std::string_view raw, std::string& out) { return loc.resolve(raw, out); }, [&] { return loc.epoch; });
            UiSettingsScreen screen(t, model, &rebind);
            const UiWidgetId root = screen.build();
            check(textOf(t, root, "Back") == "Zurueck" && textOf(t, root, "Title") == "Einstellungen", "a German table translates the screen");
            check(textOf(t, root, "tab.0") == "Grafik" && textOf(t, root, "tab.1") == "Audio", "and untranslated keys fall back to the default");
            check(t.shown(model.find("graphics.quality")->choices[0]) == "Niedrig", "choices translate");
        }
    }

    AVER_INFO("UiLocalize tests done: {} failure(s)", g_failures);
    return g_failures ? 1 : 0;
}
