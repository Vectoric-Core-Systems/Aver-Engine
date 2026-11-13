// The .ocui layout editor tab. See the header for the two-line SandboxApp hook and for why the canvas
// maths are free functions.

#include "UiLayoutEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiBindings.hpp"
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
#  include "FontSetEditor.hpp"
#  include "aver/ui/UiTextEngine.hpp"
#endif

namespace aver::editor {

using namespace aver::ui;

namespace {

bool inRange(const UiLayoutDoc& d, i32 i) { return i >= 0 && static_cast<usize>(i) < d.nodes.size(); }

bool stretched(f32 lo, f32 hi) { return hi - lo > 1e-5f; }

// Widgets that can hold other widgets in a layout. Everything else is a leaf a designer drops
// things next to, not into.
bool isContainer(UiWidgetKind k) { return k == UiWidgetKind::Panel || k == UiWidgetKind::Scroll; }

// The point through the inverse of a widget's own render transform, so a click selects what is drawn there.
bool unmap(const UiAffine& m, f32& x, f32& y) {
    const f32 det = m.a * m.d - m.b * m.c;
    if (!(std::fabs(det) > 1e-12f)) return false;
    const f32 dx = x - m.tx, dy = y - m.ty;
    x = (m.d * dx - m.c * dy) / det;
    y = (m.a * dy - m.b * dx) / det;
    return true;
}

UiWidgetId pickIn(const UiTree& t, const UiWidget& w, f32 x, f32 y) {
    if (!w.visible || w.inactive) return 0;
    const UiAffine xf = t.renderAffine(w);
    if (!xf.identity() && !unmap(xf, x, y)) return 0;
    const bool inside = w.rect.contains(x, y);
    if ((w.clip || w.kind == UiWidgetKind::Scroll || w.kind == UiWidgetKind::List) && !inside) return 0;
    for (usize i = w.children.size(); i-- > 0;)
        if (const UiWidget* k = t.get(w.children[i]))
            if (const UiWidgetId h = pickIn(t, *k, x, y)) return h;
    return inside ? w.id : 0;
}

// The project's content root for an asset: the last "Content" folder above it, else the asset's own folder.
std::string contentRoot(const std::string& assetPath) {
    std::string p = assetPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    usize found = std::string::npos;
    usize at = 0;
    while (at < p.size()) {
        const usize s = p.find('/', at);
        const usize end = s == std::string::npos ? p.size() : s;
        if (s != std::string::npos && end - at == 7) {
            std::string seg = p.substr(at, 7);
            std::transform(seg.begin(), seg.end(), seg.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (seg == "content") found = end;
        }
        if (s == std::string::npos) break;
        at = s + 1;
    }
    if (found != std::string::npos) return p.substr(0, found);
    const usize slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
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

namespace {
UiLayoutEditorLoc g_loc;
}

void setUiLayoutEditorLoc(UiLayoutEditorLoc loc) { g_loc = std::move(loc); }

UiLayoutEditor::UiLayoutEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

// A USE path is looked up under the content root, then beside this file and in the folders above it.
UiLayoutLoader UiLayoutEditor::loader() const {
    const std::string self = path_;
    return [self](const std::string& usePath, std::string& text) {
        namespace fs = std::filesystem;
        std::vector<fs::path> bases{fs::path(contentRoot(self))};
        for (fs::path dir = fs::path(self).parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
            bases.push_back(dir);
        for (const fs::path& b : bases) {
            std::error_code ec;
            const fs::path candidate = b / usePath;
            if (fs::is_regular_file(candidate, ec) && readFileText(candidate.string(), text)) return true;
        }
        return false;
    };
}

std::string UiLayoutEditor::selfUsePath() const {
    std::string self = path_, root = contentRoot(path_);
    std::replace(self.begin(), self.end(), '\\', '/');
    if (self.size() > root.size() + 1 && self.compare(0, root.size(), root) == 0 && self[root.size()] == '/') return self.substr(root.size() + 1);
    return std::string();
}

bool UiLayoutEditor::useAllowed(const std::string& usePath, i32 replacing) {
    UiLayoutDoc tmp = doc_;
    if (replacing >= 0 && inRange(tmp, replacing) && tmp.nodes[static_cast<usize>(replacing)].use) tmp.nodes[static_cast<usize>(replacing)].use->path = usePath;
    else if (uiLayoutAddUse(tmp, 0, usePath) < 0) { status_ = "That is not a layout path."; return false; }
    std::string why;
    if (!uiLayoutCheckUses(tmp, selfUsePath(), loader(), &why)) { status_ = why; return false; }
    status_.clear();
    return true;
}

std::vector<std::string> UiLayoutEditor::projectLayouts() const {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const fs::path root = contentRoot(path_);
    const std::string self = selfUsePath();
    std::error_code ec;
    usize seen = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && seen < 5000; it.increment(ec), ++seen) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".ocui") continue;
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (rel.empty() || rel == self) continue;
        out.push_back(std::move(rel));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<UiExposedProp> UiLayoutEditor::exposedOf(i32 index, std::string* err) const {
    std::vector<UiExposedProp> out;
    uiLayoutExposedProps(doc_, index, loader(), out, err);
    return out;
}

i32 UiLayoutEditor::addUse(const std::string& usePath) {
    if (!loaded_ || !useAllowed(usePath, -1)) return -1;
    pushUndo();
    const i32 added = uiLayoutAddUse(doc_, inRange(doc_, selected_) ? selected_ : 0, usePath);
    if (added < 0) { history_.cancelPush(); return -1; }
    selected_ = added;
    dirty_ = true;
    rebuildPreview();
    return added;
}

bool UiLayoutEditor::setUsePath(i32 index, const std::string& usePath) {
    if (!loaded_ || !inRange(doc_, index) || !doc_.nodes[static_cast<usize>(index)].use || usePath.empty()) return false;
    if (doc_.nodes[static_cast<usize>(index)].use->path == usePath) return true;
    if (!useAllowed(usePath, index)) return false;
    pushUndo();
    doc_.nodes[static_cast<usize>(index)].use->path = usePath;
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::setOverride(i32 index, const std::string& name, std::string_view valueText) {
    if (!loaded_ || !inRange(doc_, index) || !doc_.nodes[static_cast<usize>(index)].use) return false;
    std::vector<std::string> tokens;
    if (!uiTokenize(valueText, tokens) || tokens.empty()) return false;
    pushUndo();
    if (!uiLayoutSetOverride(doc_, index, name, std::move(tokens))) { history_.cancelPush(); return false; }
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::clearOverride(i32 index, const std::string& name) {
    if (!loaded_ || !inRange(doc_, index)) return false;
    pushUndo();
    if (!uiLayoutClearOverride(doc_, index, name)) { history_.cancelPush(); return false; }
    dirty_ = true;
    rebuildPreview();
    return true;
}

// The preview shows the preview culture: its own tree resolves references through the host's store.
void UiLayoutEditor::applyPreviewLoc() {
    if (g_loc.resolve) preview_.setTextResolver(g_loc.resolve, g_loc.epoch);
    else preview_.setTextResolver({}, {});
}

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
    reloadTheme();
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
    if (themeIsSheet() && themeDirty_ && !writeTheme(why)) return false;
    if (!writeFileTextAtomic(path_, uiWriteLayout(doc_))) {
        if (why) *why = "cannot write " + path_;
        return false;
    }
    dirty_ = false;
    themeDirty_ = false;
    return true;
}

// ---- the theme ------------------------------------------------------------------------------------------------

namespace {

std::string themeFilePath(const std::string& layoutPath, const std::string& themePath) {
    return (std::filesystem::path(contentRoot(layoutPath)) / themePath).string();
}

std::string themeKey(const UiTheme& t, const std::string& base) { return uiThemeWrite(uiThemeToSheet(t, base)); }

} // namespace

void UiLayoutEditor::reloadTheme() {
    themeError_.clear();
    themeSheet_ = UiThemeSheet{};
    themeBase_ = "dark";
    themeDirty_ = false;
    if (!themeIsSheet()) {
        theme_ = uiBuiltinTheme(doc_.theme);
        return;
    }
    std::string text, why;
    if (!readFileText(themeFilePath(path_, doc_.theme), text)) why = "cannot read theme '" + doc_.theme + "'";
    else if (!uiThemeParse(text, themeSheet_, &why)) why = "theme '" + doc_.theme + "': " + why;
    if (!why.empty()) {
        themeSheet_ = UiThemeSheet{};
        themeError_ = why;
        theme_ = uiBuiltinTheme("dark");
        return;
    }
    themeBase_ = themeSheet_.baseTheme.empty() ? "dark" : themeSheet_.baseTheme;
    theme_ = uiThemeFromSheet(themeSheet_);
    if (theme_.name.empty()) theme_.name = std::filesystem::path(doc_.theme).stem().string();
}

// The sheet as it was read, with the Aver items replaced by the edited theme's: what other tools keep in the file stays.
bool UiLayoutEditor::writeTheme(std::string* why) {
    UiThemeSheet out = themeSheet_;
    for (auto it = out.types.begin(); it != out.types.end();) {
        if (it->first == "Aver" || it->first.compare(0, 5, "Aver.") == 0) it = out.types.erase(it);
        else ++it;
    }
    const UiThemeSheet fresh = uiThemeToSheet(theme_, themeBase_);
    for (const auto& [k, v] : fresh.types) out.types[k] = v;
    out.baseTheme = themeBase_;
    out.name = theme_.name;
    const std::string file = themeFilePath(path_, doc_.theme);
    if (!writeFileTextAtomic(file, uiThemeWrite(out))) {
        if (why) *why = "cannot write " + file;
        return false;
    }
    themeSheet_ = std::move(out);
    return true;
}

std::vector<std::string> UiLayoutEditor::projectThemes() const {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const fs::path root = contentRoot(path_);
    std::error_code ec;
    usize seen = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && seen < 5000; it.increment(ec), ++seen) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (!rel.empty() && uiThemeIsSheetPath(rel)) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool UiLayoutEditor::createThemeSheet(std::string relPath) {
    namespace fs = std::filesystem;
    if (!loaded_) return false;
    std::replace(relPath.begin(), relPath.end(), '\\', '/');
    if (!uiThemeIsSheetPath(relPath)) relPath += ".octheme";
    if (relPath.size() <= 8 || relPath[0] == '/' || relPath.find("..") != std::string::npos || relPath.find(':') != std::string::npos) {
        status_ = "not a theme path under the content folder: " + relPath;
        return false;
    }
    const std::string file = themeFilePath(path_, relPath);
    std::error_code ec;
    if (fs::exists(file, ec)) { status_ = "'" + relPath + "' exists already"; return false; }
    std::string base = themeIsSheet() ? themeBase_ : doc_.theme;
    if (uiBuiltinTheme(base).name != base) base = "dark";
    UiThemeSheet sheet = uiThemeToSheet(theme_, base);
    sheet.name = fs::path(relPath).stem().string();
    fs::create_directories(fs::path(file).parent_path(), ec);
    if (!writeFileTextAtomic(file, uiThemeWrite(sheet))) { status_ = "cannot write " + file; return false; }
    pushUndo();
    doc_.theme = relPath;
    dirty_ = true;
    reloadTheme();
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::editTheme(const std::function<void(UiTheme&)>& edit, bool newUndoEntry) {
    if (!loaded_ || !themeIsSheet() || !themeError_.empty() || !edit) return false;
    UiTheme next = theme_;
    edit(next);
    if (themeKey(next, themeBase_) == themeKey(theme_, themeBase_)) return false;
    if (newUndoEntry) pushUndo();
    theme_ = std::move(next);
    themeDirty_ = true;
    dirty_ = true;
    preview_.setTheme(theme_);
    preview_.layout(metrics_ ? *metrics_ : static_cast<const UiTextMetrics&>(estimate_));
    ++structureVersion_;
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

void UiLayoutEditor::select(i32 index) {
    selected_ = inRange(doc_, index) ? index : 0;
    extraSelected_.clear();
}

void UiLayoutEditor::pushUndo() { history_.push(Snapshot{doc_, theme_}); }

// Puts an undo state back. When it names another theme than the one in force, that theme's sheet is read again and the
// restored edits laid over it.
void UiLayoutEditor::restore(Snapshot s) {
    const bool otherTheme = doc_.theme != s.doc.theme;
    doc_ = std::move(s.doc);
    if (otherTheme) reloadTheme();
    if (!otherTheme || themeIsSheet()) theme_ = std::move(s.theme);
    if (themeIsSheet() && themeError_.empty()) themeDirty_ = true;
    dirty_ = true;
    if (!inRange(doc_, selected_)) selected_ = 0;
    rebuildPreview();
}

void UiLayoutEditor::undo() {
    Snapshot s{doc_, theme_};
    if (!history_.undo(s)) return;
    restore(std::move(s));
}

void UiLayoutEditor::redo() {
    Snapshot s{doc_, theme_};
    if (!history_.redo(s)) return;
    restore(std::move(s));
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

i32 UiLayoutEditor::addPreset(usize preset) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 added = uiLayoutAddPreset(doc_, inRange(doc_, selected_) ? selected_ : 0, preset);
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
    reloadTheme();
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

i32 UiLayoutEditor::placeWidgetAt(UiWidgetKind kind, f32 px, f32 py) { return placeAt(kind, -1, nullptr, px, py); }

i32 UiLayoutEditor::placePresetAt(usize preset, f32 px, f32 py) { return placeAt(UiWidgetKind::Panel, static_cast<i64>(preset), nullptr, px, py); }

i32 UiLayoutEditor::placeUseAt(const std::string& usePath, f32 px, f32 py) { return placeAt(UiWidgetKind::Panel, -1, &usePath, px, py); }

i32 UiLayoutEditor::placeAt(UiWidgetKind kind, i64 preset, const std::string* usePath, f32 px, f32 py) {
    if (!loaded_) return -1;
    if (usePath && !useAllowed(*usePath, -1)) return -1;
    i32 parent = nodeOf(uiEditorPick(preview_, px, py));
    if (parent < 0) parent = 0;
    while (parent > 0 && !isContainer(doc_.nodes[static_cast<usize>(parent)].props.kind))
        parent = doc_.nodes[static_cast<usize>(parent)].parent;
    pushUndo();
    const i32 added = usePath ? uiLayoutAddUse(doc_, parent, *usePath)
                              : (preset >= 0 ? uiLayoutAddPreset(doc_, parent, static_cast<usize>(preset)) : uiLayoutAdd(doc_, parent, kind));
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

    // Overlay and switcher children share one box: there is no order to drag.
    if (parent.layout == UiLayoutMode::Overlay || parent.layout == UiLayoutMode::Switcher) return;

    // Inside a stack, wrap or grid the position is not the widget's to choose: dragging reorders instead.
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
    // The device combo follows a size that matches a preset (the current one first), else it is custom.
    const std::vector<UiDevicePreset>& all = uiEditorDevicePresets();
    const auto matches = [&](i32 i) { return i >= 0 && static_cast<usize>(i) < all.size() && all[static_cast<usize>(i)].width == previewW_ && all[static_cast<usize>(i)].height == previewH_; };
    if (!matches(device_)) {
        device_ = -1;
        for (i32 i = 0; static_cast<usize>(i) < all.size(); ++i) if (matches(i)) { device_ = i; break; }
    }
}

void UiLayoutEditor::layoutPreview(const UiTextMetrics& m) { preview_.layout(m); }

void UiLayoutEditor::rebuildPreview() {
    ++structureVersion_;
    status_.clear();
    preview_.clear();
    applyPreviewLoc();
    preview_.setTheme(theme_);
    preview_.setDpi(doc_.dpi);
    preview_.setViewport({0, 0, previewW_, previewH_});
    // Nested layouts are loaded for the preview; on a cycle the preview shows placeholders instead of nothing.
    UiInstantiateResult built = uiInstantiateLayoutEx(doc_, preview_, 0, loader(), selfUsePath());
    previewErrors_ = built.errors;
    if (!built.root) {
        preview_.clear();
        built = uiInstantiateLayoutEx(doc_, preview_, 0, UiLayoutLoader{});
    }
    previewIds_ = built.nodeIds;
    previewIds_.resize(doc_.nodes.size(), 0);
    preview_.layout(metrics_ ? *metrics_ : static_cast<const UiTextMetrics&>(estimate_));
    applyTimeline();
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

// A widget inside a used layout belongs to the USE node that made it.
i32 UiLayoutEditor::nodeOf(UiWidgetId id) const {
    for (const UiWidget* w = preview_.get(id); w; w = w->parent ? preview_.get(w->parent) : nullptr)
        for (usize i = 0; i < previewIds_.size(); ++i)
            if (previewIds_[i] == w->id) return static_cast<i32>(i);
    return -1;
}

// ---- palette search and device presets ----------------------------------------------------------------------

bool uiEditorPaletteMatch(std::string_view label, std::string_view query) {
    const auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string hay = lower(label);
    std::string word;
    const std::string q = lower(query) + " ";
    for (const char c : q) {
        if (c != ' ' && c != '\t') { word.push_back(c); continue; }
        if (!word.empty() && hay.find(word) == std::string::npos) return false;
        word.clear();
    }
    return true;
}

const std::vector<UiDevicePreset>& uiEditorDevicePresets() {
    static const std::vector<UiDevicePreset> presets = {
        {"Desktop 1080p", 1920, 1080, {0, 0, 0, 0}},
        {"Desktop 1440p", 2560, 1440, {0, 0, 0, 0}},
        {"Desktop 4K", 3840, 2160, {0, 0, 0, 0}},
        {"Ultrawide 21:9", 3440, 1440, {0, 0, 0, 0}},
        {"720p", 1280, 720, {0, 0, 0, 0}},
        {"Handheld 16:10", 1280, 800, {0, 0, 0, 0}},
        {"Console TV 1080p", 1920, 1080, {0.05f, 0.05f, 0.05f, 0.05f}},
        {"Console TV 4K", 3840, 2160, {0.035f, 0.035f, 0.035f, 0.035f}},
        {"Phone portrait", 1080, 2340, {0, 0.06f, 0, 0.03f}},
        {"Phone landscape", 2340, 1080, {0.06f, 0, 0.06f, 0.05f}},
        {"Tablet 4:3", 2048, 1536, {0, 0, 0, 0}},
    };
    return presets;
}

UiRect uiEditorSafeRect(const UiInsets& safe, f32 w, f32 h) {
    const f32 l = safe.left * w, t = safe.top * h, r = safe.right * w, b = safe.bottom * h;
    return {l, t, std::max(0.0f, w - l - r), std::max(0.0f, h - t - b)};
}

// ---- multi-select -------------------------------------------------------------------------------------------------

std::vector<i32> UiLayoutEditor::selection() const {
    std::vector<i32> out;
    if (inRange(doc_, selected_)) out.push_back(selected_);
    for (const std::string& n : extraSelected_) {
        const i32 i = uiLayoutFind(doc_, n);
        if (i >= 0 && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool UiLayoutEditor::isSelected(i32 index) const {
    const std::vector<i32> s = selection();
    return std::find(s.begin(), s.end(), index) != s.end();
}

void UiLayoutEditor::toggleSelect(i32 index) {
    if (!inRange(doc_, index)) return;
    const std::string& name = doc_.nodes[static_cast<usize>(index)].props.name;
    const auto inExtra = std::find(extraSelected_.begin(), extraSelected_.end(), name);
    if (index == selected_) {
        // Deselect the primary: the last other selected one takes over; the last remaining one stays.
        while (!extraSelected_.empty()) {
            const i32 next = uiLayoutFind(doc_, extraSelected_.back());
            extraSelected_.pop_back();
            if (next >= 0) { selected_ = next; return; }
        }
        return;
    }
    if (inExtra != extraSelected_.end()) { extraSelected_.erase(inExtra); return; }
    if (inRange(doc_, selected_)) extraSelected_.push_back(doc_.nodes[static_cast<usize>(selected_)].props.name);
    selected_ = index;
}

void UiLayoutEditor::selectRange(i32 first, i32 last) {
    if (!inRange(doc_, first) || !inRange(doc_, last)) return;
    if (first > last) std::swap(first, last);
    extraSelected_.clear();
    for (i32 i = first; i < last; ++i) extraSelected_.push_back(doc_.nodes[static_cast<usize>(i)].props.name);
    selected_ = last;
}

bool UiLayoutEditor::setPropertyOnSelection(std::string_view key, std::string_view text) {
    if (!loaded_) return false;
    const UiPropDesc* d = uiFindProperty(key);
    if (!d) return false;
    const std::vector<i32> sel = selection();
    pushUndo();
    bool changed = false;
    for (const i32 i : sel) {
        UiWidgetProps& p = doc_.nodes[static_cast<usize>(i)].props;
        if (!(d->kindMask & uiKindBit(p.kind)) || (d->rootOnly && i != 0)) continue;
        std::string before, after;
        uiGetProp(p, key, before);
        if (!uiSetProp(p, key, text)) continue;
        uiGetProp(p, key, after);
        changed = changed || before != after;
    }
    if (!changed) { history_.cancelPush(); return false; }
    dirty_ = true;
    rebuildPreview();
    return true;
}

void UiLayoutEditor::deleteSelection() {
    if (!loaded_) return;
    std::vector<std::string> names;
    for (const i32 i : selection())
        if (i != 0) names.push_back(doc_.nodes[static_cast<usize>(i)].props.name);
    if (names.empty()) return;
    pushUndo();
    for (const std::string& n : names) {
        const i32 i = uiLayoutFind(doc_, n);   // a deleted ancestor already took this one
        if (i > 0) uiLayoutDelete(doc_, i);
    }
    selected_ = 0;
    extraSelected_.clear();
    dirty_ = true;
    rebuildPreview();
}

void UiLayoutEditor::moveSelectionBy(f32 dx, f32 dy, bool begin) {
    if (!loaded_) return;
    const std::vector<i32> sel = selection();
    if (sel.empty() || (sel.size() == 1 && sel[0] == 0)) return;
    if (begin) pushUndo();
    for (const i32 i : sel)
        if (i != 0) uiEditorMoveBy(doc_.nodes[static_cast<usize>(i)].props, dx, dy);
    dirty_ = true;
    rebuildPreview();
}

// ---- device preview and widget states ---------------------------------------------------------------------------

void UiLayoutEditor::setDevicePreset(i32 index) {
    const std::vector<UiDevicePreset>& all = uiEditorDevicePresets();
    if (index < 0 || static_cast<usize>(index) >= all.size()) return;
    setPreviewSize(all[static_cast<usize>(index)].width, all[static_cast<usize>(index)].height);
    device_ = index;
}

UiRect UiLayoutEditor::safeZoneRect() const {
    const std::vector<UiDevicePreset>& all = uiEditorDevicePresets();
    UiInsets safe{0.05f, 0.05f, 0.05f, 0.05f};
    if (device_ >= 0 && static_cast<usize>(device_) < all.size()) {
        const UiInsets& s = all[static_cast<usize>(device_)].safe;
        if (s.left > 0 || s.top > 0 || s.right > 0 || s.bottom > 0) safe = s;
    }
    return uiEditorSafeRect(safe, previewW_, previewH_);
}

void UiLayoutEditor::applyPreviewState() {
    for (usize i = 0; i < previewIds_.size() && i < doc_.nodes.size(); ++i) {
        if (UiWidget* w = preview_.get(previewIds_[i])) {
            w->hovered = false;
            w->pressed = false;
            w->enabled = doc_.nodes[i].props.enabled;
        }
    }
    UiWidget* w = preview_.get(previewWidget(selected_));
    if (!w || previewState_ == 0) return;
    if (previewState_ == 1) w->hovered = true;
    else if (previewState_ == 2) { w->hovered = true; w->pressed = true; }
    else w->enabled = false;
}

// ---- animation timeline -------------------------------------------------------------------------------------------

std::vector<std::string> UiLayoutEditor::animationNames() const {
    std::vector<std::string> out;
    for (const UiLayoutAnim& a : doc_.anims)
        if (std::find(out.begin(), out.end(), a.name) == out.end()) out.push_back(a.name);
    return out;
}

std::vector<UiTimelineTrack> UiLayoutEditor::animationTracks(const std::string& anim) const {
    std::vector<UiTimelineTrack> out;
    for (const UiLayoutAnim& a : doc_.anims) {
        if (a.name != anim) continue;
        UiTimelineTrack t;
        t.widget = a.widget;
        UiAnimTrack parsed;
        if (uiAnimTrackFromRecord(a, parsed)) { t.prop = parsed.prop; t.keys = parsed.keys; }
        out.push_back(std::move(t));
    }
    return out;
}

f32 UiLayoutEditor::animationLength(const std::string& anim) const {
    f32 len = 0;
    for (const UiTimelineTrack& t : animationTracks(anim))
        if (!t.keys.empty()) len = std::max(len, t.keys.back().time);
    return len;
}

i32 UiLayoutEditor::animRecord(const std::string& anim, i32 track) const {
    i32 n = 0;
    for (usize i = 0; i < doc_.anims.size(); ++i) {
        if (doc_.anims[i].name != anim) continue;
        if (n++ == track) return static_cast<i32>(i);
    }
    return -1;
}

bool UiLayoutEditor::editAnimTrack(const std::string& anim, i32 track, const std::function<bool(UiAnimTrack&)>& edit) {
    const i32 rec = animRecord(anim, track);
    if (!loaded_ || rec < 0) return false;
    UiAnimTrack t;
    if (!uiAnimTrackFromRecord(doc_.anims[static_cast<usize>(rec)], t)) return false;
    pushUndo();
    if (!edit(t)) { history_.cancelPush(); return false; }
    doc_.anims[static_cast<usize>(rec)] = uiAnimTrackToRecord(anim, t);
    dirty_ = true;
    rebuildPreview();
    return true;
}

namespace {

UiAnimKey authoredKey(const UiWidgetProps& p, UiAnimProp prop, f32 time) {
    UiAnimKey k;
    k.time = time;
    const auto colour = [&](u32 c) {
        k.v[0] = static_cast<f32>(c & 0xFF); k.v[1] = static_cast<f32>((c >> 8) & 0xFF);
        k.v[2] = static_cast<f32>((c >> 16) & 0xFF); k.v[3] = static_cast<f32>(c >> 24);
    };
    switch (prop) {
        case UiAnimProp::Opacity: k.v[0] = p.opacity; break;
        case UiAnimProp::BgColor: colour(p.bgColor); break;
        case UiAnimProp::TextColor: colour(p.textColor); break;
        case UiAnimProp::AccentColor: colour(p.accentColor); break;
        case UiAnimProp::Tint: colour(p.tint); break;
        case UiAnimProp::Translate: k.v[0] = p.renderTranslate.x; k.v[1] = p.renderTranslate.y; break;
        case UiAnimProp::Rotation: k.v[0] = p.renderRotation; break;
        case UiAnimProp::Scale: k.v[0] = p.renderScale.x; k.v[1] = p.renderScale.y; break;
        case UiAnimProp::Count: break;
    }
    return k;
}

} // namespace

std::string UiLayoutEditor::addAnimation(const std::string& name) {
    if (!loaded_ || !inRange(doc_, selected_)) return {};
    const std::vector<std::string> existing = animationNames();
    std::string use = name.empty() ? "Anim" : name;
    if (!name.empty() && std::find(existing.begin(), existing.end(), use) != existing.end()) return {};
    for (int n = 2; std::find(existing.begin(), existing.end(), use) != existing.end(); ++n) use = (name.empty() ? "Anim" : name) + std::to_string(n);
    pushUndo();
    UiAnimTrack t;
    t.widget = doc_.nodes[static_cast<usize>(selected_)].props.name;
    t.prop = UiAnimProp::Opacity;
    t.keys.push_back(authoredKey(doc_.nodes[static_cast<usize>(selected_)].props, t.prop, 0.0f));
    t.keys.push_back(authoredKey(doc_.nodes[static_cast<usize>(selected_)].props, t.prop, 1.0f));
    t.keys[0].v[0] = 0.0f;
    doc_.anims.push_back(uiAnimTrackToRecord(use, t));
    dirty_ = true;
    rebuildPreview();
    setTimelineAnimation(use);
    return use;
}

bool UiLayoutEditor::deleteAnimation(const std::string& anim) {
    if (!loaded_) return false;
    const auto before = doc_.anims.size();
    pushUndo();
    doc_.anims.erase(std::remove_if(doc_.anims.begin(), doc_.anims.end(), [&](const UiLayoutAnim& a) { return a.name == anim; }), doc_.anims.end());
    if (doc_.anims.size() == before) { history_.cancelPush(); return false; }
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::renameAnimation(const std::string& anim, const std::string& newName) {
    if (!loaded_ || newName.empty() || newName == anim) return false;
    const std::vector<std::string> existing = animationNames();
    if (std::find(existing.begin(), existing.end(), anim) == existing.end() || std::find(existing.begin(), existing.end(), newName) != existing.end()) return false;
    pushUndo();
    for (UiLayoutAnim& a : doc_.anims) if (a.name == anim) a.name = newName;
    if (animName_ == anim) animName_ = newName;
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::addTrack(const std::string& anim, i32 widgetIndex, UiAnimProp prop) {
    if (!loaded_ || !inRange(doc_, widgetIndex) || anim.empty()) return false;
    const std::string& widget = doc_.nodes[static_cast<usize>(widgetIndex)].props.name;
    for (const UiLayoutAnim& a : doc_.anims) {
        UiAnimTrack t;
        if (a.name == anim && a.widget == widget && uiAnimTrackFromRecord(a, t) && t.prop == prop) return false;
    }
    const f32 end = std::max(1.0f, animationLength(anim));
    pushUndo();
    UiAnimTrack t;
    t.widget = widget;
    t.prop = prop;
    t.keys.push_back(authoredKey(doc_.nodes[static_cast<usize>(widgetIndex)].props, prop, 0.0f));
    t.keys.push_back(authoredKey(doc_.nodes[static_cast<usize>(widgetIndex)].props, prop, end));
    doc_.anims.push_back(uiAnimTrackToRecord(anim, t));
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::deleteTrack(const std::string& anim, i32 track) {
    const i32 rec = animRecord(anim, track);
    if (!loaded_ || rec < 0) return false;
    pushUndo();
    doc_.anims.erase(doc_.anims.begin() + rec);
    dirty_ = true;
    rebuildPreview();
    return true;
}

i32 UiLayoutEditor::addKey(const std::string& anim, i32 track, f32 time) {
    i32 at = -1;
    const bool ok = editAnimTrack(anim, track, [&](UiAnimTrack& t) {
        time = std::max(0.0f, time);
        for (const UiAnimKey& k : t.keys) if (std::fabs(k.time - time) < 1e-4f) return false;
        UiAnimValue v;
        if (!uiAnimSample(t, time, v)) return false;
        UiAnimKey nk;
        nk.time = time;
        for (int i = 0; i < 4; ++i) nk.v[i] = v.v[i];
        usize pos = 0;
        while (pos < t.keys.size() && t.keys[pos].time < time) ++pos;
        t.keys.insert(t.keys.begin() + static_cast<std::ptrdiff_t>(pos), nk);
        at = static_cast<i32>(pos);
        return true;
    });
    return ok ? at : -1;
}

i32 UiLayoutEditor::moveKey(const std::string& anim, i32 track, i32 key, f32 time) {
    i32 at = -1;
    const bool ok = editAnimTrack(anim, track, [&](UiAnimTrack& t) {
        if (key < 0 || static_cast<usize>(key) >= t.keys.size()) return false;
        time = std::max(0.0f, time);
        for (usize i = 0; i < t.keys.size(); ++i)
            if (static_cast<i32>(i) != key && std::fabs(t.keys[i].time - time) < 1e-4f) return false;
        UiAnimKey k = t.keys[static_cast<usize>(key)];
        t.keys.erase(t.keys.begin() + key);
        k.time = time;
        usize pos = 0;
        while (pos < t.keys.size() && t.keys[pos].time < time) ++pos;
        t.keys.insert(t.keys.begin() + static_cast<std::ptrdiff_t>(pos), k);
        at = static_cast<i32>(pos);
        return true;
    });
    return ok ? at : -1;
}

bool UiLayoutEditor::setKeyValue(const std::string& anim, i32 track, i32 key, const std::string& token) {
    return editAnimTrack(anim, track, [&](UiAnimTrack& t) {
        if (key < 0 || static_cast<usize>(key) >= t.keys.size()) return false;
        UiAnimKey k = t.keys[static_cast<usize>(key)];
        const UiEase keep = k.ease;
        if (!uiAnimKeyFromToken(t.prop, token, k)) return false;
        if (token.find('@') == std::string::npos) k.ease = keep;
        t.keys[static_cast<usize>(key)] = k;
        return true;
    });
}

bool UiLayoutEditor::setKeyEase(const std::string& anim, i32 track, i32 key, UiEase ease) {
    return editAnimTrack(anim, track, [&](UiAnimTrack& t) {
        if (key < 0 || static_cast<usize>(key) >= t.keys.size() || t.keys[static_cast<usize>(key)].ease == ease) return false;
        t.keys[static_cast<usize>(key)].ease = ease;
        return true;
    });
}

bool UiLayoutEditor::deleteKey(const std::string& anim, i32 track, i32 key) {
    return editAnimTrack(anim, track, [&](UiAnimTrack& t) {
        if (key < 0 || static_cast<usize>(key) >= t.keys.size() || t.keys.size() < 2) return false;   // a track keeps one key
        t.keys.erase(t.keys.begin() + key);
        return true;
    });
}

void UiLayoutEditor::setTimelineAnimation(const std::string& anim) {
    animName_ = anim;
    animTime_ = 0;
    animPlaying_ = false;
    applyTimeline();
}

void UiLayoutEditor::applyTimeline() {
    if (animName_.empty()) return;
    const UiWidgetId root = previewWidget(0);
    if (!preview_.findAnimation(root, animName_)) { animName_.clear(); animPlaying_ = false; animTime_ = 0; return; }
    if (animPlaying_) {
        UiAnimPlay p;
        p.loops = animLoop_ ? 0 : 1;
        preview_.animPlay(root, animName_, p);
    }
    preview_.animSeek(root, animName_, animTime_);
}

void UiLayoutEditor::timelineSeek(f32 seconds) {
    if (animName_.empty()) return;
    const UiWidgetId root = previewWidget(0);
    animPlaying_ = false;
    animTime_ = std::clamp(seconds, 0.0f, animationLength(animName_));
    preview_.animPause(root, animName_, true);
    preview_.animSeek(root, animName_, animTime_);
}

void UiLayoutEditor::timelinePlay(bool play) {
    if (animName_.empty()) return;
    const UiWidgetId root = previewWidget(0);
    if (!play) {
        preview_.animPause(root, animName_, true);
        animPlaying_ = false;
        return;
    }
    if (animTime_ >= animationLength(animName_) - 1e-4f) animTime_ = 0;
    UiAnimPlay p;
    p.loops = animLoop_ ? 0 : 1;
    preview_.animPlay(root, animName_, p);
    preview_.animSeek(root, animName_, animTime_);
    animPlaying_ = true;
}

void UiLayoutEditor::timelineTick(f32 dt) {
    if (!animPlaying_ || animName_.empty()) return;
    const UiWidgetId root = previewWidget(0);
    preview_.animAdvance(dt);
    const f32 t = preview_.animTime(root, animName_);
    if (t < 0.0f) { animPlaying_ = false; animTime_ = animationLength(animName_); }   // finished; the end values stay
    else animTime_ = t;
}

// ---- bindings and events ----------------------------------------------------------------------------------------------

namespace {

std::vector<std::string> bindRest(const std::string& format, bool onChange) {
    std::vector<std::string> rest;
    if (!format.empty()) { rest.push_back("format"); rest.push_back(format); }
    if (onChange) { rest.push_back("mode"); rest.push_back("change"); }
    return rest;
}

} // namespace

i32 UiLayoutEditor::addBinding(i32 widgetIndex, const std::string& property, const std::string& path, const std::string& format, bool onChange) {
    UiBindSource src;
    std::string p;
    if (!loaded_ || !inRange(doc_, widgetIndex) || !uiFindProperty(property) || !uiParseBindPath(path, src, p)) return -1;
    pushUndo();
    doc_.binds.push_back({doc_.nodes[static_cast<usize>(widgetIndex)].props.name, uiFindProperty(property)->key, uiBindPathText(src, p), bindRest(format, onChange)});
    dirty_ = true;
    rebuildPreview();
    return static_cast<i32>(doc_.binds.size()) - 1;
}

bool UiLayoutEditor::setBinding(i32 binding, const std::string& property, const std::string& path, const std::string& format, bool onChange) {
    UiBindSource src;
    std::string p;
    if (!loaded_ || binding < 0 || static_cast<usize>(binding) >= doc_.binds.size() || !uiFindProperty(property) || !uiParseBindPath(path, src, p)) return false;
    pushUndo();
    UiLayoutBind& b = doc_.binds[static_cast<usize>(binding)];
    b.property = uiFindProperty(property)->key;
    b.path = uiBindPathText(src, p);
    b.rest = bindRest(format, onChange);
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::deleteBinding(i32 binding) {
    if (!loaded_ || binding < 0 || static_cast<usize>(binding) >= doc_.binds.size()) return false;
    pushUndo();
    doc_.binds.erase(doc_.binds.begin() + binding);
    dirty_ = true;
    rebuildPreview();
    return true;
}

i32 UiLayoutEditor::addEvent(i32 widgetIndex, const std::string& event, const std::string& action, const std::string& target, const std::vector<std::string>& args) {
    u8 type;
    bool enter;
    UiRouteAction a;
    if (!loaded_ || !inRange(doc_, widgetIndex) || target.empty() || !uiRouteEventFromName(event, type, enter) || !uiRouteActionFromName(action, a)) return -1;
    pushUndo();
    doc_.events.push_back({doc_.nodes[static_cast<usize>(widgetIndex)].props.name, uiRouteEventName(type, enter), uiRouteActionName(a), target, args});
    dirty_ = true;
    rebuildPreview();
    return static_cast<i32>(doc_.events.size()) - 1;
}

bool UiLayoutEditor::setEvent(i32 index, const std::string& event, const std::string& action, const std::string& target, const std::vector<std::string>& args) {
    u8 type;
    bool enter;
    UiRouteAction a;
    if (!loaded_ || index < 0 || static_cast<usize>(index) >= doc_.events.size() || target.empty() || !uiRouteEventFromName(event, type, enter) ||
        !uiRouteActionFromName(action, a))
        return false;
    pushUndo();
    UiLayoutEvent& e = doc_.events[static_cast<usize>(index)];
    e.event = uiRouteEventName(type, enter);
    e.action = uiRouteActionName(a);
    e.target = target;
    e.rest = args;
    dirty_ = true;
    rebuildPreview();
    return true;
}

bool UiLayoutEditor::deleteEvent(i32 index) {
    if (!loaded_ || index < 0 || static_cast<usize>(index) >= doc_.events.size()) return false;
    pushUndo();
    doc_.events.erase(doc_.events.begin() + index);
    dirty_ = true;
    rebuildPreview();
    return true;
}

#if AVER_WITH_IMGUI

// =============================================================================== drawing ==========

namespace {

constexpr const char* kPrefLeftSplit = "uiLayoutEditor.leftSplit";
constexpr const char* kPrefInspectorSplit = "uiLayoutEditor.inspectorSplit";
constexpr f32 kDefaultLeftFraction = 0.22f;
constexpr f32 kDefaultCanvasFraction = 0.70f;

// The preview's text metrics. With a usable preview engine (the project's font set) they are the game's: the same
// layouts the packaged game draws. Otherwise ImGui's font. Sizes here are PREVIEW pixels; the painter scales to screen.
class PreviewMetrics : public UiTextMetrics {
public:
    explicit PreviewMetrics(UiTextEngine* engine = nullptr) : engine_(engine) {}
    f32 textWidth(std::string_view t, f32 px) const override {
        if (t.empty()) return 0.0f;
        if (engine_) return engine_->textWidth(t, px);
        return ImGui::GetFont()->CalcTextSizeA(px, 1.0e9f, 0.0f, t.data(), t.data() + t.size()).x;
    }
    f32 lineHeight(f32 px) const override { return engine_ ? engine_->lineHeight(px) : px * 1.25f; }
    f32 ascent(f32 px) const override { return engine_ ? engine_->ascent(px) : px * 0.95f; }
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle& style,
                                                   const UiTextBox& box) const override {
        return engine_ ? engine_->layout(text, style, box) : UiTextMetrics::layoutText(text, style, box);
    }

private:
    UiTextEngine* engine_;
};

// Set by draw(): the preview needs the device to upload glyph pages.
Engine* g_drawEngine = nullptr;

// Paints a tree into an ImDrawList at a zoom and origin. The draw list's colour packing is the UI's.
class PreviewPainter final : public UiPainter {
public:
    PreviewPainter(ImDrawList* dl, ImVec2 origin, f32 zoom, EditorTextPreview* preview = nullptr)
        : dl_(dl), origin_(origin), zoom_(zoom), preview_(preview), metrics_(preview ? &preview->engine() : nullptr) {}

    // A widget's render transform. Rects, images and text turn with it.
    void pushTransform(const UiAffine& a) override { xf_.push_back(xf_.empty() ? a : xf_.back() * a); }
    void popTransform() override { if (!xf_.empty()) xf_.pop_back(); }
    void pushClip(const UiRect& r) override {
        const ImVec2 a = at(r.x, r.y), b = at(r.right(), r.bottom());
        if (xf_.empty()) { dl_->PushClipRect(a, b, true); return; }
        const ImVec2 c = at(r.right(), r.y), d = at(r.x, r.bottom());
        dl_->PushClipRect(ImVec2(std::min(std::min(a.x, b.x), std::min(c.x, d.x)), std::min(std::min(a.y, b.y), std::min(c.y, d.y))),
                          ImVec2(std::max(std::max(a.x, b.x), std::max(c.x, d.x)), std::max(std::max(a.y, b.y), std::max(c.y, d.y))), true);
    }
    void popClip() override { dl_->PopClipRect(); }
    void fillRect(const UiRect& r, u32 rgba) override {
        if (r.empty()) return;
        quad(r, rgba);
    }
    void image(const UiRect& r, u64 texture, const UiUvRect&, u32 tint) override {
        (void)texture;
        if (r.empty()) return;
        quad(r, uiWithOpacity(tint, 0.6f));
    }
    void text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) override {
        if (s.empty()) return;
        // The tree's baseline is lineTop + ascent; ImGui wants the top of the glyph box.
        const f32 top = baselineY - ascent(px) + px * 0.12f;
        const ImVec2 p0 = at(x, top);
        const i32 first = dl_->VtxBuffer.Size;
        dl_->AddText(ImGui::GetFont(), px * zoom_ * textScale(), p0, rgba, s.data(), s.data() + s.size());
        if (xf_.empty()) return;
        // Turn the glyph quads about the text's origin by the transform's rotation (cheap: only the new vertices move).
        const UiAffine& a = xf_.back();
        const f32 len = std::sqrt(a.a * a.a + a.b * a.b);
        if (len < 1.0e-6f || std::fabs(a.b) < 1.0e-6f * len) return;
        const f32 cs = a.a / len, sn = a.b / len;
        for (i32 i = first; i < dl_->VtxBuffer.Size; ++i) {
            ImVec2& v = dl_->VtxBuffer[i].pos;
            const f32 dx = v.x - p0.x, dy = v.y - p0.y;
            v = ImVec2(p0.x + dx * cs - dy * sn, p0.y + dx * sn + dy * cs);
        }
    }
    // A laid-out block: the engine's glyph quads, painted through the editor's text preview. A legacy layout (no
    // engine) takes the base path through text().
    void drawText(const UiTextLayout& layout, const UiRect& area, UiTextAlign align, bool centreVertically, u32 rgba) override {
        if (!preview_ || layout.legacy) {
            UiPainter::drawText(layout, area, align, centreVertically, rgba);
            return;
        }
        UiDrawList list;
        UiDrawListPainter inner(list, &preview_->engine(), nullptr);
        if (!xf_.empty()) list.pushTransform(xf_.back());
        inner.drawText(layout, area, align, centreVertically, rgba);
        preview_->paint(dl_, list, origin_.x, origin_.y, zoom_);
    }
    f32 textWidth(std::string_view t, f32 px) const override { return metrics_.textWidth(t, px); }
    f32 lineHeight(f32 px) const override { return metrics_.lineHeight(px); }
    f32 ascent(f32 px) const override { return metrics_.ascent(px); }
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle& style,
                                                   const UiTextBox& box) const override {
        return metrics_.layoutText(text, style, box);
    }

private:
    ImVec2 at(f32 x, f32 y) const {
        if (!xf_.empty()) { f32 ox = 0, oy = 0; xf_.back().apply(x, y, ox, oy); x = ox; y = oy; }
        return ImVec2(origin_.x + x * zoom_, origin_.y + y * zoom_);
    }
    f32 textScale() const {
        if (xf_.empty()) return 1.0f;
        const UiAffine& a = xf_.back();
        return std::sqrt(std::max(0.0f, std::fabs(a.a * a.d - a.b * a.c)));
    }
    void quad(const UiRect& r, u32 rgba) {
        if (xf_.empty()) { dl_->AddRectFilled(at(r.x, r.y), at(r.right(), r.bottom()), rgba); return; }
        dl_->AddQuadFilled(at(r.x, r.y), at(r.right(), r.y), at(r.right(), r.bottom()), at(r.x, r.bottom()), rgba);
    }
    std::vector<UiAffine> xf_;
    ImDrawList* dl_;
    ImVec2 origin_;
    f32 zoom_;
    EditorTextPreview* preview_;
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

bool UiLayoutEditor::drawProperty(const UiPropDesc& d, UiWidgetProps* target,
                                  const std::function<void(const UiWidgetProps&)>& applied, const char* label) {
    UiWidgetProps& p = target ? *target : doc_.nodes[static_cast<usize>(selected_)].props;
    std::vector<std::string> tokens;
    d.get(p, tokens);

    bool changed = false;
    std::string newText;
    bool immediate = false;    // a click rather than a drag: the edit begins and ends this frame
    const auto fl = [](const std::string& s) { f32 v = 0; uiParseFloat(s, v); return v; };

    ImGui::PushID(label ? label : d.key);
    ImGui::TextUnformatted(label ? label : d.key);
    if (!target) {
        // The bindings column: a bound property says so, and any property can be bound from here.
        const std::string& wname = doc_.nodes[static_cast<usize>(selected_)].props.name;
        i32 bound = -1;
        for (usize b = 0; b < doc_.binds.size(); ++b)
            if (doc_.binds[b].widget == wname && uiFindProperty(doc_.binds[b].property) == &d) bound = static_cast<i32>(b);
        ImGui::SameLine();
        if (bound >= 0) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[%s]", doc_.binds[static_cast<usize>(bound)].path.c_str());
            if (ImGui::IsItemClicked()) bottomTab_ = 1, bottomOpen_ = true;
        } else if (ImGui::SmallButton("bind")) {
            if (addBinding(selected_, d.key, std::string("vm:") + d.key) >= 0) { bottomTab_ = 1; bottomOpen_ = true; }
        }
    }
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
            if (std::strcmp(d.key, "fontFamily") == 0) {
                // A family of the project's font set (Content/UI/Fonts.ocfonts); empty keeps the theme's.
                const std::vector<std::string> families = EditorTextPreview::shared().familyNames();
                if (!families.empty()) {
                    const std::string current = tokens.empty() ? std::string() : tokens[0];
                    if (ImGui::BeginCombo("##v", current.empty() ? "(theme default)" : current.c_str())) {
                        if (ImGui::Selectable("(theme default)", current.empty())) { changed = true; newText = uiQuote(""); }
                        for (const std::string& f : families)
                            if (ImGui::Selectable(f.c_str(), f == current)) { changed = true; newText = uiQuote(f); }
                        ImGui::EndCombo();
                    }
                    immediate = true;
                    break;
                }
            }
            // Sized from the value with room to type: a fixed buffer cut a longer text on the first keystroke, and every
            // keystroke commits.
            const std::string cur = tokens.empty() ? std::string() : tokens[0];
            std::vector<char> buf(std::max<usize>(cur.size() + 1024, 2048), '\0');
            std::memcpy(buf.data(), cur.data(), cur.size());
            if (std::strcmp(d.key, "text") == 0)
                changed = ImGui::InputTextMultiline("##v", buf.data(), buf.size(), ImVec2(-1, ImGui::GetTextLineHeight() * 3.5f));
            else
                changed = ImGui::InputText("##v", buf.data(), buf.size());
            if (changed) newText = uiQuote(buf.data());
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
            std::vector<char> buf(std::max<usize>(joined.size() + 1024, 2048), '\0');   // as String above
            std::memcpy(buf.data(), joined.data(), joined.size());
            changed = ImGui::InputTextMultiline("##v", buf.data(), buf.size(), ImVec2(-1, ImGui::GetTextLineHeight() * 4.5f));
            if (changed) {
                std::string line;
                for (const char* c = buf.data();; ++c) {
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
            if (applied) applied(p);
            if (!target) {
                // A multi-selection: the same value goes to every other selected widget this property applies to.
                for (const i32 other : selection()) {
                    if (other == selected_) continue;
                    UiWidgetProps& q = doc_.nodes[static_cast<usize>(other)].props;
                    if ((d.kindMask & uiKindBit(q.kind)) && !(d.rootOnly && other != 0)) uiSetProp(q, d.key, newText);
                }
            }
            rebuildPreview();
        }
    }
    if (immediate && changed) propEditEnd();
    if (ImGui::IsItemDeactivated()) propEditEnd();
    if (!target && (std::strcmp(d.key, "text") == 0 || std::strcmp(d.key, "placeholder") == 0 || std::strcmp(d.key, "items") == 0))
        drawLocalizeRow(d);
    ImGui::PopID();
    return changed;
}

// docs/LOCALIZATION.md: a text field may hold "@Table:Key=Default". Localize turns the literal into one (table UI,
// key <Layout>.<Widget>.<property>), keeping the literal as the inline default.
void UiLayoutEditor::drawLocalizeRow(const UiPropDesc& d) {
    UiWidgetProps& p = doc_.nodes[static_cast<usize>(selected_)].props;
    std::vector<std::string> tokens;
    d.get(p, tokens);
    const bool isItems = d.type == UiPropType::StringList;
    UiTextRef ref;
    bool anyRef = false, anyLiteral = false;
    std::string shown;
    for (const std::string& t : tokens) {
        const bool isRef = uiParseTextRef(t, ref);
        anyRef = anyRef || isRef;
        anyLiteral = anyLiteral || (!isRef && !t.empty());
        if (!shown.empty()) shown += ", ";
        shown += preview_.shown(t);
    }
    if (anyRef) ImGui::TextDisabled("Shown: %s", shown.c_str());
    if (!anyLiteral) return;

    // Keys use [A-Za-z0-9_.-]; anything else in a layout or widget name becomes '_'.
    const auto clean = [](std::string s) {
        for (char& c : s) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') c = '_';
        return s;
    };
    const std::string base = "@UI:" + clean(doc_.name.empty() ? std::string("Layout") : doc_.name) + "." + clean(p.name) + "." + d.key;
    if (!ImGui::SmallButton(isItems ? "Localize items" : "Localize")) return;
    std::string newText;
    for (usize i = 0; i < tokens.size(); ++i) {
        const bool keep = tokens[i].empty() || uiParseTextRef(tokens[i], ref);
        const std::string value = keep ? tokens[i]
                                       : base + (isItems ? "." + std::to_string(i) : std::string()) + "=" + tokens[i];
        newText += (newText.empty() ? "" : " ") + uiQuote(value);
    }
    propEditBegin();
    if (uiSetProp(p, d.key, newText)) {
        propEdited_ = true;
        dirty_ = true;
        rebuildPreview();
    }
    propEditEnd();
}

void UiLayoutEditor::drawInspector() {
    if (!inRange(doc_, selected_)) { ImGui::TextUnformatted("Nothing selected"); return; }
    // Re-fetched after every call that can renumber the nodes: a reference would dangle.
    const auto cur = [this]() -> UiWidgetProps& { return doc_.nodes[static_cast<usize>(selected_)].props; };
    if (selection().size() > 1) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%d widgets selected: edits apply to all", static_cast<int>(selection().size()));
        if (ImGui::SmallButton("Select only this")) select(selected_);
    }

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
        drawThemePicker();
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

    const bool isUse = doc_.nodes[static_cast<usize>(selected_)].use.has_value();
    if (isUse) drawUseInspector();

    const char* const groups[] = {"Placement", "Layout", "Appearance", "Content", "Value", "Transform", "States", "Navigation", "Accessibility", "Root"};
    for (const char* g : groups) {
        // A used layout brings its own content: the instance only places it.
        if (isUse && std::strcmp(g, "Placement") != 0 && std::strcmp(g, "Appearance") != 0) continue;
        std::vector<const UiPropDesc*> shown;
        for (const UiPropDesc& d : uiProperties()) {
            if (std::strcmp(d.group, g) != 0) continue;
            if (!(d.kindMask & uiKindBit(cur().kind))) continue;
            if (d.rootOnly && selected_ != 0) continue;
            shown.push_back(&d);
        }
        if (shown.empty()) continue;
        const bool open = std::strcmp(g, "Placement") == 0 || std::strcmp(g, "Content") == 0 || std::strcmp(g, "Value") == 0;
        if (::aver::editor::sectionHeader(g, open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (std::strcmp(g, "States") == 0) drawStates();
            for (const UiPropDesc* d : shown) drawProperty(*d);
        }
    }
}

// A USE node: the layout it instantiates and the properties that layout exposes.
void UiLayoutEditor::drawUseInspector() {
    const i32 index = selected_;
    const std::string usePath = doc_.nodes[static_cast<usize>(index)].use->path;
    ImGui::Separator();
    ImGui::TextUnformatted("Layout");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##usepath", usePath.c_str())) {
        for (const std::string& l : projectLayouts())
            if (ImGui::Selectable(l.c_str(), l == usePath)) setUsePath(index, l);
        ImGui::EndCombo();
    }
    if (!status_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", status_.c_str());
    for (const std::string& e : previewErrors_)
        if (e.find(usePath) != std::string::npos) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", e.c_str());

    std::string err;
    const std::vector<UiExposedProp> exposed = exposedOf(index, &err);
    if (exposed.empty()) {
        ImGui::TextDisabled("%s", err.empty() ? "This layout exposes no properties (EXPOSE records)." : err.c_str());
    } else {
        ImGui::Separator();
        ImGui::TextUnformatted("Exposed properties");
        for (const UiExposedProp& ep : exposed) {
            const UiPropDesc* d = uiFindProperty(ep.key);
            if (!d) continue;
            UiWidgetProps scratch = uiDefaultProps(UiWidgetKind::Panel);
            d->set(scratch, ep.value);
            const std::string name = ep.name;
            drawProperty(*d, &scratch, [this, index, name, d](const UiWidgetProps& p) {
                std::vector<std::string> tokens;
                d->get(p, tokens);
                uiLayoutSetOverride(doc_, index, name, std::move(tokens));
            }, ep.name.c_str());
            if (ep.overridden) {
                ImGui::PushID(("reset" + name).c_str());
                if (ImGui::SmallButton("reset to the layout's value")) clearOverride(index, name);
                ImGui::PopID();
            }
        }
    }
    ImGui::TextDisabled("Children of this node fill the layout's slots (set their 'slot').");
    ImGui::Separator();
}

void UiLayoutEditor::drawHierarchyRow(i32 index) {
    const std::vector<i32> children = uiLayoutChildren(doc_, index);
    const UiWidgetProps& p = doc_.nodes[static_cast<usize>(index)].props;
    const u32 version = structureVersion_;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isSelected(index)) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(index);
    const UiLayoutNode& node = doc_.nodes[static_cast<usize>(index)];
    const bool open = node.use ? ImGui::TreeNodeEx("##row", flags, "%s  (layout: %s)", p.name.c_str(), node.use->path.c_str())
                               : ImGui::TreeNodeEx("##row", flags, "%s  (%s)", p.name.c_str(), uiKindName(p.kind));
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        const ImGuiIO& rio = ImGui::GetIO();
        if (rio.KeyCtrl) toggleSelect(index);
        else if (rio.KeyShift) selectRange(selected_, index);
        else select(index);
    }
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
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_PRESET")) {
            select(index);
            addPreset(static_cast<usize>(*static_cast<const i32*>(pl->Data)));
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_USE")) {
            select(index);
            addUse(std::string(static_cast<const char*>(pl->Data)));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##rowmenu")) {
        if (!isSelected(index)) select(index);
        if (ImGui::MenuItem("Duplicate", nullptr, false, index != 0 && selection().size() == 1)) duplicateSelected();
        if (ImGui::MenuItem("Delete", nullptr, false, index != 0)) deleteSelection();
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
    char query[128];
    std::snprintf(query, sizeof query, "%s", paletteQuery_.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##palsearch", "Search the palette", query, sizeof query)) paletteQuery_ = query;
    const bool searching = !paletteQuery_.empty();
    ImGui::TextDisabled("Drag onto the canvas or the hierarchy");
    if (searching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Widgets", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k) {
            const UiWidgetKind kind = static_cast<UiWidgetKind>(k);
            if (!uiEditorPaletteMatch(kKindLabels[k], paletteQuery_)) continue;
            ImGui::PushID(static_cast<int>(k));
            ImGui::Selectable(kKindLabels[k]);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) addWidget(kind);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("AVER_UI_KIND", &kind, sizeof kind);
                ImGui::Text("New %s", kKindLabels[k]);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }
    if (searching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Panels", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::vector<UiWidgetPreset>& presets = uiWidgetPresets();
        for (usize k = 0; k < presets.size(); ++k) {
            if (!uiEditorPaletteMatch(presets[k].label, paletteQuery_)) continue;
            const i32 index = static_cast<i32>(k);
            ImGui::PushID(1000 + index);
            ImGui::Selectable(presets[k].label);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) addPreset(k);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("AVER_UI_PRESET", &index, sizeof index);
                ImGui::Text("New %s", presets[k].label);
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }
    if (searching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Layouts", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The project's other .ocui files, rescanned when the palette is not busy; a few hundred paths at most.
        if (layoutsScanFrame_ == 0 || ImGui::GetFrameCount() - layoutsScanFrame_ > 120) {
            layouts_ = projectLayouts();
            layoutsScanFrame_ = ImGui::GetFrameCount();
        }
        if (layouts_.empty()) ImGui::TextDisabled("No other .ocui files in the project.");
        for (usize k = 0; k < layouts_.size(); ++k) {
            if (!uiEditorPaletteMatch(layouts_[k], paletteQuery_)) continue;
            ImGui::PushID(2000 + static_cast<int>(k));
            ImGui::Selectable(layouts_[k].c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) addUse(layouts_[k]);
            if (ImGui::BeginDragDropSource()) {
                char buf[512];
                std::snprintf(buf, sizeof buf, "%s", layouts_[k].c_str());
                ImGui::SetDragDropPayload("AVER_UI_USE", buf, std::strlen(buf) + 1);
                ImGui::Text("Use %s", layouts_[k].c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
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
    drawThemePicker();
    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", "Add child")) {
        for (u32 k = 0; k < static_cast<u32>(UiWidgetKind::Count); ++k)
            if (ImGui::Selectable(kKindLabels[k])) addWidget(static_cast<UiWidgetKind>(k));
        ImGui::Separator();
        const std::vector<UiWidgetPreset>& presets = uiWidgetPresets();
        for (usize k = 0; k < presets.size(); ++k)
            if (ImGui::Selectable(presets[k].label)) addPreset(k);
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
    char sizeLabel[64];
    const std::vector<UiDevicePreset>& devices = uiEditorDevicePresets();
    if (device_ >= 0 && static_cast<usize>(device_) < devices.size())
        std::snprintf(sizeLabel, sizeof sizeLabel, "%s", devices[static_cast<usize>(device_)].name);
    else
        std::snprintf(sizeLabel, sizeof sizeLabel, "Custom %.0fx%.0f", previewW_, previewH_);
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##size", sizeLabel)) {
        for (usize i = 0; i < devices.size(); ++i) {
            char l[96];
            std::snprintf(l, sizeof l, "%s  %.0fx%.0f", devices[i].name, devices[i].width, devices[i].height);
            if (ImGui::Selectable(l, static_cast<i32>(i) == device_)) setDevicePreset(static_cast<i32>(i));
        }
        ImGui::Separator();
        f32 custom[2] = {previewW_, previewH_};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputFloat2("Custom", custom, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue)) setPreviewSize(custom[0], custom[1]);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Safe zone", &safeZone_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shades the margin a TV or phone may crop (the device preset's, else 5%).");
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
    if (g_loc.cultures && g_loc.culture) {
        const std::vector<std::string> cultures = g_loc.cultures();
        if (cultures.size() > 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            const std::string active = g_loc.culture();
            if (ImGui::BeginCombo("##culture", active.c_str())) {
                for (const std::string& c : cultures)
                    if (ImGui::Selectable(c.c_str(), c == active) && g_loc.setCulture) g_loc.setCulture(c);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview culture: shows translated text for references like @UI:Menu.Play=Play.");
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Animation / bindings / events", &bottomOpen_);
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

    // The project's font set drives the preview's text when it has a TrueType face; else ImGui's font as before.
    EditorTextPreview& textPreview = EditorTextPreview::shared();
    textPreview.ensureLoaded(fontContentRootOf(path_));
    if (g_loc.culture) textPreview.engine().setLanguage(g_loc.culture());
    textPreview.beginFrame(static_cast<u64>(ImGui::GetFrameCount()));
    if (g_drawEngine) textPreview.syncGpu(*g_drawEngine);
    const bool engineText = textPreview.usable();
    PreviewMetrics metrics(engineText ? &textPreview.engine() : nullptr);
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
        timelineTick(io.DeltaTime);
        applyPreviewState();
    }

    PreviewPainter painter(dl, origin, zoom, engineText ? &textPreview : nullptr);
    preview_.draw(painter);
    dl->AddRect(toScreen(0, 0), toScreen(previewW_, previewH_), IM_COL32(120, 126, 140, 255));

    if (safeZone_) {
        // Dim the margin outside the safe rectangle and outline the rectangle.
        const UiRect z = safeZoneRect();
        const ImU32 dim = IM_COL32(0, 0, 0, 90);
        dl->AddRectFilled(toScreen(0, 0), toScreen(previewW_, z.y), dim);
        dl->AddRectFilled(toScreen(0, z.bottom()), toScreen(previewW_, previewH_), dim);
        dl->AddRectFilled(toScreen(0, z.y), toScreen(z.x, z.bottom()), dim);
        dl->AddRectFilled(toScreen(z.right(), z.y), toScreen(previewW_, z.bottom()), dim);
        dl->AddRect(toScreen(z.x, z.y), toScreen(z.right(), z.bottom()), IM_COL32(255, 210, 80, 220), 0.0f, 0, 1.5f);
    }

    if (!interact_ && loaded_) {
        // Hover and selection outlines, and the handles on the selection.
        const ImU32 accent = IM_COL32(86, 156, 255, 255);
        const UiWidgetId hoverId = hovered ? uiEditorPick(preview_, mouse.x, mouse.y) : 0;
        if (hoverId && nodeOf(hoverId) != selected_) {
            const UiWidget* h = preview_.get(hoverId);
            dl->AddRect(toScreen(h->rect.x, h->rect.y), toScreen(h->rect.right(), h->rect.bottom()), IM_COL32(86, 156, 255, 110));
        }
        for (const i32 other : selection()) {
            if (other == selected_) continue;
            const UiRect o = previewRect(other);
            if (!o.empty()) dl->AddRect(toScreen(o.x, o.y), toScreen(o.right(), o.bottom()), IM_COL32(86, 156, 255, 190), 0.0f, 0, 1.5f);
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
                if (picked >= 0) {
                    if (io.KeyCtrl) toggleSelect(picked);
                    else if (!isSelected(picked)) select(picked);   // a click inside the selection keeps it, so a drag moves all of it
                }
                canvasDrag_ = (picked >= 0 && picked != 0) ? 16 : 0;
            }
        }
        if (active && canvasDrag_ != 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            const f32 dx = io.MouseDelta.x / zoom, dy = io.MouseDelta.y / zoom;   // preview px
            const bool first = !canvasDragStarted_;
            canvasDragStarted_ = true;
            if ((canvasDrag_ & 16) && selection().size() > 1) {
                const f32 sc = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
                moveSelectionBy(dx / sc, dy / sc, first);
            } else if (canvasDrag_ & 16) {
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
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_PRESET")) {
                placePresetAt(static_cast<usize>(*static_cast<const i32*>(pl->Data)), mouse.x, mouse.y);
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("AVER_UI_USE")) {
                placeUseAt(std::string(static_cast<const char*>(pl->Data)), mouse.x, mouse.y);
            }
            ImGui::EndDragDropTarget();
        }

        // Keyboard: delete, duplicate, nudge.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && selected_ > 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelection();
            else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelected();
            else {
                const f32 step = (io.KeyShift ? 10.0f : 1.0f) * zoom * preview_.scale();
                f32 dx = 0, dy = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx = -step;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx = step;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy = -step;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy = step;
                if ((dx != 0.0f || dy != 0.0f) && selection().size() > 1) {
                    const f32 sc = preview_.scale() > 0.0f ? preview_.scale() : 1.0f;
                    moveSelectionBy(dx / zoom / sc, dy / zoom / sc, true);
                } else if (dx != 0.0f || dy != 0.0f) {
                    dragSelected(dx / zoom, dy / zoom, true);
                }
            }
        }
    }

    dl->PopClipRect();
    metrics_ = nullptr;
}

// ---- the bottom panel: animation timeline, bindings, events --------------------------------------------------

namespace {

// A text cell that commits on Enter or when it loses focus. The text being typed lives here while the cell is active, so the
// model (and its undo) is only touched once per edit. One cell is active at a time.
bool editCell(const char* label, const std::string& current, std::string& committed) {
    static ImGuiID active = 0;
    static std::string typing;
    const ImGuiID id = ImGui::GetID(label);
    char tmp[512];
    std::snprintf(tmp, sizeof tmp, "%s", (active == id ? typing : current).c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(label, tmp, sizeof tmp);
    if (ImGui::IsItemActive()) { active = id; typing = tmp; }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        active = 0;
        committed = tmp;
        return true;
    }
    if (ImGui::IsItemDeactivated() && active == id) active = 0;
    return false;
}

const char* const kEventNames[] = {"click", "toggled", "valuechanged", "selectionchanged", "textchanged", "submitted", "focus", "blur", "hover", "unhover", "cancel", "command", "finished"};
const char* const kActionNames[] = {"command", "graph", "method"};

std::string joinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (usize i = 0; i < args.size(); ++i) { if (i) s += ' '; s += args[i]; }
    return s;
}

std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : text) {
        if (c == ' ') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

void UiLayoutEditor::drawBottomPanel() {
    const char* const tabs[] = {"Animation", "Bindings", "Events", "Theme"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Selectable(tabs[i], bottomTab_ == i, 0, ImVec2(ImGui::CalcTextSize(tabs[i]).x + 18.0f, 0))) bottomTab_ = i;
    }
    ImGui::Separator();
    if (bottomTab_ == 0) drawTimeline();
    else if (bottomTab_ == 1) drawBindingsTab();
    else if (bottomTab_ == 2) drawEventsTab();
    else drawThemeSection();
}

// The layout's theme: a built-in, a project .octheme, or a new sheet. Used by the toolbar and the document section.
void UiLayoutEditor::drawThemePicker() {
    ImGui::PushID("themepicker");
    const std::string shown = themeIsSheet() ? std::filesystem::path(doc_.theme).filename().string() : doc_.theme;
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("Theme", shown.c_str())) {
        for (const char* const* t = uiBuiltinThemeNames(); *t; ++t)
            if (ImGui::Selectable(*t, doc_.theme == *t)) setTheme(*t);
        const std::vector<std::string> sheets = projectThemes();
        if (!sheets.empty()) ImGui::Separator();
        for (const std::string& s : sheets)
            if (ImGui::Selectable(s.c_str(), doc_.theme == s)) setTheme(s);
        ImGui::Separator();
        if (ImGui::Selectable("New theme sheet...")) { bottomTab_ = 3; bottomOpen_ = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The layout's theme: a built-in one or an .octheme sheet (edited in the Theme tab below).");
    ImGui::PopID();
}

// The Theme tab: create a sheet from the theme in force, and edit one with live preview. Every edit is an undo entry.
void UiLayoutEditor::drawThemeSection() {
    if (!themeIsSheet()) {
        ImGui::TextWrapped("'%s' is a built-in theme and cannot be changed. A sheet starts from it and holds only what you change.", doc_.theme.c_str());
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", newThemePath_.c_str());
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText("Path in Content", buf, sizeof buf)) newThemePath_ = buf;
        ImGui::SameLine();
        if (ImGui::Button("Create sheet") && !createThemeSheet(newThemePath_)) AVER_WARN("[UiLayoutEditor] {}", status_);
        if (!status_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", status_.c_str());
        return;
    }
    if (!themeError_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", themeError_.c_str());
        ImGui::TextDisabled("The preview shows the dark theme. Pick another theme above.");
        return;
    }
    ImGui::Text("%s%s", doc_.theme.c_str(), themeDirty_ ? "  (unsaved, saved with the layout)" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("based on %s", themeBase_.c_str());

    // One undo entry per gesture: the first change of a drag pushes, the rest continue it.
    const auto edit = [&](const std::function<void(UiTheme&)>& f) {
        if (editTheme(f, !themeEditing_)) themeEditing_ = true;
    };
    const auto settle = [&]() { if (ImGui::IsItemDeactivated()) themeEditing_ = false; };
    const auto colour = [&](const char* label, u32 cur, const std::function<void(UiTheme&, u32)>& set) {
        f32 c[4];
        const ImVec4 cv = colourVec(cur);
        c[0] = cv.x; c[1] = cv.y; c[2] = cv.z; c[3] = cv.w;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar)) { const u32 v = colourPack(c); edit([&](UiTheme& t) { set(t, v); }); }
        settle();
    };
    const auto length = [&](const char* label, f32 cur, f32 lo, f32 hi, const std::function<void(UiTheme&, f32)>& set) {
        f32 v = cur;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::DragFloat(label, &v, 0.25f, lo, hi, "%.0f")) { const f32 r = std::round(v); edit([&](UiTheme& t) { set(t, r); }); }
        settle();
    };

    if (::aver::editor::sectionHeader("Theme", ImGuiTreeNodeFlags_DefaultOpen)) {
        length("Base font size", theme_.baseFontSize, 6.0f, 96.0f, [](UiTheme& t, f32 v) { t.baseFontSize = v; });
        colour("Focus ring", theme_.focusColor, [](UiTheme& t, u32 v) { t.focusColor = v; });
        length("Focus ring width", theme_.focusWidth, 0.0f, 12.0f, [](UiTheme& t, f32 v) { t.focusWidth = v; });
        colour("Modal veil", theme_.dim, [](UiTheme& t, u32 v) { t.dim = v; });
        length("List row height", theme_.rowHeight, 8.0f, 200.0f, [](UiTheme& t, f32 v) { t.rowHeight = v; });
        length("Scrollbar width", theme_.scrollbarWidth, 2.0f, 40.0f, [](UiTheme& t, f32 v) { t.scrollbarWidth = v; });
    }

    constexpr i32 kKinds = static_cast<i32>(UiWidgetKind::Count);
    const i32 namedCount = static_cast<i32>(theme_.named.size());
    themeSel_ = std::clamp(themeSel_, 0, kKinds + namedCount - 1);
    const auto selName = [&](i32 i) { return i < kKinds ? std::string(kKindLabels[i]) : "style: " + theme_.named[static_cast<usize>(i - kKinds)].first; };
    ImGui::Separator();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("Style of", selName(themeSel_).c_str())) {
        for (i32 i = 0; i < kKinds + namedCount; ++i)
            if (ImGui::Selectable(selName(i).c_str(), i == themeSel_)) themeSel_ = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", newVariant_.c_str());
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputText("##newvariant", buf, sizeof buf)) newVariant_ = buf;
        ImGui::SameLine();
        if (ImGui::Button("Add variant") && !newVariant_.empty()) {
            const std::string nv = newVariant_;
            bool exists = false;
            for (const auto& n : theme_.named) exists = exists || n.first == nv;
            if (!exists) {
                edit([&](UiTheme& t) { t.named.emplace_back(nv, UiStyle{}); });
                themeEditing_ = false;
                themeSel_ = kKinds + static_cast<i32>(theme_.named.size()) - 1;
                newVariant_.clear();
            }
        }
        if (themeSel_ >= kKinds) {
            ImGui::SameLine();
            if (ImGui::Button("Delete variant")) {
                const usize at = static_cast<usize>(themeSel_ - kKinds);
                edit([&](UiTheme& t) { t.named.erase(t.named.begin() + static_cast<std::ptrdiff_t>(at)); });
                themeEditing_ = false;
                themeSel_ = 0;
                return;
            }
        }
    }
    if (themeSel_ >= kKinds) ImGui::TextDisabled("A variant only overrides what is set (empty colour, 0 length: the widget's own style shows through).");

    // The selected style, by reference into the live theme for reading and through edit() for writing.
    const bool isKind = themeSel_ < kKinds;
    const auto styleOf = [&](UiTheme& t) -> UiStyle& { return isKind ? t.kinds[static_cast<usize>(themeSel_)] : t.named[static_cast<usize>(themeSel_ - kKinds)].second; };
    const UiStyle cur = styleOf(theme_);
    const auto sc = [&](const char* label, u32 value, u32 UiStyle::*member) {
        colour(label, value, [this, &styleOf, member](UiTheme& t, u32 v) { styleOf(t).*member = v; });
    };
    if (::aver::editor::sectionHeader("Colours", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* const states[4] = {"Background", "Background hovered", "Background pressed", "Background disabled"};
        for (int i = 0; i < 4; ++i)
            colour(states[i], cur.background[i], [&styleOf, i](UiTheme& t, u32 v) { styleOf(t).background[i] = v; });
        sc("Border", cur.border, &UiStyle::border);
        sc("Text", cur.text, &UiStyle::text);
        sc("Text disabled", cur.textDisabled, &UiStyle::textDisabled);
        sc("Accent", cur.accent, &UiStyle::accent);
        sc("Track", cur.track, &UiStyle::track);
        sc("Placeholder", cur.placeholder, &UiStyle::placeholder);
    }
    if (::aver::editor::sectionHeader("Shape and spacing", ImGuiTreeNodeFlags_DefaultOpen)) {
        length("Corner radius", cur.radius, 0.0f, 64.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).radius = v; });
        length("Border width", cur.borderWidth, 0.0f, 16.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).borderWidth = v; });
        length("Padding left", cur.padding.left, 0.0f, 200.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).padding.left = v; });
        length("Padding top", cur.padding.top, 0.0f, 200.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).padding.top = v; });
        length("Padding right", cur.padding.right, 0.0f, 200.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).padding.right = v; });
        length("Padding bottom", cur.padding.bottom, 0.0f, 200.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).padding.bottom = v; });
    }
    if (::aver::editor::sectionHeader("Font", ImGuiTreeNodeFlags_DefaultOpen)) {
        length("Font size (0 = base)", cur.fontSize, 0.0f, 128.0f, [&styleOf](UiTheme& t, f32 v) { styleOf(t).fontSize = v; });
        const char* const weights[3] = {"Inherit", "Regular", "Bold"};
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Weight", weights[cur.bold + 1])) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(weights[i], cur.bold + 1 == i)) { edit([&styleOf, i](UiTheme& t) { styleOf(t).bold = static_cast<i8>(i - 1); }); themeEditing_ = false; }
            ImGui::EndCombo();
        }
        char fam[128];
        std::snprintf(fam, sizeof fam, "%s", cur.fontFamily.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("Font family", fam, sizeof fam)) { const std::string f = fam; edit([&](UiTheme& t) { styleOf(t).fontFamily = f; }); }
        settle();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A family of the project's font set (Content/UI/Fonts.ocfonts); empty keeps the default.");
    }
}

