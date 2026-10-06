// The .ocui layout asset: parse, write, instantiate, capture and the structural edits.
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace aver::ui {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

const char* const kScaleNames[] = {"constant", "width", "height", "shortest", "blend"};

const char* const kDisplayNames[] = {"Panel", "Text", "Image", "Button", "Toggle", "Slider",
                                     "Choice", "List", "Scroll", "TextInput", "Progress", "KeyBind"};

bool inRange(const UiLayoutDoc& d, i32 i) { return i >= 0 && static_cast<usize>(i) < d.nodes.size(); }

} // namespace

const char* uiScaleModeName(UiScaleMode m) {
    const usize i = static_cast<usize>(m);
    return i < 5 ? kScaleNames[i] : "height";
}

bool uiScaleModeFromName(std::string_view s, UiScaleMode& out) {
    for (usize i = 0; i < 5; ++i)
        if (ieq(s, kScaleNames[i])) { out = static_cast<UiScaleMode>(i); return true; }
    return false;
}

bool UiLayoutDoc::valid(std::string* why) const {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (nodes.empty()) return fail("a layout needs a root widget");
    if (nodes[0].parent != -1) return fail("node 0 must be the root");
    for (usize i = 0; i < nodes.size(); ++i) {
        const UiLayoutNode& n = nodes[i];
        if (n.props.name.empty()) return fail("widget " + std::to_string(i) + " has no name");
        if (i > 0 && (n.parent < 0 || static_cast<usize>(n.parent) >= i))
            return fail("widget '" + n.props.name + "' has no earlier parent");
        for (usize j = 0; j < i; ++j)
            if (nodes[j].props.name == n.props.name) return fail("duplicate widget name '" + n.props.name + "'");
    }
    return true;
}

// ---- parse ---------------------------------------------------------------------------------------

bool uiParseLayout(std::string_view text, UiLayoutDoc& out, std::string* err) {
    UiLayoutDoc doc;
    bool sawHeader = false;
    std::unordered_map<std::string, i32> index;
    usize lineNo = 0;
    const auto fail = [&](const std::string& m) {
        if (err) *err = ".ocui line " + std::to_string(lineNo) + ": " + m;
        return false;
    };

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        usize lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        line.remove_prefix(lead);
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> t;
        if (!uiTokenize(line, t)) return fail("unterminated quote");
        if (t.empty()) continue;

        if (ieq(t[0], "OCUI")) {
            i32 v = 0;
            if (t.size() < 2 || !uiParseInt(t[1], v)) return fail("OCUI needs a version number");
            if (v > kUiLayoutVersion) return fail("unsupported layout version " + std::to_string(v));
            doc.version = v;
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail("missing OCUI header");

        if (ieq(t[0], "NAME")) {
            doc.name.clear();
            for (usize i = 1; i < t.size(); ++i) { if (i > 1) doc.name += ' '; doc.name += t[i]; }
        } else if (ieq(t[0], "THEME")) {
            if (t.size() < 2) return fail("THEME needs a name");
            doc.theme = t[1];
        } else if (ieq(t[0], "SCALE")) {
            UiScaleMode m;
            f32 w = 0, h = 0;
            if (t.size() < 4 || !uiScaleModeFromName(t[1], m) || !uiParseFloat(t[2], w) || !uiParseFloat(t[3], h))
                return fail("SCALE needs <mode> <refWidth> <refHeight>");
            doc.dpi.mode = m;
            doc.dpi.refWidth = w;
            doc.dpi.refHeight = h;
        } else if (ieq(t[0], "WIDGET")) {
            if (t.size() < 4) return fail("WIDGET needs <name> <kind> <parent|->");
            UiWidgetKind kind;
            if (!uiKindFromName(t[2], kind)) return fail("unknown widget kind '" + t[2] + "'");
            if (index.count(t[1])) return fail("duplicate widget name '" + t[1] + "'");
            UiLayoutNode n;
            n.props = uiDefaultProps(kind);
            n.props.name = t[1];
            if (t[3] == "-") {
                if (!doc.nodes.empty()) return fail("a second root widget '" + t[1] + "'; a layout has one root");
                n.parent = -1;
            } else {
                const auto it = index.find(t[3]);
                if (it == index.end()) return fail("parent '" + t[3] + "' is not defined above '" + t[1] + "'");
                n.parent = it->second;
            }
            if (doc.nodes.empty() && n.parent != -1) return fail("the first widget must be the root (parent '-')");
            index[t[1]] = static_cast<i32>(doc.nodes.size());
            doc.nodes.push_back(std::move(n));
        } else if (ieq(t[0], "SET")) {
            if (t.size() < 3) return fail("SET needs <widget> <key> <values...>");
            const auto it = index.find(t[1]);
            if (it == index.end()) return fail("SET names unknown widget '" + t[1] + "'");
            const UiPropDesc* d = uiFindProperty(t[2]);
            if (!d) continue;   // a property from a newer build
            const std::vector<std::string> values(t.begin() + 3, t.end());
            if (!d->set(doc.nodes[static_cast<usize>(it->second)].props, values))
                return fail("bad value for '" + t[2] + "' on '" + t[1] + "'");
        }
        // anything else: a record from a newer build
    }

    if (!sawHeader) { if (err) *err = "not an .ocui: no OCUI header line"; return false; }
    std::string why;
    if (!doc.valid(&why)) { if (err) *err = ".ocui: " + why; return false; }
    out = std::move(doc);
    return true;
}

