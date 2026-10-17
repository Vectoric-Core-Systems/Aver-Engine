// The Font Set editor tab and the editors' shared text preview. See the header.

#include "FontSetEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "aver/rhi/RHI.hpp"
#  include "aver/runtime/Engine.hpp"
#endif

namespace aver::editor {

using namespace aver::ui;

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

i32 findFamily(const UiFontSetDoc& d, std::string_view name) {
    for (usize i = 0; i < d.families.size(); ++i)
        if (ieq(d.families[i].name, name)) return static_cast<i32>(i);
    return -1;
}

bool familyOk(const UiFontSetDoc& d, i32 i) { return i >= 0 && static_cast<usize>(i) < d.families.size(); }

// A family name no other family has (case-insensitively); `skip` is the family being renamed.
std::string uniqueName(const UiFontSetDoc& d, std::string_view want, i32 skip) {
    std::string base(want);
    if (base.empty()) base = "Family";
    std::string name = base;
    for (int n = 2;; ++n) {
        bool taken = false;
        for (usize i = 0; i < d.families.size(); ++i)
            if (static_cast<i32>(i) != skip && ieq(d.families[i].name, name)) taken = true;
        if (!taken) return name;
        name = base + " " + std::to_string(n);
    }
}

int groupOf(const UiFontEntry& e) { return static_cast<int>(e.source); }

// Inserts after the last entry whose group is not later than the new entry's, so the chain stays Project, System, Bitmap.
i32 insertEntry(UiFontFamilyDef& f, UiFontEntry e) {
    const int g = groupOf(e);
    usize pos = 0;
    for (usize i = f.entries.size(); i-- > 0;)
        if (groupOf(f.entries[i]) <= g) { pos = i + 1; break; }
    f.entries.insert(f.entries.begin() + static_cast<std::ptrdiff_t>(pos), std::move(e));
    return static_cast<i32>(pos);
}

bool parseFloatStrict(std::string_view s, f32& out) {
    if (s.empty()) return false;
    const std::string t(s);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != 0 || !std::isfinite(v)) return false;
    out = static_cast<f32>(v);
    return true;
}

bool parseIntStrict(std::string_view s, i32& out) {
    if (s.empty()) return false;
    const std::string t(s);
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != 0) return false;
    out = static_cast<i32>(v);
    return true;
}

bool parseBool(std::string_view s, bool& out) {
    if (ieq(s, "true") || s == "1" || ieq(s, "yes")) { out = true; return true; }
    if (ieq(s, "false") || s == "0" || ieq(s, "no")) { out = false; return true; }
    return false;
}

bool entryOk(const UiFontSetDoc& d, i32 fam, i32 entry) {
    return familyOk(d, fam) && entry >= 0 && static_cast<usize>(entry) < d.families[static_cast<usize>(fam)].entries.size();
}

std::string joinPath(const std::string& dir, const std::string& name) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    p += name;
    return p;
}

bool hasDotDot(const std::string& name) {
    usize i = 0;
    while (i <= name.size()) {
        usize j = name.find_first_of("/\\", i);
        if (j == std::string::npos) j = name.size();
        if (name.compare(i, j - i, "..") == 0) return true;
        i = j + 1;
    }
    return false;
}

// Opens a font the way the games' hosts do (GameUiText.hpp), except nothing is traced: this is the editor.
bool openFontFile(const std::string& root, UiFontKind kind, const std::string& name, UiFontBlob& out) {
    if (name.empty() || hasDotDot(name)) return false;
    std::vector<std::string> candidates;
    switch (kind) {
        case UiFontKind::Content: candidates.push_back(joinPath(root, name)); break;
        case UiFontKind::Engine: candidates.push_back(joinPath(executableDir(), name)); break;
        case UiFontKind::System:
#if defined(_WIN32)
            if (const char* win = std::getenv("WINDIR")) candidates.push_back(joinPath(std::string(win) + "\\Fonts", name));
            if (const char* local = std::getenv("LOCALAPPDATA"))
                candidates.push_back(joinPath(std::string(local) + "\\Microsoft\\Windows\\Fonts", name));
#endif
            break;
    }
    for (std::string& c : candidates) {
        toNativeSeparators(c);
        auto* held = new MappedFile();
        if (mapFileReadOnly(c, *held)) {
            out.data = held->data;
            out.size = held->size;
            out.owner = std::shared_ptr<void>(held, [](void* p) {
                auto* m = static_cast<MappedFile*>(p);
                unmap(*m);
                delete m;
            });
            return true;
        }
        delete held;
    }
    return false;
}

// Textures of previews that no longer exist, destroyed by the next preview that syncs on the same device.
struct Orphan {
    u32 tex = 0;
    i32 frame = 0;
    void* device = nullptr;
};
std::vector<Orphan>& orphans() {
    static std::vector<Orphan> v;
    return v;
}
i32 g_gpuFrame = 0;   // bumps once per ImGui frame that synced a preview

} // namespace

// ---- document edits -------------------------------------------------------------------------------

std::string fontSetStarterText() {
    return "OCFONTS 1\n"
           "# Fonts the game UI draws with. Each FAMILY is a fallback chain: a glyph comes from the first face that has it.\n"
           "# FACE paths are relative to Content; @exe/ means next to the executable.\n"
           "DEFAULT Body\n"
           "\n"
           "FAMILY Body\n"
           "FACE @exe/Roboto-Regular.ttf\n"
           "FACE @exe/Roboto-Medium.ttf weight=bold\n"
           "SYSTEM auto\n"
           "BITMAP legacy\n";
}