void UiLayoutEditor::drawTimeline() {
    const std::vector<std::string> names = animationNames();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##anim", animName_.empty() ? "(no animation)" : animName_.c_str())) {
        for (const std::string& n : names)
            if (ImGui::Selectable(n.c_str(), n == animName_)) setTimelineAnimation(n);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) addAnimation();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new animation with an opacity track for the selected widget.");
    ImGui::SameLine();
    ImGui::BeginDisabled(animName_.empty());
    if (ImGui::Button("Delete")) deleteAnimation(animName_);
    ImGui::SameLine();
    {
        std::string renamed;
        ImGui::SetNextItemWidth(110.0f);
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof nameBuf, "%s", animName_.c_str());
        ImGui::InputText("##animrename", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit() && nameBuf != animName_) renameAnimation(animName_, nameBuf);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rename (Enter).");
    }
    ImGui::SameLine();
    if (ImGui::Button(animPlaying_ ? "Pause" : "Play")) timelinePlay(!animPlaying_);
    ImGui::SameLine();
    if (ImGui::Button("|<")) timelineSeek(0.0f);
    ImGui::SameLine();
    bool loop = animLoop_;
    if (ImGui::Checkbox("Loop", &loop)) setTimelineLoop(loop);
    const f32 len = animName_.empty() ? 0.0f : animationLength(animName_);
    ImGui::SameLine();
    f32 t = animTime_;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("##tltime", &t, 0.01f, 0.0f, std::max(len, 0.01f), "%.2fs")) timelineSeek(t);
    ImGui::SameLine();
    ImGui::TextDisabled("of %.2fs", len);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("##tlzoom", &timelinePxPerSec_, 2.0f, 30.0f, 800.0f, "%.0f px/s");
    ImGui::EndDisabled();

    // Add a track for the selected widget (this also makes the first animation).
    if (inRange(doc_, selected_)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        const char* const propLabel = uiAnimPropName(static_cast<UiAnimProp>(timelineAddProp_));
        if (ImGui::BeginCombo("##trackprop", propLabel)) {
            for (i32 i = 0; i < static_cast<i32>(UiAnimProp::Count); ++i)
                if (ImGui::Selectable(uiAnimPropName(static_cast<UiAnimProp>(i)), i == timelineAddProp_)) timelineAddProp_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const std::string target = animName_.empty() ? std::string("Anim") : animName_;
        if (ImGui::Button("Add track")) {
            addTrack(target, selected_, static_cast<UiAnimProp>(timelineAddProp_));
            if (animName_.empty()) setTimelineAnimation(target);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animate this property of the selected widget.");
    }
    if (animName_.empty()) {
        ImGui::TextDisabled("Select a widget and press New, or pick an animation. Scrub the ruler to preview; keys can be dragged.");
        return;
    }

    const std::vector<UiTimelineTrack> tracks = animationTracks(animName_);
    const f32 labelW = 200.0f, rowH = 22.0f, rulerH = 22.0f;
    const f32 editorH = ImGui::GetFrameHeightWithSpacing() * 2.0f + 4.0f;
    if (ImGui::BeginChild("##timeline", ImVec2(0, std::max(40.0f, ImGui::GetContentRegionAvail().y - editorH)), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const f32 width = labelW + (std::max(len, 1.0f) + 1.0f) * timelinePxPerSec_;
        const f32 height = rulerH + rowH * static_cast<f32>(tracks.size()) + 6.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tlarea", ImVec2(width, std::max(height, ImGui::GetContentRegionAvail().y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const f32 x0 = p0.x + labelW;
        const auto timeAt = [&](f32 x) { return std::max(0.0f, (x - x0) / timelinePxPerSec_); };
        const auto xOf = [&](f32 time) { return x0 + time * timelinePxPerSec_; };
        const auto rowAt = [&](f32 y) { return static_cast<i32>(std::floor((y - p0.y - rulerH) / rowH)); };

        // Rows, ruler and playhead.
        for (usize r = 0; r < tracks.size(); ++r) {
            const f32 y = p0.y + rulerH + rowH * static_cast<f32>(r);
            dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + width, y + rowH), (static_cast<i32>(r) == timelineTrack_) ? IM_COL32(60, 70, 92, 255) : (r % 2 ? IM_COL32(40, 42, 48, 255) : IM_COL32(36, 38, 43, 255)));
            const std::string label = tracks[r].widget + "." + uiAnimPropName(tracks[r].prop);
            dl->AddText(ImVec2(p0.x + 6.0f, y + 3.0f), IM_COL32(215, 218, 226, 255), label.c_str());
            if (tracks[r].keys.size() > 1) {
                const f32 a = xOf(tracks[r].keys.front().time), b = xOf(tracks[r].keys.back().time);
                dl->AddRectFilled(ImVec2(a, y + rowH * 0.5f - 1.5f), ImVec2(b, y + rowH * 0.5f + 1.5f), IM_COL32(90, 110, 150, 160));
            }
        }
        dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + rulerH), IM_COL32(50, 53, 61, 255));
        const f32 major = timelinePxPerSec_ >= 100.0f ? 0.5f : 1.0f;
        for (f32 time = 0.0f; xOf(time) < p0.x + width; time += 0.1f) {
            const bool whole = std::fmod(time + 1e-4f, major) < 0.02f;
            const f32 x = xOf(time);
            dl->AddLine(ImVec2(x, p0.y + rulerH - (whole ? 10.0f : 5.0f)), ImVec2(x, p0.y + rulerH), IM_COL32(150, 154, 166, 255));
            if (whole) {
                char l[16];
                std::snprintf(l, sizeof l, "%.1f", time);
                dl->AddText(ImVec2(x + 3.0f, p0.y + 2.0f), IM_COL32(180, 184, 196, 255), l);
            }
        }
        dl->AddLine(ImVec2(x0, p0.y), ImVec2(x0, p0.y + height), IM_COL32(110, 116, 130, 255));

        // Keys.
        i32 hoverTrack = -1, hoverKey = -1;
        for (usize r = 0; r < tracks.size(); ++r) {
            const f32 cy = p0.y + rulerH + rowH * (static_cast<f32>(r) + 0.5f);
            for (usize k = 0; k < tracks[r].keys.size(); ++k) {
                f32 time = tracks[r].keys[k].time;
                if (timelineDragging_ && static_cast<i32>(r) == timelineTrack_ && static_cast<i32>(k) == timelineKey_) time = timelineDragTime_;
                const f32 cx = xOf(time);
                const bool sel = static_cast<i32>(r) == timelineTrack_ && static_cast<i32>(k) == timelineKey_;
                if (hovered && std::fabs(io.MousePos.x - cx) <= 7.0f && std::fabs(io.MousePos.y - cy) <= rowH * 0.5f) { hoverTrack = static_cast<i32>(r); hoverKey = static_cast<i32>(k); }
                const ImU32 fill = sel ? IM_COL32(255, 205, 80, 255) : (hoverTrack == static_cast<i32>(r) && hoverKey == static_cast<i32>(k) ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 175, 255, 255));
                dl->AddQuadFilled(ImVec2(cx, cy - 6), ImVec2(cx + 6, cy), ImVec2(cx, cy + 6), ImVec2(cx - 6, cy), fill);
                dl->AddQuad(ImVec2(cx, cy - 6), ImVec2(cx + 6, cy), ImVec2(cx, cy + 6), ImVec2(cx - 6, cy), IM_COL32(20, 22, 28, 255));
            }
        }
        const f32 px = xOf(animTime_);
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + std::max(height, 24.0f)), IM_COL32(240, 90, 80, 255), 2.0f);
        dl->AddTriangleFilled(ImVec2(px - 5, p0.y), ImVec2(px + 5, p0.y), ImVec2(px, p0.y + 8), IM_COL32(240, 90, 80, 255));

        // Pointer: a press on the ruler scrubs; on a key selects and drags it; on a row selects it (double-click adds a key).
        const bool pressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(0);
        if (pressed) {
            timelineDragging_ = false;
            if (io.MousePos.y < p0.y + rulerH) {
                timelineTrack_ = -1; timelineKey_ = -1;
                scrubbing_ = true;
                timelineSeek(timeAt(io.MousePos.x));
            } else if (hoverKey >= 0) {
                timelineTrack_ = hoverTrack; timelineKey_ = hoverKey;
                timelineDragging_ = true;
                timelineDragTime_ = tracks[static_cast<usize>(hoverTrack)].keys[static_cast<usize>(hoverKey)].time;
                timelineDragMoved_ = false;
                timelineSeek(timelineDragTime_);
            } else {
                const i32 row = rowAt(io.MousePos.y);
                timelineTrack_ = row >= 0 && static_cast<usize>(row) < tracks.size() ? row : -1;
                timelineKey_ = -1;
                if (io.MousePos.x > x0) timelineSeek(timeAt(io.MousePos.x));
                if (timelineTrack_ >= 0 && ImGui::IsMouseDoubleClicked(0)) timelineKey_ = addKey(animName_, timelineTrack_, timeAt(io.MousePos.x));
            }
        }
        if (active && ImGui::IsMouseDoubleClicked(0) && !pressed) {
            const i32 row = rowAt(io.MousePos.y);
            if (row >= 0 && static_cast<usize>(row) < tracks.size() && io.MousePos.x > x0 && hoverKey < 0) {
                timelineTrack_ = row;
                timelineKey_ = addKey(animName_, row, timeAt(io.MousePos.x));
            }
        }
        if (active && scrubbing_) timelineSeek(timeAt(io.MousePos.x));
        if (active && timelineDragging_) {
            f32 nt = timeAt(io.MousePos.x);
            if (!io.KeyShift) nt = std::round(nt * 100.0f) / 100.0f;   // 10 ms snap; Shift drags free
            if (std::fabs(nt - timelineDragTime_) > 1e-5f) timelineDragMoved_ = true;
            timelineDragTime_ = nt;
            timelineSeek(nt);
        }
        if (!active) {
            if (timelineDragging_ && timelineDragMoved_) timelineKey_ = moveKey(animName_, timelineTrack_, timelineKey_, timelineDragTime_);
            timelineDragging_ = false;
            scrubbing_ = false;
        }
        if (hovered && ImGui::IsMouseClicked(1)) {
            if (hoverKey >= 0) { timelineTrack_ = hoverTrack; timelineKey_ = hoverKey; }
            ImGui::OpenPopup("##keymenu");
        }
        if (ImGui::BeginPopup("##keymenu")) {
            const bool haveKey = timelineTrack_ >= 0 && timelineKey_ >= 0;
            if (ImGui::MenuItem("Add key at playhead", nullptr, false, timelineTrack_ >= 0)) timelineKey_ = addKey(animName_, timelineTrack_, animTime_);
            if (ImGui::MenuItem("Delete key", nullptr, false, haveKey)) { deleteKey(animName_, timelineTrack_, timelineKey_); timelineKey_ = -1; }
            if (ImGui::MenuItem("Delete track", nullptr, false, timelineTrack_ >= 0)) { deleteTrack(animName_, timelineTrack_); timelineTrack_ = timelineKey_ = -1; }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // The selected key.
    const std::vector<UiTimelineTrack> now = animationTracks(animName_);
    if (timelineTrack_ >= 0 && static_cast<usize>(timelineTrack_) < now.size() && timelineKey_ >= 0 &&
        static_cast<usize>(timelineKey_) < now[static_cast<usize>(timelineTrack_)].keys.size()) {
        const UiTimelineTrack& tr = now[static_cast<usize>(timelineTrack_)];
        const UiAnimKey& key = tr.keys[static_cast<usize>(timelineKey_)];
        ImGui::Text("%s.%s key %d", tr.widget.c_str(), uiAnimPropName(tr.prop), timelineKey_);
        ImGui::SameLine();
        f32 kt = key.time;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat("##keytime", &kt, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            const i32 moved = moveKey(animName_, timelineTrack_, timelineKey_, kt);
            if (moved >= 0) timelineKey_ = moved;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time in seconds (Enter).");
        ImGui::SameLine();
        UiAnimKey plain = key;
        plain.ease = UiEase::Linear;
        std::string valueText = uiAnimKeyToken(tr.prop, plain), committed;
        ImGui::SetNextItemWidth(120.0f);
        if (editCell("##keyvalue", valueText, committed)) setKeyValue(animName_, timelineTrack_, timelineKey_, committed);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Value: a number, x,y, or #RRGGBBAA.");
        if (uiAnimPropKind(tr.prop) == UiAnimKind::Colour) {
            ImGui::SameLine();
            ImGui::ColorButton("##keyswatch", ImVec4(key.v[0] / 255.0f, key.v[1] / 255.0f, key.v[2] / 255.0f, key.v[3] / 255.0f));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##keyease", uiEaseName(key.ease))) {
            for (i32 e = 0; e <= static_cast<i32>(UiEase::CubicInOut); ++e)
                if (ImGui::Selectable(uiEaseName(static_cast<UiEase>(e)), static_cast<UiEase>(e) == key.ease)) setKeyEase(animName_, timelineTrack_, timelineKey_, static_cast<UiEase>(e));
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the value moves from this key to the next.");
        ImGui::SameLine();
        if (ImGui::Button("Delete key")) { deleteKey(animName_, timelineTrack_, timelineKey_); timelineKey_ = -1; }
        ImGui::SameLine();
        if (ImGui::Button("Delete track")) { deleteTrack(animName_, timelineTrack_); timelineTrack_ = timelineKey_ = -1; }
    } else {
        ImGui::TextDisabled("Click a key to edit it. Double-click a row to add a key; drag a key to move it (Shift: no snap); right-click for more.");
    }
}

void UiLayoutEditor::drawBindingsTab() {
    ImGui::TextDisabled("A binding copies a source to a widget property, one way. vm:Path = C# view model, var:Name = graph variable. "
                        "Format example: Score: {0:D6}");
    const f32 footer = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (ImGui::BeginTable("##binds", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(40.0f, ImGui::GetContentRegionAvail().y - footer)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Source and path", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("On change", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        const std::string selName = inRange(doc_, selected_) ? doc_.nodes[static_cast<usize>(selected_)].props.name : std::string();
        for (usize b = 0; b < doc_.binds.size(); ++b) {
            const UiLayoutBind row = doc_.binds[b];
            std::string format;
            bool onChange = false;
            for (usize i = 0; i + 1 < row.rest.size(); i += 2) {
                if (row.rest[i] == "format") format = row.rest[i + 1];
                else if (row.rest[i] == "mode") onChange = row.rest[i + 1] == "change";
            }
            ImGui::PushID(static_cast<int>(b));
            ImGui::TableNextRow();
            if (row.widget == selName) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 80, 120, 70));
            bool mutated = false;
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(row.widget.c_str())) select(uiLayoutFind(doc_, row.widget));
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##prop", row.property.c_str())) {
                const i32 wi = uiLayoutFind(doc_, row.widget);
                for (const UiPropDesc& d : uiProperties()) {
                    if (wi >= 0 && !(d.kindMask & uiKindBit(doc_.nodes[static_cast<usize>(wi)].props.kind))) continue;
                    if (ImGui::Selectable(d.key, row.property == d.key)) { setBinding(static_cast<i32>(b), d.key, row.path, format, onChange); mutated = true; }
                }
                ImGui::EndCombo();
            }
            std::string text;
            ImGui::TableSetColumnIndex(2);
            if (!mutated && editCell("##path", row.path, text)) { setBinding(static_cast<i32>(b), row.property, text, format, onChange); mutated = true; }
            ImGui::TableSetColumnIndex(3);
            if (!mutated && editCell("##format", format, text)) { setBinding(static_cast<i32>(b), row.property, row.path, text, onChange); mutated = true; }
            ImGui::TableSetColumnIndex(4);
            if (!mutated && ImGui::Checkbox("##change", &onChange)) { setBinding(static_cast<i32>(b), row.property, row.path, format, onChange); mutated = true; }
            ImGui::TableSetColumnIndex(5);
            if (!mutated && ImGui::SmallButton("x")) { deleteBinding(static_cast<i32>(b)); mutated = true; }
            ImGui::PopID();
            if (mutated) break;
        }
        ImGui::EndTable();
    }
    // Add: the selected widget, a property, a path.
    if (inRange(doc_, selected_)) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s.", doc_.nodes[static_cast<usize>(selected_)].props.name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##newprop", newBindProp_.c_str())) {
            for (const UiPropDesc& d : uiProperties())
                if ((d.kindMask & uiKindBit(doc_.nodes[static_cast<usize>(selected_)].props.kind)) && ImGui::Selectable(d.key, newBindProp_ == d.key)) newBindProp_ = d.key;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        char pathBuf[256], fmtBuf[128];
        std::snprintf(pathBuf, sizeof pathBuf, "%s", newBindPath_.c_str());
        std::snprintf(fmtBuf, sizeof fmtBuf, "%s", newBindFormat_.c_str());
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputTextWithHint("##newpath", "vm:Player.Score or var:Health", pathBuf, sizeof pathBuf)) newBindPath_ = pathBuf;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputTextWithHint("##newfmt", "format {0}", fmtBuf, sizeof fmtBuf)) newBindFormat_ = fmtBuf;
        ImGui::SameLine();
        ImGui::Checkbox("on change", &newBindChange_);
        ImGui::SameLine();
        if (ImGui::Button("Add binding") && addBinding(selected_, newBindProp_, newBindPath_, newBindFormat_, newBindChange_) >= 0) newBindPath_.clear();
    }
}