// ---- write ---------------------------------------------------------------------------------------

std::string uiWriteLayout(const UiLayoutDoc& doc) {
    std::string s = "OCUI " + std::to_string(doc.version) + "\n";
    if (!doc.name.empty()) s += "NAME " + uiQuote(doc.name) + "\n";
    s += "THEME " + uiQuote(doc.theme) + "\n";
    s += std::string("SCALE ") + uiScaleModeName(doc.dpi.mode) + " " + uiFormatFloat(doc.dpi.refWidth) + " " +
         uiFormatFloat(doc.dpi.refHeight) + "\n";

    for (const UiLayoutNode& n : doc.nodes) {
        const UiWidgetProps& p = n.props;
        s += "\nWIDGET " + uiQuote(p.name) + " " + uiKindName(p.kind) + " ";
        s += n.parent < 0 ? std::string("-") : uiQuote(doc.nodes[static_cast<usize>(n.parent)].props.name);
        s += "\n";
        const UiWidgetProps defaults = uiDefaultProps(p.kind);
        for (const UiPropDesc& d : uiProperties()) {
            std::vector<std::string> a, b;
            d.get(p, a);
            d.get(defaults, b);
            if (a == b) continue;
            s += "SET " + uiQuote(p.name) + " " + d.key;
            for (const std::string& tok : a) s += " " + uiQuote(tok);
            s += "\n";
        }
    }
    return s;
}

// ---- instantiate / capture ---------------------------------------------------------------------------------

UiWidgetId uiInstantiateLayout(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent) {
    if (!doc.valid()) return 0;
    std::vector<UiWidgetId> ids(doc.nodes.size(), 0);
    for (usize i = 0; i < doc.nodes.size(); ++i) {
        const UiLayoutNode& n = doc.nodes[i];
        const UiWidgetId par = n.parent < 0 ? parent : ids[static_cast<usize>(n.parent)];
        ids[i] = tree.createFrom(n.props, par);
        if (!ids[i]) {
            if (ids[0]) tree.destroy(ids[0]);
            return 0;
        }
    }
    return ids[0];
}

bool uiCaptureLayout(const UiTree& tree, UiWidgetId root, UiLayoutDoc& out) {
    const UiWidget* r = tree.get(root);
    if (!r) return false;
    UiLayoutDoc doc;
    const std::function<void(const UiWidget&, i32)> walk = [&](const UiWidget& w, i32 parent) {
        UiLayoutNode n;
        n.props = static_cast<const UiWidgetProps&>(w);
        n.parent = parent;
        n.props.name = uiUniqueWidgetName(doc, w.name.empty() ? std::string_view(uiKindName(w.kind)) : std::string_view(w.name));
        const i32 idx = static_cast<i32>(doc.nodes.size());
        doc.nodes.push_back(std::move(n));
        for (const UiWidgetId c : w.children)
            if (const UiWidget* k = tree.get(c)) walk(*k, idx);
    };
    walk(*r, -1);
    out = std::move(doc);
    return true;
}

// ---- structural edits ----------------------------------------------------------------------------------------

namespace {

using Order = std::vector<std::vector<i32>>;

Order captureOrder(const std::vector<UiLayoutNode>& nodes) {
    Order o(nodes.size());
    for (usize i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent >= 0) o[static_cast<usize>(nodes[i].parent)].push_back(static_cast<i32>(i));
    return o;
}

void emitDfs(const std::vector<UiLayoutNode>& old, const Order& order, i32 idx, i32 newParent,
             std::vector<UiLayoutNode>& out, std::vector<i32>& remap) {
    const i32 ni = static_cast<i32>(out.size());
    remap[static_cast<usize>(idx)] = ni;
    out.push_back(old[static_cast<usize>(idx)]);
    out.back().parent = newParent;
    for (const i32 c : order[static_cast<usize>(idx)]) emitDfs(old, order, c, ni, out, remap);
}

// Rewrites the nodes in pre-order from node 0; unreachable nodes drop out. Returns old -> new index.
std::vector<i32> rebuild(UiLayoutDoc& doc, const Order& order) {
    std::vector<i32> remap(doc.nodes.size(), -1);
    std::vector<UiLayoutNode> out;
    out.reserve(doc.nodes.size());
    if (!doc.nodes.empty()) emitDfs(doc.nodes, order, 0, -1, out, remap);
    doc.nodes = std::move(out);
    return remap;
}

bool isSelfOrAncestor(const UiLayoutDoc& d, i32 maybeAncestor, i32 index) {
    for (i32 w = index; w >= 0 && inRange(d, w); w = d.nodes[static_cast<usize>(w)].parent)
        if (w == maybeAncestor) return true;
    return false;
}

void removeFrom(std::vector<i32>& list, i32 v) { list.erase(std::remove(list.begin(), list.end(), v), list.end()); }

} // namespace