std::string fontContentRootOf(const std::string& assetPath) {
    std::string p = assetPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    usize found = std::string::npos;
    usize at = 0;
    while (at < p.size()) {
        const usize s = p.find('/', at);
        const usize end = s == std::string::npos ? p.size() : s;
        if (end - at == 7 && ieq(std::string_view(p).substr(at, 7), "Content") && s != std::string::npos) found = end;
        if (s == std::string::npos) break;
        at = s + 1;
    }
    if (found != std::string::npos) return p.substr(0, found);
    const usize slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

i32 fsAddFamily(UiFontSetDoc& d, std::string_view name) {
    UiFontFamilyDef f;
    f.name = uniqueName(d, name, -1);
    d.families.push_back(std::move(f));
    if (d.defaultFamily.empty() || findFamily(d, d.defaultFamily) < 0) d.defaultFamily = d.families.front().name;
    return static_cast<i32>(d.families.size() - 1);
}

bool fsRemoveFamily(UiFontSetDoc& d, i32 index) {
    if (!familyOk(d, index) || d.families.size() <= 1) return false;
    const std::string gone = d.families[static_cast<usize>(index)].name;
    d.families.erase(d.families.begin() + index);
    d.overrides.erase(std::remove_if(d.overrides.begin(), d.overrides.end(),
                                     [&](const UiFontOverride& o) { return ieq(o.from, gone) || ieq(o.to, gone); }),
                      d.overrides.end());
    if (ieq(d.defaultFamily, gone) || findFamily(d, d.defaultFamily) < 0) d.defaultFamily = d.families.front().name;
    return true;
}

bool fsRenameFamily(UiFontSetDoc& d, i32 index, std::string_view newName) {
    if (!familyOk(d, index) || newName.empty()) return false;
    for (usize i = 0; i < d.families.size(); ++i)
        if (static_cast<i32>(i) != index && ieq(d.families[i].name, newName)) return false;
    const std::string old = d.families[static_cast<usize>(index)].name;
    const std::string now(newName);
    d.families[static_cast<usize>(index)].name = now;
    if (ieq(d.defaultFamily, old)) d.defaultFamily = now;
    for (UiFontOverride& o : d.overrides) {
        if (ieq(o.from, old)) o.from = now;
        if (ieq(o.to, old)) o.to = now;
    }
    return true;
}

bool fsSetDefault(UiFontSetDoc& d, i32 index) {
    if (!familyOk(d, index)) return false;
    d.defaultFamily = d.families[static_cast<usize>(index)].name;
    return true;
}

i32 fsAddFace(UiFontSetDoc& d, i32 family, std::string_view file) {
    if (!familyOk(d, family) || file.empty()) return -1;
    UiFontEntry e;
    e.source = UiFontEntry::Source::Project;
    e.file = std::string(file);
    return insertEntry(d.families[static_cast<usize>(family)], std::move(e));
}

i32 fsAddSystem(UiFontSetDoc& d, i32 family, std::string_view file) {
    if (!familyOk(d, family)) return -1;
    UiFontEntry e;
    e.source = UiFontEntry::Source::System;
    e.file = file.empty() ? std::string("auto") : std::string(file);
    return insertEntry(d.families[static_cast<usize>(family)], std::move(e));
}

i32 fsAddBitmap(UiFontSetDoc& d, i32 family) {
    if (!familyOk(d, family)) return -1;
    UiFontFamilyDef& f = d.families[static_cast<usize>(family)];
    for (usize i = 0; i < f.entries.size(); ++i)
        if (f.entries[i].source == UiFontEntry::Source::Bitmap) return static_cast<i32>(i);
    UiFontEntry e;
    e.source = UiFontEntry::Source::Bitmap;
    e.file = "legacy";
    return insertEntry(f, std::move(e));
}

bool fsRemoveEntry(UiFontSetDoc& d, i32 family, i32 entry) {
    if (!entryOk(d, family, entry)) return false;
    auto& v = d.families[static_cast<usize>(family)].entries;
    v.erase(v.begin() + entry);
    return true;
}

i32 fsMoveEntry(UiFontSetDoc& d, i32 family, i32 entry, i32 delta) {
    if (!entryOk(d, family, entry) || delta == 0) return -1;
    auto& v = d.families[static_cast<usize>(family)].entries;
    const i32 to = entry + (delta < 0 ? -1 : 1);
    if (to < 0 || static_cast<usize>(to) >= v.size()) return -1;
    if (groupOf(v[static_cast<usize>(to)]) != groupOf(v[static_cast<usize>(entry)])) return -1;
    std::swap(v[static_cast<usize>(to)], v[static_cast<usize>(entry)]);
    return to;
}

bool fsSetAttr(UiFontSetDoc& d, i32 family, i32 entry, std::string_view key, std::string_view value) {
    if (!entryOk(d, family, entry)) return false;
    UiFontEntry& e = d.families[static_cast<usize>(family)].entries[static_cast<usize>(entry)];
    if (key == "file") {
        if (value.empty()) return false;
        e.file = std::string(value);
        return true;
    }
    if (key == "weight") {
        if (ieq(value, "bold")) e.bold = true;
        else if (ieq(value, "regular")) e.bold = false;
        else return false;
        return true;
    }
    if (key == "italic") {
        bool b = false;
        if (!parseBool(value, b)) return false;
        e.italic = b;
        return true;
    }
    if (key == "index") {
        i32 v = 0;
        if (!parseIntStrict(value, v) || v < 0) return false;
        e.index = v;
        return true;
    }
    if (key == "lang") {
        e.lang = std::string(value);
        return true;
    }
    if (key == "scale") {
        f32 v = 0.0f;
        if (!parseFloatStrict(value, v) || v <= 0.0f) return false;
        e.scale = v;
        return true;
    }
    if (key == "dy") {
        f32 v = 0.0f;
        if (!parseFloatStrict(value, v)) return false;
        e.dy = v;
        return true;
    }
    return false;
}

bool fsSetLineGap(UiFontSetDoc& d, i32 family, f32 multiplier) {
    if (!familyOk(d, family) || !(multiplier >= 0.0f) || !std::isfinite(multiplier)) return false;
    d.families[static_cast<usize>(family)].lineGap = multiplier;
    return true;
}

i32 fsAddOverride(UiFontSetDoc& d, std::string_view lang, std::string_view from, std::string_view to) {
    if (lang.empty()) return -1;
    const i32 f = findFamily(d, from), t = findFamily(d, to);
    if (f < 0 || t < 0) return -1;
    const std::string& fromName = d.families[static_cast<usize>(f)].name;
    const std::string& toName = d.families[static_cast<usize>(t)].name;
    for (usize i = 0; i < d.overrides.size(); ++i) {
        UiFontOverride& o = d.overrides[i];
        if (ieq(o.lang, lang) && ieq(o.from, fromName)) {
            o.to = toName;
            return static_cast<i32>(i);
        }
    }
    d.overrides.push_back(UiFontOverride{std::string(lang), fromName, toName});
    return static_cast<i32>(d.overrides.size() - 1);
}

bool fsRemoveOverride(UiFontSetDoc& d, i32 index) {
    if (index < 0 || static_cast<usize>(index) >= d.overrides.size()) return false;
    d.overrides.erase(d.overrides.begin() + index);
    return true;
}

std::string fsValidate(const UiFontSetDoc& d) {
    if (d.families.empty()) return "the font set needs at least one family";
    for (usize i = 0; i < d.families.size(); ++i) {
        const UiFontFamilyDef& f = d.families[i];
        if (f.name.empty()) return "family " + std::to_string(i + 1) + " has no name";
        for (usize j = 0; j < i; ++j)
            if (ieq(d.families[j].name, f.name)) return "two families are named '" + f.name + "'";
        for (const UiFontEntry& e : f.entries)
            if (e.source != UiFontEntry::Source::Bitmap && e.file.empty()) return "family '" + f.name + "' has an entry with no file";
    }
    if (!d.defaultFamily.empty() && findFamily(d, d.defaultFamily) < 0)
        return "DEFAULT names no family: '" + d.defaultFamily + "'";
    for (const UiFontOverride& o : d.overrides) {
        if (o.lang.empty()) return "an OVERRIDE has no language";
        if (findFamily(d, o.from) < 0) return "OVERRIDE " + o.lang + ": no family named '" + o.from + "'";
        if (findFamily(d, o.to) < 0) return "OVERRIDE " + o.lang + ": no family named '" + o.to + "'";
    }
    return {};
}

// ---- coverage -------------------------------------------------------------------------------------

FontCoverage fsCoverage(const UiFontFace& face) {
    struct Block { const char* name; u32 first, last; };
    static const Block kBlocks[] = {
        {"Basic Latin", 0x20, 0x7E},           {"Latin-1 Supplement", 0xA0, 0xFF},  {"Latin Extended-A", 0x100, 0x17F},
        {"General Punctuation", 0x2000, 0x206F}, {"Greek", 0x370, 0x3FF},           {"Cyrillic", 0x400, 0x4FF},
        {"Hebrew", 0x590, 0x5FF},              {"Arabic", 0x600, 0x6FF},            {"Devanagari", 0x900, 0x97F},
        {"Thai", 0xE00, 0xE7F},                {"Hiragana", 0x3040, 0x309F},        {"Katakana", 0x30A0, 0x30FF},
        {"CJK Unified Ideographs", 0x4E00, 0x9FFF}, {"Hangul Syllables", 0xAC00, 0xD7AF},
    };
    FontCoverage c;
    c.glyphs = face.glyphCount();
    c.kern = face.kernSource();
    for (const Block& b : kBlocks) {
        FontBlockCoverage fb;
        fb.name = b.name;
        fb.first = b.first;
        fb.last = b.last;
        for (u32 cp = b.first; cp <= b.last; ++cp)
            if (face.glyphFor(cp) != 0) ++fb.present;
        c.blocks.push_back(fb);
    }
    for (u32 cp = 0xFE70; cp <= 0xFEFC; ++cp)
        if (face.glyphFor(cp) != 0) ++c.arabicFormsPresent;
    return c;
}

// ---- the preview engine ---------------------------------------------------------------------------

EditorTextPreview& EditorTextPreview::shared() {
    static EditorTextPreview p;
    return p;
}

EditorTextPreview::EditorTextPreview() {
    UiTextConfig cfg;
    cfg.pageSize = 512;
    cfg.maxPages = 8;
    cfg.glyphBudget = 512;
    eng_.configure(cfg);
}

EditorTextPreview::~EditorTextPreview() {
    // The device may already be gone at process exit, so the textures are handed to whichever preview syncs next
    // on the same device instead of being destroyed here.
    for (const Page& p : pages_)
        if (p.tex) orphans().push_back(Orphan{p.tex, g_gpuFrame, device_});
    for (const Retired& r : retired_) orphans().push_back(Orphan{r.tex, r.frame, r.device});
}

bool EditorTextPreview::loadDoc(const UiFontSetDoc& doc, const std::string& contentRoot, std::string* err) {
    coverage_.clear();
    UiFontLoader loader;
    loader.open = [contentRoot](UiFontKind kind, const std::string& name, UiFontBlob& out) {
        return openFontFile(contentRoot, kind, name, out);
    };
    const bool ok = eng_.loadFontSet(doc, loader, err);
    root_ = contentRoot;
    loadedOnce_ = true;
    return ok && eng_.usable();
}

bool EditorTextPreview::ensureLoaded(const std::string& contentRoot) {
    if (loadedOnce_ && contentRoot == root_) return usable();
    UiFontSetDoc doc;
    std::string text;
    const std::string path = joinPath(contentRoot, "UI/Fonts.ocfonts");
    if (!contentRoot.empty() && fileExists(path) && readFileText(path, text) && UiFontSet::parse(text, doc, nullptr))
        return loadDoc(doc, contentRoot);
    UiFontSet::parse(uiDefaultFontSetText(), doc, nullptr);
    return loadDoc(doc, contentRoot);
}

void EditorTextPreview::beginFrame(u64 stamp) {
    if (stamp == stamp_) return;
    stamp_ = stamp;
    eng_.beginFrame();
}

std::vector<std::string> EditorTextPreview::familyNames() const {
    std::vector<std::string> v;
    const UiFontSet& fs = eng_.fontSet();
    for (u32 i = 0; i < fs.familyCount(); ++i) v.push_back(fs.familyName(i));
    return v;
}

const FontCoverage* EditorTextPreview::coverageOf(u16 faceSlot) {
    const UiFontFace* f = eng_.fontSet().face(faceSlot);
    if (!f) return nullptr;
    auto it = coverage_.find(faceSlot);
    if (it == coverage_.end()) it = coverage_.emplace(faceSlot, fsCoverage(*f)).first;
    return &it->second;
}

void EditorTextPreview::retire(u32 tex) {
    if (tex) retired_.push_back(Retired{tex, g_gpuFrame, device_});
}

// ---- the tab: state -------------------------------------------------------------------------------

FontSetEditor::FontSetEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

EditorTextPreview& FontSetEditor::preview() {
    if (!preview_) preview_ = std::make_unique<EditorTextPreview>();
    return *preview_;
}

void FontSetEditor::loadFromDisk() {
    std::string text;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "cannot read " + path_;
        return;
    }
    UiFontSetDoc d;
    std::string why;
    if (!UiFontSet::parse(text, d, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    doc_ = std::move(d);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    family_ = 0;
    history_.clear();
    ++version_;
}

std::string FontSetEditor::title() const {
    // No manual dirty marker: the host adds ImGuiWindowFlags_UnsavedDocument for a dirty editor.
    return std::filesystem::path(path_).filename().string() + "###fonts:" + path_;
}

bool FontSetEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    const std::string bad = fsValidate(doc_);
    if (!bad.empty()) {
        if (why) *why = bad;
        return false;
    }
    if (!writeFileTextAtomic(path_, UiFontSet::write(doc_))) {
        if (why) *why = "cannot write " + path_;
        return false;
    }
    dirty_ = false;
    EditorTextPreview::shared().invalidate();   // the layout editor's preview reads the saved file
    return true;
}

void FontSetEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[FontSetEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
    EditorTextPreview::shared().invalidate();
}

void FontSetEditor::selectFamily(i32 index) {
    if (doc_.families.empty()) { family_ = 0; return; }
    family_ = std::clamp(index, 0, static_cast<i32>(doc_.families.size()) - 1);
}

void FontSetEditor::pushUndo() { history_.push(doc_); }

void FontSetEditor::undo() {
    if (!history_.undo(doc_)) return;
    markDirty();
    selectFamily(family_);
}

void FontSetEditor::redo() {
    if (!history_.redo(doc_)) return;
    markDirty();
    selectFamily(family_);
}

i32 FontSetEditor::addFamily(std::string_view name) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsAddFamily(doc_, name);
    if (r < 0) { history_.cancelPush(); return -1; }
    family_ = r;
    markDirty();
    return r;
}