void UiLayoutEditor::drawEventsTab() {
    ImGui::TextDisabled("An event route sends a widget event to a command (raised as a Command event), a graph event or a C# method. "
                        "Extra arguments are passed on.");
    const f32 footer = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (ImGui::BeginTable("##events", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(40.0f, ImGui::GetContentRegionAvail().y - footer)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        const std::string selName = inRange(doc_, selected_) ? doc_.nodes[static_cast<usize>(selected_)].props.name : std::string();
        for (usize e = 0; e < doc_.events.size(); ++e) {
            const UiLayoutEvent row = doc_.events[e];
            ImGui::PushID(static_cast<int>(e));
            ImGui::TableNextRow();
            if (row.widget == selName) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 80, 120, 70));
            bool mutated = false;
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(row.widget.c_str())) select(uiLayoutFind(doc_, row.widget));
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##event", row.event.c_str())) {
                for (const char* n : kEventNames)
                    if (ImGui::Selectable(n, row.event == n)) { setEvent(static_cast<i32>(e), n, row.action, row.target, row.rest); mutated = true; }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            if (!mutated && ImGui::BeginCombo("##action", row.action.c_str())) {
                for (const char* n : kActionNames)
                    if (ImGui::Selectable(n, row.action == n)) { setEvent(static_cast<i32>(e), row.event, n, row.target, row.rest); mutated = true; }
                ImGui::EndCombo();
            }
            std::string text;
            ImGui::TableSetColumnIndex(3);
            if (!mutated && editCell("##target", row.target, text)) { setEvent(static_cast<i32>(e), row.event, row.action, text, row.rest); mutated = true; }
            ImGui::TableSetColumnIndex(4);
            if (!mutated && editCell("##args", joinArgs(row.rest), text)) { setEvent(static_cast<i32>(e), row.event, row.action, row.target, splitArgs(text)); mutated = true; }
            ImGui::TableSetColumnIndex(5);
            if (!mutated && ImGui::SmallButton("x")) { deleteEvent(static_cast<i32>(e)); mutated = true; }
            ImGui::PopID();
            if (mutated) break;
        }
        ImGui::EndTable();
    }
    if (inRange(doc_, selected_)) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", doc_.nodes[static_cast<usize>(selected_)].props.name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::BeginCombo("##newevent", newEvent_.c_str())) {
            for (const char* n : kEventNames) if (ImGui::Selectable(n, newEvent_ == n)) newEvent_ = n;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("##newaction", newEventAction_.c_str())) {
            for (const char* n : kActionNames) if (ImGui::Selectable(n, newEventAction_ == n)) newEventAction_ = n;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        char targetBuf[256];
        std::snprintf(targetBuf, sizeof targetBuf, "%s", newEventTarget_.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##newtarget", newEventAction_ == "command" ? "command, e.g. game.start or ui.close" : newEventAction_ == "graph" ? "graph event name" : "Method or Member.Method", targetBuf, sizeof targetBuf))
            newEventTarget_ = targetBuf;
        ImGui::SameLine();
        if (ImGui::Button("Add event") && addEvent(selected_, newEvent_, newEventAction_, newEventTarget_) >= 0) newEventTarget_.clear();
    }
}