UiWidgetProps uiNewWidgetProps(UiWidgetKind kind) {
    UiWidgetProps p = uiDefaultProps(kind);
    switch (kind) {
        case UiWidgetKind::Panel: p.width = 200; p.height = 120; break;
        case UiWidgetKind::Text: p.text = "Text"; break;
        case UiWidgetKind::Image: p.width = 96; p.height = 96; break;
        case UiWidgetKind::Button: p.text = "Button"; break;
        case UiWidgetKind::Toggle: p.text = "Toggle"; break;
        case UiWidgetKind::Slider: p.width = 220; p.value = 0.5f; break;
        case UiWidgetKind::Choice: p.items = {"Option A", "Option B", "Option C"}; p.width = 220; break;
        case UiWidgetKind::List: p.items = {"Item 1", "Item 2", "Item 3"}; p.width = 220; p.height = 150; break;
        case UiWidgetKind::Scroll: p.width = 240; p.height = 160; p.spacing = 6; break;
        case UiWidgetKind::TextInput: p.placeholder = "Type here"; p.width = 220; break;
        case UiWidgetKind::ProgressBar: p.value = 0.5f; p.width = 220; p.height = 18; break;
        case UiWidgetKind::KeyBind: p.text = "Key"; p.width = 160; break;
        case UiWidgetKind::Count: break;
    }
    return p;
}

std::vector<i32> uiLayoutChildren(const UiLayoutDoc& doc, i32 parent) {
    std::vector<i32> out;
    for (usize i = 0; i < doc.nodes.size(); ++i)
        if (doc.nodes[i].parent == parent) out.push_back(static_cast<i32>(i));
    return out;
}

i32 uiLayoutFind(const UiLayoutDoc& doc, std::string_view name) {
    for (usize i = 0; i < doc.nodes.size(); ++i)
        if (doc.nodes[i].props.name == name) return static_cast<i32>(i);
    return -1;
}

std::string uiUniqueWidgetName(const UiLayoutDoc& doc, std::string_view base) {
    const std::string b = base.empty() ? std::string("Widget") : std::string(base);
    if (uiLayoutFind(doc, b) < 0) return b;
    for (int n = 2;; ++n) {
        const std::string candidate = b + "_" + std::to_string(n);
        if (uiLayoutFind(doc, candidate) < 0) return candidate;
    }
}

i32 uiLayoutAdd(UiLayoutDoc& doc, i32 parent, UiWidgetKind kind, std::string_view name) {
    if (!inRange(doc, parent)) return -1;
    UiLayoutNode n;
    n.props = uiNewWidgetProps(kind);
    const usize k = static_cast<usize>(kind);
    n.props.name = uiUniqueWidgetName(doc, name.empty() ? std::string_view(kDisplayNames[k < 12 ? k : 0]) : name);
    n.parent = parent;
    const i32 added = static_cast<i32>(doc.nodes.size());
    doc.nodes.push_back(std::move(n));
    const std::vector<i32> remap = rebuild(doc, captureOrder(doc.nodes));
    return remap[static_cast<usize>(added)];
}

i32 uiLayoutDelete(UiLayoutDoc& doc, i32 index) {
    if (!inRange(doc, index) || index == 0) return -1;
    const i32 parentOld = doc.nodes[static_cast<usize>(index)].parent;
    Order order = captureOrder(doc.nodes);
    removeFrom(order[static_cast<usize>(parentOld)], index);
    const std::vector<i32> remap = rebuild(doc, order);
    return remap[static_cast<usize>(parentOld)];
}

i32 uiLayoutReparent(UiLayoutDoc& doc, i32 index, i32 newParent, i32 position) {
    if (!inRange(doc, index) || !inRange(doc, newParent) || index == 0) return -1;
    if (isSelfOrAncestor(doc, index, newParent)) return -1;
    Order order = captureOrder(doc.nodes);
    removeFrom(order[static_cast<usize>(doc.nodes[static_cast<usize>(index)].parent)], index);
    std::vector<i32>& list = order[static_cast<usize>(newParent)];
    if (position < 0 || static_cast<usize>(position) >= list.size()) list.push_back(index);
    else list.insert(list.begin() + position, index);
    doc.nodes[static_cast<usize>(index)].parent = newParent;
    const std::vector<i32> remap = rebuild(doc, order);
    return remap[static_cast<usize>(index)];
}

