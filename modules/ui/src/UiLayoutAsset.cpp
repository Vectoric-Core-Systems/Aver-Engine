// The .ocui layout asset: parse, write, instantiate, capture and the structural edits.
#include "aver/ui/UiLayoutAsset.hpp"

#include "aver/ui/UiAnimation.hpp"
#include "aver/ui/UiBindings.hpp"
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <initializer_list>
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

bool UiLayoutDoc::usesV2() const {
    if (!exposed.empty() || !slots.empty() || !anims.empty() || !binds.empty() || !events.empty()) return true;
    for (const UiLayoutNode& n : nodes) if (n.use) return true;
    return false;
}

bool UiLayoutDoc::valid(std::string* why) const {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (nodes.empty()) return fail("a layout needs a root widget");
    if (nodes[0].parent != -1) return fail("node 0 must be the root");
    if (nodes[0].use) return fail("the root cannot be a USE");
    for (usize i = 0; i < nodes.size(); ++i) {
        const UiLayoutNode& n = nodes[i];
        if (n.props.name.empty()) return fail("widget " + std::to_string(i) + " has no name");
        if (n.use && n.use->path.empty()) return fail("USE '" + n.props.name + "' has no path");
        if (i > 0 && (n.parent < 0 || static_cast<usize>(n.parent) >= i))
            return fail("widget '" + n.props.name + "' has no earlier parent");
        for (usize j = 0; j < i; ++j)
            if (nodes[j].props.name == n.props.name) return fail("duplicate widget name '" + n.props.name + "'");
    }
    const auto has = [&](const std::string& name) {
        for (const UiLayoutNode& n : nodes) if (n.props.name == name) return true;
        return false;
    };
    for (const UiLayoutExpose& e : exposed)
        if (!has(e.widget)) return fail("EXPOSE '" + e.name + "' names unknown widget '" + e.widget + "'");
    for (const UiLayoutSlot& s : slots)
        if (!has(s.widget)) return fail("SLOT '" + s.name + "' names unknown widget '" + s.widget + "'");
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
            if (v > kUiLayoutVersionMax) return fail("unsupported layout version " + std::to_string(v));
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
        } else if (doc.version >= 2 && ieq(t[0], "USE")) {
            if (t.size() < 4) return fail("USE needs <name> <path> <parent>");
            if (index.count(t[1])) return fail("duplicate widget name '" + t[1] + "'");
            if (t[3] == "-") return fail("a USE cannot be the root");
            const auto it = index.find(t[3]);
            if (it == index.end()) return fail("parent '" + t[3] + "' is not defined above '" + t[1] + "'");
            UiLayoutNode n;
            n.props = uiDefaultProps(UiWidgetKind::Panel);
            n.props.name = t[1];
            n.parent = it->second;
            n.use = UiLayoutUse{};
            n.use->path = t[2];
            index[t[1]] = static_cast<i32>(doc.nodes.size());
            doc.nodes.push_back(std::move(n));
        } else if (doc.version >= 2 && ieq(t[0], "OVERRIDE")) {
            if (t.size() < 4) return fail("OVERRIDE needs <instance> <name> <values...>");
            const auto it = index.find(t[1]);
            if (it == index.end() || !doc.nodes[static_cast<usize>(it->second)].use)
                return fail("OVERRIDE names '" + t[1] + "', which is not a USE");
            std::vector<UiLayoutUse::Override>& list = doc.nodes[static_cast<usize>(it->second)].use->overrides;
            UiLayoutUse::Override ov{t[2], std::vector<std::string>(t.begin() + 3, t.end())};
            bool replaced = false;
            for (UiLayoutUse::Override& o : list) if (o.name == ov.name) { o = ov; replaced = true; }
            if (!replaced) list.push_back(std::move(ov));
        } else if (doc.version >= 2 && ieq(t[0], "EXPOSE")) {
            if (t.size() < 4) return fail("EXPOSE needs <name> <widget> <key>");
            for (const UiLayoutExpose& e : doc.exposed) if (e.name == t[1]) return fail("duplicate EXPOSE name '" + t[1] + "'");
            doc.exposed.push_back({t[1], t[2], t[3]});
        } else if (doc.version >= 2 && ieq(t[0], "SLOT")) {
            if (t.size() < 3) return fail("SLOT needs <name> <widget>");
            for (const UiLayoutSlot& s : doc.slots) if (s.name == t[1]) return fail("duplicate SLOT name '" + t[1] + "'");
            doc.slots.push_back({t[1], t[2]});
        } else if (doc.version >= 2 && ieq(t[0], "ANIM")) {
            if (t.size() < 4) return fail("ANIM needs <name> <widget> <property> [<time> <value>]...");
            doc.anims.push_back({t[1], t[2], t[3], std::vector<std::string>(t.begin() + 4, t.end())});
        } else if (doc.version >= 2 && ieq(t[0], "BIND")) {
            if (t.size() < 4) return fail("BIND needs <widget> <property> <path>");
            doc.binds.push_back({t[1], t[2], t[3], std::vector<std::string>(t.begin() + 4, t.end())});
        } else if (doc.version >= 2 && ieq(t[0], "EVENT")) {
            if (t.size() < 5) return fail("EVENT needs <widget> <event> <action> <target>");
            doc.events.push_back({t[1], t[2], t[3], t[4], std::vector<std::string>(t.begin() + 5, t.end())});
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
    // Nothing new is written unless it is used: a document without v2 content keeps its own version.
    const i32 version = doc.usesV2() ? std::max(doc.version, 2) : doc.version;
    std::string s = "OCUI " + std::to_string(version) + "\n";
    if (!doc.name.empty()) s += "NAME " + uiQuote(doc.name) + "\n";
    s += "THEME " + uiQuote(doc.theme) + "\n";
    s += std::string("SCALE ") + uiScaleModeName(doc.dpi.mode) + " " + uiFormatFloat(doc.dpi.refWidth) + " " +
         uiFormatFloat(doc.dpi.refHeight) + "\n";

    for (const UiLayoutNode& n : doc.nodes) {
        const UiWidgetProps& p = n.props;
        if (n.use) s += "\nUSE " + uiQuote(p.name) + " " + uiQuote(n.use->path) + " ";
        else s += "\nWIDGET " + uiQuote(p.name) + " " + uiKindName(p.kind) + " ";
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
        if (n.use)
            for (const UiLayoutUse::Override& ov : n.use->overrides) {
                s += "OVERRIDE " + uiQuote(p.name) + " " + uiQuote(ov.name);
                for (const std::string& v : ov.values) s += " " + uiQuote(v);
                s += "\n";
            }
    }
    const auto line = [&s](const char* record, std::initializer_list<const std::string*> head, const std::vector<std::string>& tail) {
        s += record;
        for (const std::string* h : head) s += " " + uiQuote(*h);
        for (const std::string& t : tail) s += " " + uiQuote(t);
        s += "\n";
    };
    const std::vector<std::string> none;
    if (!doc.exposed.empty() || !doc.slots.empty()) s += "\n";
    for (const UiLayoutExpose& e : doc.exposed) line("EXPOSE", {&e.name, &e.widget, &e.key}, none);
    for (const UiLayoutSlot& sl : doc.slots) line("SLOT", {&sl.name, &sl.widget}, none);
    if (!doc.anims.empty()) s += "\n";
    for (const UiLayoutAnim& a : doc.anims) line("ANIM", {&a.name, &a.widget, &a.property}, a.keys);
    if (!doc.binds.empty()) s += "\n";
    for (const UiLayoutBind& b : doc.binds) line("BIND", {&b.widget, &b.property, &b.path}, b.rest);
    if (!doc.events.empty()) s += "\n";
    for (const UiLayoutEvent& e : doc.events) line("EVENT", {&e.widget, &e.event, &e.action, &e.target}, e.rest);
    return s;
}