void UiLayoutEditor::drawStates() {
    static const char* const names[] = {"Normal", "Hovered", "Pressed", "Disabled"};
    ImGui::TextUnformatted("Preview as");
    ImGui::SameLine();
    for (i32 i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(names[i], previewState_ == i)) setPreviewState(i);
    }
    ImGui::TextDisabled("Normal is bgColor / textColor (Appearance). A state colour left at none derives from the normal one.");
    if (interact_) ImGui::TextDisabled("(Interact is on: the preview shows real hover and press instead.)");
}

void UiLayoutEditor::draw(Engine& e) {
    g_drawEngine = &e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    drawToolbar();
    if (!status_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", status_.c_str());
    else if (!previewErrors_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s%s", previewErrors_[0].c_str(),
                           previewErrors_.size() > 1 ? (" (+" + std::to_string(previewErrors_.size() - 1) + " more)").c_str() : "");
    ImGui::Separator();

    const f32 dpi = fontScale();
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
    if (ImGui::BeginChild("##uicenter", ImVec2(canvasW, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const f32 totalH = ImGui::GetContentRegionAvail().y;
        const f32 minBottom = 120.0f * dpi, minCanvas = 160.0f * dpi;
        f32 bottomH = 0.0f;
        if (bottomOpen_) bottomH = std::clamp(bottomHeight_ > 0.0f ? bottomHeight_ : totalH * 0.36f, minBottom, std::max(minBottom, totalH - minCanvas));
        const f32 split = bottomOpen_ ? 6.0f * dpi : 0.0f;
        if (ImGui::BeginChild("##uicanvas", ImVec2(0, std::max(32.0f, totalH - bottomH - split)), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            drawCanvas();
        ImGui::EndChild();
        if (bottomOpen_) {
            ImGui::InvisibleButton("##uisplitbottom", ImVec2(-1, split));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) bottomHeight_ = std::clamp(bottomH - ImGui::GetIO().MouseDelta.y, minBottom, std::max(minBottom, totalH - minCanvas));
            if (ImGui::BeginChild("##uibottom", ImVec2(0, 0), true)) drawBottomPanel();
            ImGui::EndChild();
        }
    }
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