i32 uiLayoutMoveSibling(UiLayoutDoc& doc, i32 index, i32 delta) {
    if (!inRange(doc, index) || index == 0 || delta == 0) return -1;
    Order order = captureOrder(doc.nodes);
    std::vector<i32>& list = order[static_cast<usize>(doc.nodes[static_cast<usize>(index)].parent)];
    const auto it = std::find(list.begin(), list.end(), index);
    const i32 pos = static_cast<i32>(it - list.begin());
    const i32 target = pos + (delta < 0 ? -1 : 1);
    if (target < 0 || static_cast<usize>(target) >= list.size()) return -1;
    std::swap(list[static_cast<usize>(pos)], list[static_cast<usize>(target)]);
    const std::vector<i32> remap = rebuild(doc, order);
    return remap[static_cast<usize>(index)];
}

i32 uiLayoutDuplicate(UiLayoutDoc& doc, i32 index) {
    if (!inRange(doc, index) || index == 0) return -1;
    Order order = captureOrder(doc.nodes);

    std::vector<i32> subtree;
    const std::function<void(i32)> gather = [&](i32 i) {
        subtree.push_back(i);
        for (const i32 c : order[static_cast<usize>(i)]) gather(c);
    };
    gather(index);

    std::unordered_map<i32, i32> copyOf;
    for (const i32 old : subtree) {
        UiLayoutNode n = doc.nodes[static_cast<usize>(old)];
        n.props.name = uiUniqueWidgetName(doc, doc.nodes[static_cast<usize>(old)].props.name);
        n.parent = old == index ? doc.nodes[static_cast<usize>(index)].parent : copyOf[doc.nodes[static_cast<usize>(old)].parent];
        copyOf[old] = static_cast<i32>(doc.nodes.size());
        doc.nodes.push_back(std::move(n));
    }
    const i32 copyRoot = copyOf[index];

    Order after = captureOrder(doc.nodes);
    std::vector<i32>& siblings = after[static_cast<usize>(doc.nodes[static_cast<usize>(index)].parent)];
    removeFrom(siblings, copyRoot);
    const auto it = std::find(siblings.begin(), siblings.end(), index);
    siblings.insert(it == siblings.end() ? siblings.end() : it + 1, copyRoot);
    const std::vector<i32> remap = rebuild(doc, after);
    return remap[static_cast<usize>(copyRoot)];
}

bool uiLayoutRename(UiLayoutDoc& doc, i32 index, std::string_view name) {
    if (!inRange(doc, index) || name.empty()) return false;
    const i32 existing = uiLayoutFind(doc, name);
    if (existing >= 0 && existing != index) return false;
    doc.nodes[static_cast<usize>(index)].props.name = std::string(name);
    return true;
}

UiLayoutDoc uiStarterLayout() {
    UiLayoutDoc d;
    d.name = "NewLayout";
    UiLayoutNode root;
    root.props = uiDefaultProps(UiWidgetKind::Panel);
    root.props.name = "Root";
    root.props.anchors = UiAnchors{0, 0, 1, 1};
    root.props.style = "clear";
    root.props.inputMode = UiInputMode::Menu;
    d.nodes.push_back(std::move(root));

    const i32 win = uiLayoutAdd(d, 0, UiWidgetKind::Panel, "Window");
    {
        UiWidgetProps& p = d.nodes[static_cast<usize>(win)].props;
        p.anchors = UiAnchors{0.5f, 0.5f, 0.5f, 0.5f};
        p.pivot = {0.5f, 0.5f};
        p.width = 420;
        p.height = -1;
        p.layout = UiLayoutMode::VStack;
        p.spacing = 12;
        p.padding = UiInsets{24, 24, 24, 24};
    }
    const i32 title = uiLayoutAdd(d, win, UiWidgetKind::Text, "Title");
    d.nodes[static_cast<usize>(title)].props.text = "Menu";
    d.nodes[static_cast<usize>(title)].props.style = "title";
    d.nodes[static_cast<usize>(title)].props.textAlign = UiTextAlign::Center;
    const char* const buttons[][2] = {{"Play", "play"}, {"Settings", "settings"}, {"Quit", "quit"}};
    for (const auto& b : buttons) {
        const i32 i = uiLayoutAdd(d, win, UiWidgetKind::Button, std::string(b[0]) + "Button");
        d.nodes[static_cast<usize>(i)].props.text = b[0];
        d.nodes[static_cast<usize>(i)].props.command = b[1];
    }
    d.nodes[0].props.defaultFocus = "PlayButton";
    return d;
}

} // namespace aver::ui