// ---- instantiate / capture ---------------------------------------------------------------------------------

namespace {

constexpr usize kMaxUseDepth = 16;

std::string normPath(std::string_view p) {
    std::string out;
    for (const char c : p) out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    return out;
}

struct InstCtx {
    UiTree& tree;
    const UiLayoutLoader& loader;
    std::vector<std::string> stack;        // normalised paths being built, outermost first
    std::vector<std::string> displayStack;
    std::vector<std::string>* errors;
    bool fatal = false;
    UiLayoutRuntime runtime;               // the ANIM, BIND and EVENT records of everything built so far
};

std::string chainText(const InstCtx& cx, const std::string& again) {
    std::string s;
    for (const std::string& p : cx.displayStack) s += p + " -> ";
    return s + again;
}

// Reads a used layout. Empty `why` on success.
bool loadUsed(const UiLayoutLoader& loader, const std::string& path, UiLayoutDoc& out, std::string& why) {
    std::string text;
    if (!loader || !loader(path, text)) { why = "cannot find layout '" + path + "'"; return false; }
    std::string err;
    if (!uiParseLayout(text, out, &err)) { why = "layout '" + path + "': " + err; return false; }
    return true;
}

// The instance's own props laid over the used layout's root: every property that differs from a new Panel.
void overlayPlacement(UiWidgetProps& root, const UiWidgetProps& instance) {
    const UiWidgetProps defaults = uiDefaultProps(UiWidgetKind::Panel);
    for (const UiPropDesc& d : uiProperties()) {
        std::vector<std::string> a, b;
        d.get(instance, a);
        d.get(defaults, b);
        if (a != b) d.set(root, a);
    }
}

UiWidgetId makePlaceholder(InstCtx& cx, const UiWidgetProps& instance, UiWidgetId parent, const std::string& path) {
    UiWidgetProps ph = instance;
    ph.kind = UiWidgetKind::Panel;
    if (ph.width < 0) ph.width = 220;
    if (ph.height < 0) ph.height = 28;
    ph.borderWidth = 1.0f;
    ph.borderColor = uiRgba(220, 60, 60);
    const UiWidgetId id = cx.tree.createFrom(ph, parent);
    if (id) {
        UiWidgetProps t = uiNewWidgetProps(UiWidgetKind::Text);
        t.name = instance.name + "/Missing";
        t.text = "Missing layout: " + path;
        t.textColor = uiRgba(240, 120, 120);
        t.anchors = UiAnchors{0, 0, 1, 1};
        cx.tree.createFrom(t, id);
    }
    return id;
}

bool buildDoc(InstCtx& cx, const UiLayoutDoc& doc, std::vector<UiWidgetProps> props, UiWidgetId parent,
              const std::string& prefix, const std::string& rootFull, std::vector<UiWidgetId>& ids);

// One USE node: load, overlay, override, build. Returns the instance root (0 on a fatal error).
UiWidgetId buildUse(InstCtx& cx, const UiLayoutNode& n, const UiWidgetProps& instance, UiWidgetId parent,
                    const std::string& full, std::unordered_map<std::string, UiWidgetId>& slotsOut) {
    const UiLayoutUse& u = *n.use;
    const std::string key = normPath(u.path);
    for (usize i = 0; i < cx.stack.size(); ++i)
        if (cx.stack[i] == key) {
            cx.fatal = true;
            if (cx.errors) cx.errors->push_back("layout cycle: " + chainText(cx, u.path));
            return 0;
        }
    if (cx.stack.size() >= kMaxUseDepth) {
        cx.fatal = true;
        if (cx.errors) cx.errors->push_back("layouts nested more than " + std::to_string(kMaxUseDepth) + " deep: " + chainText(cx, u.path));
        return 0;
    }
    UiLayoutDoc used;
    std::string why;
    if (!loadUsed(cx.loader, u.path, used, why)) {
        if (cx.errors) cx.errors->push_back(why);
        return makePlaceholder(cx, instance, parent, u.path);
    }

    std::vector<UiWidgetProps> props;
    props.reserve(used.nodes.size());
    for (const UiLayoutNode& un : used.nodes) props.push_back(un.props);
    overlayPlacement(props[0], instance);
    for (const UiLayoutUse::Override& ov : u.overrides) {
        usize e = 0;
        while (e < used.exposed.size() && used.exposed[e].name != ov.name) ++e;
        if (e == used.exposed.size()) {
            if (cx.errors) cx.errors->push_back("layout '" + u.path + "' exposes no '" + ov.name + "'");
            continue;
        }
        const i32 target = uiLayoutFind(used, used.exposed[e].widget);
        const UiPropDesc* d = uiFindProperty(used.exposed[e].key);
        if (target < 0 || !d || !d->set(props[static_cast<usize>(target)], ov.values)) {
            if (cx.errors) cx.errors->push_back("cannot set '" + ov.name + "' on '" + u.path + "'");
        }
    }

    cx.stack.push_back(key);
    cx.displayStack.push_back(u.path);
    std::vector<UiWidgetId> cids;
    const bool ok = buildDoc(cx, used, std::move(props), parent, full + "/", full, cids);
    cx.stack.pop_back();
    cx.displayStack.pop_back();
    if (!ok || cx.fatal || cids.empty()) return 0;
    for (const UiLayoutSlot& s : used.slots) {
        const i32 w = uiLayoutFind(used, s.widget);
        if (w >= 0) slotsOut[s.name] = cids[static_cast<usize>(w)];
    }
    return cids[0];
}

// Creates `doc` under `parent`; inner widget names get `prefix`, the root is named `rootFull`.
// Adds the document's ANIM, BIND and EVENT records to the runtime. Names get `prefix`, so a nested layout's animation is
// "<instance>/<name>" and its widgets "<instance>/<widget>", the names they were built with.
void collectRuntime(InstCtx& cx, const UiLayoutDoc& doc, const std::vector<UiWidgetId>& ids, const std::string& prefix,
                    const std::string& rootFull) {
    const std::string rootOriginal = doc.nodes[0].props.name;
    const auto full = [&](const std::string& n) { return n == rootOriginal ? rootFull : prefix + n; };
    const auto err = [&](const std::string& what) { if (cx.errors) cx.errors->push_back(what); };
    std::vector<std::string> animErrors;
    for (UiAnimation& a : uiAnimationsFromDoc(doc, &animErrors)) {
        a.name = prefix + a.name;
        for (UiAnimTrack& t : a.tracks) t.widget = full(t.widget);
        cx.runtime.anims.push_back(std::move(a));
    }
    for (const std::string& e : animErrors) err(e);
    const auto idOf = [&](const std::string& widget) -> UiWidgetId {
        const i32 i = uiLayoutFind(doc, widget);
        return i < 0 ? 0 : ids[static_cast<usize>(i)];
    };
    for (const UiLayoutBind& b : doc.binds) {
        UiBinding nb;
        nb.widget = idOf(b.widget);
        nb.property = b.property;
        if (!nb.widget) { err("BIND " + b.widget + " " + b.property + ": no such widget"); continue; }
        if (!uiFindProperty(b.property)) { err("BIND " + b.widget + " " + b.property + ": unknown property"); continue; }
        if (!uiParseBindPath(b.path, nb.source, nb.path)) { err("BIND " + b.widget + " " + b.property + ": empty path"); continue; }
        for (usize i = 0; i + 1 < b.rest.size(); i += 2) {
            if (b.rest[i] == "format") nb.format = b.rest[i + 1];
            else if (b.rest[i] == "mode") nb.onChange = b.rest[i + 1] == "change";
        }
        cx.runtime.binds.push_back(std::move(nb));
    }
    for (const UiLayoutEvent& e : doc.events) {
        UiEventRoute r;
        r.widget = idOf(e.widget);
        if (!r.widget) { err("EVENT " + e.widget + " " + e.event + ": no such widget"); continue; }
        if (!uiRouteEventFromName(e.event, r.type, r.hoverEnter)) { err("EVENT " + e.widget + ": unknown event '" + e.event + "'"); continue; }
        if (!uiRouteActionFromName(e.action, r.action)) { err("EVENT " + e.widget + " " + e.event + ": unknown action '" + e.action + "'"); continue; }
        r.target = e.target;
        r.args = e.rest;
        cx.runtime.routes.push_back(std::move(r));
    }
}

bool buildDoc(InstCtx& cx, const UiLayoutDoc& doc, std::vector<UiWidgetProps> props, UiWidgetId parent,
              const std::string& prefix, const std::string& rootFull, std::vector<UiWidgetId>& ids) {
    ids.assign(doc.nodes.size(), 0);
    const std::string rootOriginal = doc.nodes[0].props.name;
    // A name the document refers to (navigation, default focus) follows the rename.
    const auto mapName = [&](const std::string& n) {
        if (n.empty()) return n;
        return n == rootOriginal ? rootFull : prefix + n;
    };
    std::vector<std::unordered_map<std::string, UiWidgetId>> useSlots(doc.nodes.size());
    for (usize i = 0; i < doc.nodes.size(); ++i) {
        if (cx.fatal) return false;
        const UiLayoutNode& n = doc.nodes[i];
        UiWidgetId par = n.parent < 0 ? parent : ids[static_cast<usize>(n.parent)];
        if (n.parent >= 0 && doc.nodes[static_cast<usize>(n.parent)].use) {
            const std::string& slot = props[i].slot;
            if (!slot.empty()) {
                const auto it = useSlots[static_cast<usize>(n.parent)].find(slot);
                if (it != useSlots[static_cast<usize>(n.parent)].end()) par = it->second;
                else if (cx.errors) cx.errors->push_back("'" + doc.nodes[static_cast<usize>(n.parent)].use->path + "' has no slot '" + slot + "'");
            }
        }
        UiWidgetProps p = std::move(props[i]);
        p.name = i == 0 ? rootFull : prefix + p.name;
        p.navUp = mapName(p.navUp);
        p.navDown = mapName(p.navDown);
        p.navLeft = mapName(p.navLeft);
        p.navRight = mapName(p.navRight);
        p.defaultFocus = mapName(p.defaultFocus);
        if (n.use) ids[i] = buildUse(cx, n, p, par, p.name, useSlots[i]);
        else ids[i] = cx.tree.createFrom(p, par);
        if (!ids[i]) return false;
    }
    collectRuntime(cx, doc, ids, prefix, rootFull);
    return true;
}

} // namespace