bool FontSetEditor::removeFamily(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsRemoveFamily(doc_, index)) { history_.cancelPush(); return false; }
    selectFamily(family_ >= index && family_ > 0 ? family_ - 1 : family_);
    markDirty();
    return true;
}

bool FontSetEditor::renameFamily(i32 index, std::string_view name) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsRenameFamily(doc_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool FontSetEditor::setDefault(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsSetDefault(doc_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 FontSetEditor::addFace(std::string_view file) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsAddFace(doc_, family_, file);
    if (r < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return r;
}

i32 FontSetEditor::addSystem(std::string_view file) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsAddSystem(doc_, family_, file);
    if (r < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return r;
}

i32 FontSetEditor::addBitmap() {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsAddBitmap(doc_, family_);
    if (r < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return r;
}

bool FontSetEditor::removeEntry(i32 entry) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsRemoveEntry(doc_, family_, entry)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 FontSetEditor::moveEntry(i32 entry, i32 delta) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsMoveEntry(doc_, family_, entry, delta);
    if (r < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return r;
}

bool FontSetEditor::setAttr(i32 entry, std::string_view key, std::string_view value) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsSetAttr(doc_, family_, entry, key, value)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool FontSetEditor::setLineGap(f32 multiplier) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsSetLineGap(doc_, family_, multiplier)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 FontSetEditor::addOverride(std::string_view lang, std::string_view from, std::string_view to) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 r = fsAddOverride(doc_, lang, from, to);
    if (r < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return r;
}

bool FontSetEditor::removeOverride(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!fsRemoveOverride(doc_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

std::unique_ptr<AssetEditor> makeFontSetEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocfonts") return nullptr;
    return std::make_unique<FontSetEditor>(path);
}

#if AVER_WITH_IMGUI

// =============================================================================== GPU =============

void EditorTextPreview::uploadDirty() {
    auto* dev = static_cast<rhi::IDevice*>(device_);
    if (!dev) return;
    rhi::IResourceFactory* res = dev->resources();
    if (!res) return;
    UiGlyphAtlas& at = eng_.atlas();
    const u32 n = at.pageCount();
    while (pages_.size() > n) {
        retire(pages_.back().tex);
        pages_.pop_back();
    }
    pages_.resize(n);
    const u32 sz = at.pageSize();
    std::vector<u8> rgba;
    for (u32 p = 0; p < n; ++p) {
        Page& pg = pages_[p];
        if (pg.tex && pg.size == sz && !at.pageDirty(p)) continue;
        const u8* src = at.pagePixels(p);
        if (!src) continue;
        // Straight white with the coverage as alpha: ImGui blends with straight alpha.
        rgba.resize(static_cast<usize>(sz) * sz * 4);
        for (usize i = 0; i < static_cast<usize>(sz) * sz; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = src[i];
        }
        const void* data = rgba.data();
        rhi::TextureDesc td;
        td.width = td.height = sz;
        td.format = rhi::Format::RGBA8Unorm;
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.initialData = &data;
        td.initialDataCount = 1;
        td.debugName = "Editor.TextPreview page";
        const rhi::TextureHandle tex = res->createTexture(td);
        if (!tex) continue;
        retire(pg.tex);
        pg.tex = tex;
        pg.size = sz;
        pg.imguiId = dev->uiTextureId(tex);
        at.markUploaded(p);
    }
}

void EditorTextPreview::syncGpu(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;
    device_ = dev;
    const i32 imguiFrame = ImGui::GetFrameCount();
    if (imguiFrame != syncedFrame_) {
        syncedFrame_ = imguiFrame;
        ++g_gpuFrame;
        if (rhi::IResourceFactory* res = dev->resources()) {
            // Textures drawn in the last few frames may still be in flight: destroy only the old ones.
            for (usize i = retired_.size(); i-- > 0;)
                if (g_gpuFrame - retired_[i].frame >= 4 && retired_[i].device == device_) {
                    res->destroyTexture(retired_[i].tex);
                    retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
                }
            auto& orph = orphans();
            for (usize i = orph.size(); i-- > 0;)
                if (g_gpuFrame - orph[i].frame >= 4 && orph[i].device == device_) {
                    res->destroyTexture(orph[i].tex);
                    orph.erase(orph.begin() + static_cast<std::ptrdiff_t>(i));
                }
        }
    }
    uploadDirty();
}

u64 EditorTextPreview::imguiTexture(u32 page) const { return page < pages_.size() ? pages_[page].imguiId : 0; }

void EditorTextPreview::paint(ImDrawList* dl, const UiDrawList& list, f32 originX, f32 originY, f32 zoom) {
    if (!dl) return;
    // Glyphs laid out after this frame's sync have dirty pages: upload them now so they show this frame.
    if (eng_.pendingUploads() > 0) uploadDirty();

    const auto at = [&](f32 x, f32 y) { return ImVec2(originX + x * zoom, originY + y * zoom); };
    const auto straight = [](u32 premul) {
        const u32 a = premul >> 24;
        if (a == 0) return 0u;
        const auto un = [&](u32 c) { return std::min(255u, (c * 255u + a / 2) / a); };
        return un(premul & 0xFF) | (un((premul >> 8) & 0xFF) << 8) | (un((premul >> 16) & 0xFF) << 16) | (a << 24);
    };
    const std::vector<UiVertex>& vs = list.vertices();
    const std::vector<u32>& ix = list.indices();

    for (usize layer = 0; layer < static_cast<usize>(UiLayer::Count); ++layer) {
        for (const UiDrawCmd& c : list.commands(static_cast<UiLayer>(layer))) {
            if (c.clip.empty()) continue;
            dl->PushClipRect(at(static_cast<f32>(c.clip.left), static_cast<f32>(c.clip.top)),
                             at(static_cast<f32>(c.clip.right), static_cast<f32>(c.clip.bottom)), true);
            // Every quad is six indices: (0 1 2)(0 2 3).
            for (u32 k = 0; k + 6 <= c.indexCount; k += 6) {
                const u32 base = c.indexOffset + k;
                if (base + 5 >= ix.size()) break;
                const UiVertex& v0 = vs[ix[base]];
                const UiVertex& v1 = vs[ix[base + 1]];
                const UiVertex& v2 = vs[ix[base + 2]];
                const UiVertex& v3 = vs[ix[base + 5]];
                const ImU32 col = straight(v0.rgba);
                if (c.texture == 0) {
                    dl->AddQuadFilled(at(v0.x, v0.y), at(v1.x, v1.y), at(v2.x, v2.y), at(v3.x, v3.y), col);
                } else if (c.texture & kUiAtlasTokenBit) {
                    const u64 id = imguiTexture(static_cast<u32>(c.texture & 0xFFFFu));
                    if (!id) continue;
                    dl->AddImageQuad(static_cast<ImTextureID>(id), at(v0.x, v0.y), at(v1.x, v1.y), at(v2.x, v2.y),
                                     at(v3.x, v3.y), ImVec2(v0.u, v0.v), ImVec2(v1.u, v1.v), ImVec2(v2.u, v2.v),
                                     ImVec2(v3.u, v3.v), col);
                }
                // Any other texture is an image the preview does not own: skipped.
            }
            dl->PopClipRect();
        }
    }
}

// =============================================================================== the tab ==========

namespace {

// Straight RGBA for the "tint by face" legend and glyphs.
const u32 kTints[7] = {uiRgba(240, 240, 240), uiRgba(255, 120, 100), uiRgba(110, 200, 120), uiRgba(110, 160, 255),
                       uiRgba(255, 210, 90),  uiRgba(220, 130, 240), uiRgba(90, 220, 230)};

bool editString(const char* id, std::string& s, f32 width) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputText(id, buf, sizeof buf)) return false;
    s = buf;
    return true;
}

ImVec4 vec(u32 rgba) {
    return ImVec4(static_cast<f32>(rgba & 0xFF) / 255.0f, static_cast<f32>((rgba >> 8) & 0xFF) / 255.0f,
                  static_cast<f32>((rgba >> 16) & 0xFF) / 255.0f, static_cast<f32>(rgba >> 24) / 255.0f);
}

const char* sourceLabel(UiFontEntry::Source s) {
    switch (s) {
        case UiFontEntry::Source::Project: return "FACE";
        case UiFontEntry::Source::System: return "SYSTEM";
        case UiFontEntry::Source::Bitmap: return "BITMAP";
    }
    return "?";
}

} // namespace

void FontSetEditor::touched() {
    if (!editing_) {
        editSnapshot_ = frameStart_;
        editing_ = true;
    }
    markDirty();
}

void FontSetEditor::commitEdit() {
    if (editing_ && !ImGui::IsAnyItemActive()) {
        history_.push(std::move(editSnapshot_));
        editing_ = false;
    }
}

void FontSetEditor::drawToolbar() {
    if (ImGui::Button("Save")) {
        std::string why;
        status_ = save(&why) ? std::string() : why;
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
    ImGui::SetNextItemWidth(90.0f * (ImGui::GetFontSize() / 16.0f));
    ImGui::InputTextWithHint("##lang", "language", language_, sizeof language_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The UI language to preview (ja, ar, zh-Hans ...): reorders chains with lang= and applies OVERRIDE rows");
    const std::string bad = fsValidate(doc_);
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", status_.c_str());
    } else if (!bad.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", bad.c_str());
    }
    if (!doc_.note.empty()) ImGui::TextDisabled("%s", doc_.note.c_str());
}

void FontSetEditor::drawFamilies(f32 dpi) {
    ImGui::TextUnformatted("Families");
    for (usize i = 0; i < doc_.families.size(); ++i) {
        std::string label = doc_.families[i].name;
        if (ieq(label, doc_.defaultFamily)) label += "   (default)";
        label += "##fam" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), static_cast<i32>(i) == family_)) selectFamily(static_cast<i32>(i));
    }
    ImGui::SetNextItemWidth(140.0f * dpi);
    ImGui::InputTextWithHint("##newfam", "new family", newFamily_, sizeof newFamily_);
    ImGui::SameLine();
    if (ImGui::Button("Add family")) {
        addFamily(newFamily_);
        newFamily_[0] = 0;
    }
    if (!familyOk(doc_, family_)) return;

    UiFontFamilyDef& fam = doc_.families[static_cast<usize>(family_)];
    if (ImGui::Button("Make default")) setDefault(family_);
    ImGui::SameLine();
    ImGui::BeginDisabled(doc_.families.size() <= 1);
    const bool removeIt = ImGui::Button("Remove family");
    ImGui::EndDisabled();
    if (removeIt) {
        removeFamily(family_);
        return;
    }
    {
        static char renameBuf[128];
        static i32 renameFor = -1;
        if (renameFor != family_) {
            std::snprintf(renameBuf, sizeof renameBuf, "%s", fam.name.c_str());
            renameFor = family_;
        }
        ImGui::SetNextItemWidth(140.0f * dpi);
        const bool enter = ImGui::InputText("##rename", renameBuf, sizeof renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Rename") || enter) {
            if (renameFamily(family_, renameBuf)) renameFor = -1;
        }
    }

    ImGui::Separator();
    ImGui::Text("Chain of '%s'", fam.name.c_str());
    ImGui::TextDisabled("A glyph comes from the first face with it: project faces, then OS fonts, then the bitmap font.");
    for (usize j = 0; j < fam.entries.size(); ++j) {
        UiFontEntry& e = fam.entries[j];
        ImGui::PushID(static_cast<int>(j));
        ImGui::Separator();
        ImGui::TextColored(vec(e.source == UiFontEntry::Source::Project ? uiRgba(130, 200, 255)
                               : e.source == UiFontEntry::Source::System ? uiRgba(200, 200, 120) : uiRgba(180, 180, 180)),
                           "%s", sourceLabel(e.source));
        ImGui::SameLine();
        if (e.source == UiFontEntry::Source::Bitmap) {
            ImGui::TextDisabled("the font lent with aver_ui_set_font");
        } else if (editString("##file", e.file, 220.0f * dpi)) {
            touched();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && moveEntry(static_cast<i32>(j), -1) >= 0) {
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && moveEntry(static_cast<i32>(j), 1) >= 0) {
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            removeEntry(static_cast<i32>(j));
            ImGui::PopID();
            break;
        }
        if (e.source != UiFontEntry::Source::Bitmap) {
            if (ImGui::Checkbox("Bold", &e.bold)) touched();
            ImGui::SameLine();
            if (ImGui::Checkbox("Italic", &e.italic)) touched();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f * dpi);
            if (editString("lang##l", e.lang, 70.0f * dpi)) touched();
            ImGui::SetNextItemWidth(80.0f * dpi);
            if (ImGui::DragFloat("scale", &e.scale, 0.01f, 0.1f, 4.0f, "%.2f")) touched();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * dpi);
            if (ImGui::DragFloat("dy", &e.dy, 0.05f, -16.0f, 16.0f, "%.2f")) touched();
            if (e.source == UiFontEntry::Source::Project) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f * dpi);
                if (ImGui::InputInt("index", &e.index, 0, 0)) {
                    e.index = std::max(0, e.index);
                    touched();
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(220.0f * dpi);
    ImGui::InputTextWithHint("##newface", "Fonts/Name.ttf or @exe/Name.ttf", newFace_, sizeof newFace_);
    ImGui::SameLine();
    if (ImGui::Button("Add face") && newFace_[0]) {
        addFace(newFace_);
        newFace_[0] = 0;
    }
    if (ImGui::Button("Add OS fonts (auto)")) addSystem("auto");
    ImGui::SameLine();
    if (ImGui::Button("Add bitmap font")) addBitmap();
    if (ImGui::DragFloat("Line gap x", &fam.lineGap, 0.01f, 0.0f, 4.0f, "%.2f")) {
        fam.lineGap = std::max(0.0f, fam.lineGap);
        touched();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Per-language replacements");
    for (usize i = 0; i < doc_.overrides.size(); ++i) {
        const UiFontOverride& o = doc_.overrides[i];
        ImGui::PushID(static_cast<int>(1000 + i));
        ImGui::Text("%s: %s -> %s", o.lang.c_str(), o.from.c_str(), o.to.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            removeOverride(static_cast<i32>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    std::vector<const char*> names;
    for (const UiFontFamilyDef& f : doc_.families) names.push_back(f.name.c_str());
    overrideFrom_ = std::clamp(overrideFrom_, 0, static_cast<int>(names.size()) - 1);
    overrideTo_ = std::clamp(overrideTo_, 0, static_cast<int>(names.size()) - 1);
    ImGui::SetNextItemWidth(60.0f * dpi);
    ImGui::InputText("##ovl", newOverrideLang_, sizeof newOverrideLang_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * dpi);
    ImGui::Combo("##ovf", &overrideFrom_, names.data(), static_cast<int>(names.size()));
    ImGui::SameLine();
    ImGui::TextUnformatted("->");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * dpi);
    ImGui::Combo("##ovt", &overrideTo_, names.data(), static_cast<int>(names.size()));
    ImGui::SameLine();
    if (ImGui::Button("Add")) addOverride(newOverrideLang_, names[static_cast<usize>(overrideFrom_)], names[static_cast<usize>(overrideTo_)]);
}

void FontSetEditor::drawPreview(Engine& e, f32 dpi) {
    (void)e;
    EditorTextPreview& pv = preview();
    UiTextEngine& eng = pv.engine();
    if (!pv.usable()) {
        ImGui::TextWrapped("No TrueType face could be opened from this set, so the game would draw with the baked bitmap font. "
                           "Check the face paths (relative to the project's Content folder) and that the files are valid.");
        for (const std::string& w : pv.warnings()) ImGui::TextDisabled("%s", w.c_str());
        return;
    }
    ImGui::SetNextItemWidth(140.0f * dpi);
    ImGui::SliderFloat("Size", &previewPx_, 8.0f, 64.0f, "%.0f px");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f * dpi);
    ImGui::SliderFloat("Wrap width", &wrapWidth_, 120.0f, 700.0f, "%.0f px");
    ImGui::Checkbox("Bold", &previewBold_);
    ImGui::SameLine();
    ImGui::Checkbox("Italic", &previewItalic_);
    ImGui::SameLine();
    ImGui::Checkbox("Tint by face", &tint_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * dpi);
    ImGui::Combo("Direction", &previewDir_, "Auto\0LTR\0RTL\0");

    const std::string family = familyOk(doc_, family_) ? doc_.families[static_cast<usize>(family_)].name : std::string();
    UiTextStyle st;
    st.family = family;
    st.px = previewPx_;
    st.bold = previewBold_;
    st.italic = previewItalic_;
    st.dir = previewDir_ == 1 ? UiTextDir::LTR : previewDir_ == 2 ? UiTextDir::RTL : UiTextDir::Auto;
    const u32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    struct Sample { const char* label; std::string text; bool wrap; };
    const std::vector<Sample> samples = {
        {"Latin", "The quick brown fox jumps over the lazy dog 0123456789", false},
        {"Kerning and ligature pairs", "AVAWAY Te To Ty We Yo fi fl ffi", false},
        {"Greek", "\U00000393\U000003B5\U000003B9\U000003AC \U000003C3\U000003BF\U000003C5 \U0000039A\U000003CC\U000003C3\U000003BC\U000003B5", false},
        {"Cyrillic", "\U0000041F\U00000440\U00000438\U00000432\U00000435\U00000442, \U0000043C\U00000438\U00000440", false},
        {"Arabic (shaped, right to left)",
         "\U00000645\U00000631\U0000062D\U00000628\U00000627 \U00000628\U00000627\U00000644\U00000639\U00000627\U00000644\U00000645", false},
        {"Hebrew", "\U000005E9\U000005DC\U000005D5\U000005DD \U000005E2\U000005D5\U000005DC\U000005DD", false},
        {"Japanese", "\U00003053\U00003093\U0000306B\U00003061\U0000306F\U00004E16\U0000754C", false},
        {"Chinese", "\U00004F60\U0000597D\U0000FF0C\U00004E16\U0000754C", false},
        {"Mixed direction", "Order 123 \U00000645\U00000631\U0000062D\U00000628\U00000627 and \U000005E9\U000005DC\U000005D5\U000005DD done", false},
        {"Combining marks", "e\U00000301 a\U00000308 o\U00000302 n\U00000303", false},
        {"Missing glyph", "before \U0010FFFD after", false},
        {"Wrapped paragraph",
         "The quick brown fox jumps over the lazy dog, then keeps running until the line has to wrap. "
         "\U00003053\U00003093\U0000306B\U00003061\U0000306F\U00004E16\U0000754C\U00003001\U00003053\U00003053\U00003067"
         "\U00003082\U00003067\U00006298\U00003082\U0000308A\U00003053\U00003093\U00003067\U00003044\U0000307E\U00003059\U00003002",
         true},
    };

    for (const Sample& s : samples) {
        ImGui::TextDisabled("%s", s.label);
        UiTextBox box;
        box.wrap = s.wrap;
        box.maxWidth = s.wrap ? wrapWidth_ : 0.0f;
        std::shared_ptr<const UiTextLayout> lay = eng.layout(s.text, st, box);
        if (!lay) continue;
        if (tint_) {
            auto copy = std::make_shared<UiTextLayout>(*lay);
            copy->palette.assign(8, 0);
            for (int i = 0; i < 7; ++i) copy->palette[static_cast<usize>(i + 1)] = kTints[i];
            for (UiGlyphRef& g : copy->glyphs)
                g.color = static_cast<u16>(1 + (g.face == kUiFaceBitmap ? 6u : (g.face % 6u)));
            lay = copy;
        }
        const f32 w = s.wrap ? wrapWidth_ : std::max(1.0f, lay->width);
        const f32 h = std::max(lay->height, previewPx_);
        ImGui::Dummy(ImVec2(w, h));
        const ImVec2 o = ImGui::GetItemRectMin();
        if (s.wrap) dl->AddRect(o, ImVec2(o.x + w, o.y + h), IM_COL32(120, 120, 120, 120));
        UiDrawList list;
        UiDrawListPainter painter(list, &eng, nullptr);
        painter.drawText(*lay, UiRect{0.0f, 0.0f, w, h}, UiTextAlign::Start, false, ink);
        pv.paint(dl, list, o.x, o.y, 1.0f);
    }

    if (tint_) {
        ImGui::Separator();
        ImGui::TextDisabled("Faces (colour = which face drew the glyph)");
        const UiFontSet& fs = eng.fontSet();
        for (u32 id = 0; id < fs.faceCount() && id < 6; ++id) {
            if (!fs.face(static_cast<u16>(id))) continue;
            ImGui::TextColored(vec(kTints[1 + (id % 6)]), "slot %u: %s", id, fs.faceName(static_cast<u16>(id)).c_str());
        }
    }
    for (const std::string& w : pv.warnings()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", w.c_str());
}

void FontSetEditor::drawCoverage() {
    EditorTextPreview& pv = preview();
    const UiFontSet& fs = pv.engine().fontSet();
    ImGui::TextDisabled("Faces open now. OS fonts open the first time a glyph needs them.");
    for (u32 id = 0; id < fs.faceCount(); ++id) {
        const UiFontEntry& en = fs.faceEntry(static_cast<u16>(id));
        const std::string& name = fs.faceName(static_cast<u16>(id));
        if (en.source == UiFontEntry::Source::Bitmap) {
            ImGui::TextDisabled("slot %u  BITMAP  the baked bitmap font", id);
            continue;
        }
        const FontCoverage* c = pv.coverageOf(static_cast<u16>(id));
        if (!c) {
            ImGui::TextDisabled("slot %u  %s  (not opened)", id, name.c_str());
            continue;
        }
        const std::string head = "slot " + std::to_string(id) + "  " + name + "##cov" + std::to_string(id);
        if (!ImGui::CollapsingHeader(head.c_str())) continue;
        ImGui::Text("%u glyphs, kerning: %s", c->glyphs,
                    c->kern == UiKernSource::Gpos ? "GPOS" : c->kern == UiKernSource::Kern ? "kern table" : "none");
        ImGui::Text("Arabic presentation forms (FE70-FEFC): %u of %u%s", c->arabicFormsPresent, c->arabicFormsTotal,
                    c->arabicFormsPresent == 0 ? "  (Arabic text falls back to the isolated letters)" : "");
        for (const FontBlockCoverage& b : c->blocks) {
            if (b.present == 0) continue;
            char overlay[48];
            std::snprintf(overlay, sizeof overlay, "%u / %u", b.present, b.total());
            ImGui::ProgressBar(static_cast<f32>(b.present) / static_cast<f32>(b.total()), ImVec2(160.0f, 0.0f), overlay);
            ImGui::SameLine();
            ImGui::TextUnformatted(b.name);
        }
    }
}

void FontSetEditor::drawAtlas(f32 dpi) {
    EditorTextPreview& pv = preview();
    UiTextEngine& eng = pv.engine();
    const UiTextStats s = eng.stats();
    ImGui::Text("%u pages, %u glyphs resident, %llu rasterised, %llu pages recycled, %llu overflows", s.pages, s.glyphsResident,
                static_cast<unsigned long long>(s.rasterTotal), static_cast<unsigned long long>(s.evictedPages),
                static_cast<unsigned long long>(s.overflow));
    ImGui::Text("layouts: %llu hits, %llu misses, %u cached, %llu incomplete", static_cast<unsigned long long>(s.layoutHits),
                static_cast<unsigned long long>(s.layoutMisses), s.cacheEntries, static_cast<unsigned long long>(s.incompleteLayouts));
    if (ImGui::Button("Clear atlas")) {
        eng.atlas().clear();
        eng.clearLayouts();
    }
    const UiGlyphAtlas& at = eng.atlas();
    const f32 edge = 128.0f * dpi;
    for (u32 p = 0; p < at.pageCount(); ++p) {
        const f32 used = static_cast<f32>(at.pageUsedPixels(p)) / static_cast<f32>(at.pageSize() * at.pageSize());
        ImGui::PushID(static_cast<int>(p));
        ImGui::Text("page %u", p);
        ImGui::ProgressBar(used, ImVec2(edge, 0.0f));
        if (const u64 id = pv.imguiTexture(p)) {
            ImGui::Image(static_cast<ImTextureID>(id), ImVec2(edge, edge));
        }
        ImGui::PopID();
    }
}

void FontSetEditor::draw(Engine& e) {
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    frameStart_ = doc_;
    const f32 dpi = ImGui::GetFontSize() / 16.0f;

    EditorTextPreview& pv = preview();
    const std::string root = fontContentRootOf(path_);
    if (previewVersion_ != version_) {
        std::string err;
        pv.loadDoc(doc_, root, &err);
        previewVersion_ = version_;
    }
    pv.engine().setLanguage(language_);
    pv.beginFrame(static_cast<u64>(ImGui::GetFrameCount()));
    pv.syncGpu(e);

    drawToolbar();
    ImGui::Separator();

    const f32 leftW = std::max(260.0f * dpi, ImGui::GetContentRegionAvail().x * 0.42f);
    if (ImGui::BeginChild("##fsleft", ImVec2(leftW, 0), true)) drawFamilies(dpi);
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##fsright", ImVec2(0, 0), true)) {
        if (ImGui::BeginTabBar("##fstabs")) {
            if (ImGui::BeginTabItem("Preview")) { drawPreview(e, dpi); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Coverage")) { drawCoverage(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Atlas")) { drawAtlas(dpi); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    commitEdit();
}

void FontSetEditor::resetLayout() {}

#else   // AVER_WITH_IMGUI

// The headless build still gets load/save/undo, the edits and the preview engine; only the window is absent.
void FontSetEditor::draw(Engine& e) { (void)e; }
void FontSetEditor::resetLayout() {}

#endif  // AVER_WITH_IMGUI

} // namespace aver::editor