UiInstantiateResult uiInstantiateLayoutEx(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent,
                                          const UiLayoutLoader& loader, std::string_view selfPath) {
    UiInstantiateResult r;
    if (!doc.valid()) { r.errors.push_back("not a valid layout"); return r; }
    InstCtx cx{tree, loader, {}, {}, &r.errors, false};
    if (!selfPath.empty()) {
        cx.stack.push_back(normPath(selfPath));
        cx.displayStack.emplace_back(selfPath);
    }
    std::vector<UiWidgetProps> props;
    props.reserve(doc.nodes.size());
    for (const UiLayoutNode& n : doc.nodes) props.push_back(n.props);
    const bool ok = buildDoc(cx, doc, std::move(props), parent, std::string(), doc.nodes[0].props.name, r.nodeIds);
    if (!ok || cx.fatal) {
        if (!r.nodeIds.empty() && r.nodeIds[0]) tree.destroy(r.nodeIds[0]);
        r.nodeIds.clear();
        if (r.errors.empty()) r.errors.push_back("the layout's widgets could not be created");
        return r;
    }
    r.root = r.nodeIds[0];
    if (!cx.runtime.anims.empty() || !cx.runtime.binds.empty() || !cx.runtime.routes.empty()) tree.attachRuntime(r.root, std::move(cx.runtime));
    return r;
}

UiWidgetId uiInstantiateLayout(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent) {
    return uiInstantiateLayoutEx(doc, tree, parent, UiLayoutLoader{}).root;
}

namespace {

bool checkUses(const UiLayoutDoc& doc, const UiLayoutLoader& loader, std::vector<std::string>& stack,
               std::vector<std::string>& shown, std::string* err) {
    for (const UiLayoutNode& n : doc.nodes) {
        if (!n.use) continue;
        const std::string key = normPath(n.use->path);
        for (const std::string& s : stack)
            if (s == key) {
                if (err) {
                    *err = "layout cycle: ";
                    for (const std::string& d : shown) *err += d + " -> ";
                    *err += n.use->path;
                }
                return false;
            }
        if (stack.size() >= kMaxUseDepth) {
            if (err) *err = "layouts nested more than " + std::to_string(kMaxUseDepth) + " deep";
            return false;
        }
        UiLayoutDoc used;
        std::string why;
        if (!loadUsed(loader, n.use->path, used, why)) continue;
        stack.push_back(key);
        shown.push_back(n.use->path);
        const bool ok = checkUses(used, loader, stack, shown, err);
        stack.pop_back();
        shown.pop_back();
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool uiLayoutCheckUses(const UiLayoutDoc& doc, std::string_view selfPath, const UiLayoutLoader& loader, std::string* err) {
    std::vector<std::string> stack, shown;
    if (!selfPath.empty()) { stack.push_back(normPath(selfPath)); shown.emplace_back(selfPath); }
    return checkUses(doc, loader, stack, shown, err);
}

bool uiLayoutExposedProps(const UiLayoutDoc& doc, i32 index, const UiLayoutLoader& loader,
                          std::vector<UiExposedProp>& out, std::string* err) {
    out.clear();
    if (!inRange(doc, index) || !doc.nodes[static_cast<usize>(index)].use) {
        if (err) *err = "not a USE";
        return false;
    }
    const UiLayoutUse& u = *doc.nodes[static_cast<usize>(index)].use;
    UiLayoutDoc used;
    std::string why;
    if (!loadUsed(loader, u.path, used, why)) { if (err) *err = why; return false; }
    for (const UiLayoutExpose& e : used.exposed) {
        UiExposedProp p;
        p.name = e.name;
        p.widget = e.widget;
        p.key = e.key;
        for (const UiLayoutUse::Override& ov : u.overrides)
            if (ov.name == e.name) { p.value = ov.values; p.overridden = true; }
        if (!p.overridden) {
            const i32 w = uiLayoutFind(used, e.widget);
            if (const UiPropDesc* d = uiFindProperty(e.key); d && w >= 0) d->get(used.nodes[static_cast<usize>(w)].props, p.value);
        }
        out.push_back(std::move(p));
    }
    return true;
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

const std::vector<UiWidgetPreset>& uiWidgetPresets() {
    static const std::vector<UiWidgetPreset> presets = [] {
        std::vector<UiWidgetPreset> v;
        const auto add = [&](const char* label, const char* name, auto&& fill) {
            UiWidgetProps p = uiNewWidgetProps(UiWidgetKind::Panel);
            p.style = "clear";
            fill(p);
            v.push_back({label, name, std::move(p)});
        };
        add("Canvas Panel", "CanvasPanel", [](UiWidgetProps&) {});
        add("Vertical Box", "VerticalBox", [](UiWidgetProps& p) { p.layout = UiLayoutMode::VStack; p.spacing = 6; p.width = -1; p.height = -1; });
        add("Horizontal Box", "HorizontalBox", [](UiWidgetProps& p) { p.layout = UiLayoutMode::HStack; p.spacing = 6; p.width = -1; p.height = -1; });
        add("Grid Panel", "GridPanel", [](UiWidgetProps& p) { p.layout = UiLayoutMode::Grid; p.columns = 2; p.spacing = 6; p.width = -1; p.height = -1; });
        add("Uniform Grid Panel", "UniformGridPanel", [](UiWidgetProps& p) {
            p.layout = UiLayoutMode::Grid; p.uniform = true; p.columns = 2; p.spacing = 6; p.width = -1; p.height = -1;
        });
        add("Overlay", "Overlay", [](UiWidgetProps& p) { p.layout = UiLayoutMode::Overlay; });
        add("Wrap Box", "WrapBox", [](UiWidgetProps& p) { p.layout = UiLayoutMode::Wrap; p.spacing = 6; p.width = 300; p.height = -1; });
        add("Size Box", "SizeBox", [](UiWidgetProps& p) { p.layout = UiLayoutMode::Overlay; p.width = 200; p.height = 100; });
        add("Scale Box", "ScaleBox", [](UiWidgetProps& p) {
            p.layout = UiLayoutMode::Overlay; p.scaleFit = UiScaleFit::Contain; p.width = 240; p.height = 120;
        });
        add("Border", "Border", [](UiWidgetProps& p) {
            p.style.clear(); p.layout = UiLayoutMode::Overlay; p.padding = UiInsets{8, 8, 8, 8}; p.width = -1; p.height = -1;
        });
        add("Spacer", "Spacer", [](UiWidgetProps& p) { p.hit = UiHitMode::Never; p.width = 16; p.height = 16; });
        add("Widget Switcher", "WidgetSwitcher", [](UiWidgetProps& p) { p.layout = UiLayoutMode::Switcher; });
        return v;
    }();
    return presets;
}

i32 uiLayoutAddPreset(UiLayoutDoc& doc, i32 parent, usize preset, std::string_view name) {
    const std::vector<UiWidgetPreset>& all = uiWidgetPresets();
    if (!inRange(doc, parent) || preset >= all.size()) return -1;
    const UiWidgetPreset& pr = all[preset];
    const i32 idx = uiLayoutAdd(doc, parent, UiWidgetKind::Panel, name.empty() ? std::string_view(pr.name) : name);
    if (idx < 0) return -1;
    UiWidgetProps& p = doc.nodes[static_cast<usize>(idx)].props;
    const std::string keep = p.name;
    p = pr.props;
    p.name = keep;
    return idx;
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
    std::vector<std::string> before;
    for (const UiLayoutNode& n : doc.nodes) before.push_back(n.props.name);
    const std::vector<i32> remap = rebuild(doc, order);
    // Records about the deleted widgets go with them (an EXPOSE or SLOT of a missing widget makes the file invalid).
    std::vector<std::string> removed;
    for (const std::string& name : before) if (uiLayoutFind(doc, name) < 0) removed.push_back(name);
    const auto drop = [&](auto& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& r) {
            return std::find(removed.begin(), removed.end(), r.widget) != removed.end();
        }), list.end());
    };
    drop(doc.exposed);
    drop(doc.slots);
    drop(doc.anims);
    drop(doc.binds);
    drop(doc.events);
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
    const std::string old = doc.nodes[static_cast<usize>(index)].props.name;
    const std::string now(name);
    doc.nodes[static_cast<usize>(index)].props.name = now;
    const auto follow = [&](std::string& ref) { if (ref == old) ref = now; };
    for (UiLayoutExpose& e : doc.exposed) follow(e.widget);
    for (UiLayoutSlot& s : doc.slots) follow(s.widget);
    for (UiLayoutAnim& a : doc.anims) follow(a.widget);
    for (UiLayoutBind& b : doc.binds) follow(b.widget);
    for (UiLayoutEvent& e : doc.events) follow(e.widget);
    return true;
}

i32 uiLayoutAddUse(UiLayoutDoc& doc, i32 parent, std::string_view path, std::string_view name) {
    if (!inRange(doc, parent) || path.empty()) return -1;
    std::string base(name);
    if (base.empty()) {
        base = std::string(path);
        const usize slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base.erase(0, slash + 1);
        const usize dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > 0) base.erase(dot);
    }
    UiLayoutNode n;
    n.props = uiDefaultProps(UiWidgetKind::Panel);
    n.props.name = uiUniqueWidgetName(doc, base);
    n.parent = parent;
    n.use = UiLayoutUse{};
    n.use->path = std::string(path);
    const i32 added = static_cast<i32>(doc.nodes.size());
    doc.nodes.push_back(std::move(n));
    const std::vector<i32> remap = rebuild(doc, captureOrder(doc.nodes));
    return remap[static_cast<usize>(added)];
}

bool uiLayoutSetOverride(UiLayoutDoc& doc, i32 index, std::string_view name, std::vector<std::string> values) {
    if (!inRange(doc, index) || !doc.nodes[static_cast<usize>(index)].use || name.empty()) return false;
    std::vector<UiLayoutUse::Override>& list = doc.nodes[static_cast<usize>(index)].use->overrides;
    for (UiLayoutUse::Override& o : list)
        if (o.name == name) { o.values = std::move(values); return true; }
    list.push_back({std::string(name), std::move(values)});
    return true;
}

bool uiLayoutClearOverride(UiLayoutDoc& doc, i32 index, std::string_view name) {
    if (!inRange(doc, index) || !doc.nodes[static_cast<usize>(index)].use) return false;
    std::vector<UiLayoutUse::Override>& list = doc.nodes[static_cast<usize>(index)].use->overrides;
    const usize before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const UiLayoutUse::Override& o) { return o.name == name; }), list.end());
    return list.size() != before;
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
