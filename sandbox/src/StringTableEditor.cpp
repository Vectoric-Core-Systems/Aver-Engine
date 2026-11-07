// The localization string-table editor tab. See the header for the SandboxApp hook and the empty-means-missing rule.
#include "StringTableEditor.hpp"
#include "EditorChrome.hpp"

#include "EditorKeybinds.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcStrings.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <set>

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "imgui.h"
#endif

namespace fs = std::filesystem;

namespace aver::editor {
namespace {

StringTableEditorHooks g_hooks;

std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string pathStr(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
fs::path toPath(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string extOf(const std::string& path) { return lowerAscii(pathStr(toPath(path).extension())); }

} // namespace

void setStringTableEditorHooks(StringTableEditorHooks hooks) { g_hooks = std::move(hooks); }

StringTableEditor::StringTableEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

std::string StringTableEditor::defaultTag() const {
    const std::string d = g_hooks.defaultCulture ? fmt::normalizeCulture(g_hooks.defaultCulture()) : std::string();
    return d.empty() ? std::string("en") : d;
}

std::string StringTableEditor::title() const {
    return table_ + " (strings)###stringtable:" + path_;
}

bool StringTableEditor::dirty() const {
    return std::any_of(culDirty_.begin(), culDirty_.end(), [](char c) { return c != 0; });
}

bool StringTableEditor::isMissing(usize cul, usize k) const {
    return cul == 0 ? grid_.present[cul][k] == 0 : grid_.text[cul][k].empty();
}

usize StringTableEditor::missingCount() const {
    usize n = 0;
    for (usize c = 0; c < grid_.cultures.size(); ++c)
        for (usize k = 0; k < grid_.keys.size(); ++k)
            if (isMissing(c, k)) ++n;
    return n;
}

bool StringTableEditor::refuse(std::string why) {
    editError_ = std::move(why);
    return false;
}

// ---- load / save ------------------------------------------------------------------------------------

void StringTableEditor::loadFromDisk() {
    loaded_ = false;
    loadError_.clear();
    editError_.clear();
    diagnostics_.clear();
    grid_ = Grid{};
    files_.clear();
    culDirty_.clear();
    history_.clear();
    changedOnDisk_ = false;

    const fs::path p = toPath(path_);
    table_ = pathStr(p.stem());
    if (!fmt::validLocTable(table_)) {
        loadError_ = "'" + table_ + "' is not a valid table name (letters, digits and underscores)";
        return;
    }
    locDir_ = pathStr(p.parent_path().parent_path());

    // Culture folders.
    std::map<std::string, std::string> folderOf;     // tag -> folder name
    std::error_code ec;
    for (fs::directory_iterator it(toPath(locDir_), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string folder = pathStr(it->path().filename());
        const std::string tag = fmt::normalizeCulture(folder);
        if (tag.empty()) { diagnostics_.push_back(folder + ": not a culture tag; ignored"); continue; }
        folderOf.emplace(tag, folder);
    }
    const std::string def = defaultTag();
    std::vector<std::string> order = {def};
    for (const auto& [tag, folder] : folderOf) { (void)folder; if (tag != def) order.push_back(tag); }

    std::vector<fmt::OcStringsData> data(order.size());
    for (usize c = 0; c < order.size(); ++c) {
        const std::string& tag = order[c];
        CultureFile f;
        const auto fo = folderOf.find(tag);
        const std::string dir = locDir_ + "/" + (fo != folderOf.end() ? fo->second : tag);
        const std::string ocs = dir + "/" + table_ + ".ocstrings", csv = dir + "/" + table_ + ".csv";
        const bool hasOcs = fileExists(ocs), hasCsv = fileExists(csv);
        if (hasOcs && hasCsv) diagnostics_.push_back(tag + ": " + table_ + ".ocstrings and .csv both exist; the .ocstrings wins");
        f.csv = !hasOcs && hasCsv;
        f.path = f.csv ? csv : ocs;
        f.exists = hasOcs || hasCsv;
        if (f.exists) {
            readFileText(f.path, f.original);
            std::vector<std::string> diag;
            std::string err;
            if (fmt::loadStrings(f.path, data[c], &diag, &err)) {
                for (const std::string& m : diag) diagnostics_.push_back(tag + ": " + m);
            } else {
                f.broken = true;
                diagnostics_.push_back(tag + ": " + err + " (this culture is left alone)");
            }
        }
        files_[tag] = std::move(f);
    }

    // Keys: the default culture's order first, then the others' new ones.
    std::set<std::string> seen;
    for (const fmt::OcStringsData& d : data)
        for (const fmt::OcStringEntry& e : d.entries)
            if (seen.insert(e.key).second) grid_.keys.push_back(e.key);
    const usize nk = grid_.keys.size();
    grid_.cultures = order;
    grid_.text.assign(order.size(), std::vector<std::string>(nk));
    grid_.note.assign(order.size(), std::vector<std::string>(nk));
    grid_.present.assign(order.size(), std::vector<char>(nk, 0));
    std::map<std::string, usize> keyIndex;
    for (usize k = 0; k < nk; ++k) keyIndex[grid_.keys[k]] = k;
    for (usize c = 0; c < order.size(); ++c) {
        for (const fmt::OcStringEntry& e : data[c].entries) {
            const usize k = keyIndex[e.key];
            grid_.text[c][k] = e.text;
            grid_.note[c][k] = e.note;
            grid_.present[c][k] = 1;
        }
    }
    culDirty_.assign(order.size(), 0);
    loaded_ = true;
}

bool StringTableEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: " + loadError_;
        return false;
    }
    std::string failures;
    for (usize c = 0; c < grid_.cultures.size(); ++c) {
        if (!culDirty_[c]) continue;
        CultureFile& f = files_[grid_.cultures[c]];
        if (f.broken) { failures += grid_.cultures[c] + ": the file did not parse and is left alone; "; continue; }
        fmt::OcStringsData d;
        d.culture = grid_.cultures[c];
        for (usize k = 0; k < grid_.keys.size(); ++k) {
            if (!grid_.present[c][k]) continue;
            if (c != 0 && grid_.text[c][k].empty()) continue;           // untranslated: not written, the runtime falls back
            d.entries.push_back({grid_.keys[k], grid_.text[c][k], grid_.note[c][k]});
        }
        const std::string text = f.csv ? fmt::writeStringsCsv(d) : fmt::writeOcstrings(d, f.original);
        std::error_code ec;
        fs::create_directories(toPath(f.path).parent_path(), ec);
        if (!writeFileTextAtomic(f.path, text)) { failures += "could not write " + f.path + "; "; continue; }
        f.original = text;
        f.exists = true;
        culDirty_[c] = 0;
    }
    if (!failures.empty()) {
        if (why) *why = failures;
        return false;
    }
    changedOnDisk_ = false;
    return true;
}

void StringTableEditor::onFileChanged() {
    if (dirty()) {
        changedOnDisk_ = true;
        AVER_WARN("[StringTableEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

// ---- undo -------------------------------------------------------------------------------------------

void StringTableEditor::pushUndo() { history_.push(grid_); }

void StringTableEditor::undo() {
    if (!history_.undo(grid_)) return;
    touchAll();     // which files differ from disk is not tracked through a snapshot; rewriting an unchanged one is a no-op merge
}

void StringTableEditor::redo() {
    if (!history_.redo(grid_)) return;
    touchAll();
}

// ---- edits ------------------------------------------------------------------------------------------

bool StringTableEditor::addKey(std::string key) {
    if (!loaded_) return false;
    editError_.clear();
    if (!fmt::validLocKey(key)) return refuse("'" + key + "' is not a valid key (letters, digits, '.', '-' and '_')");
    if (std::find(grid_.keys.begin(), grid_.keys.end(), key) != grid_.keys.end()) return refuse("the key '" + key + "' already exists");
    pushUndo();
    grid_.keys.push_back(std::move(key));
    for (usize c = 0; c < grid_.cultures.size(); ++c) {
        grid_.text[c].emplace_back();
        grid_.note[c].emplace_back();
        grid_.present[c].push_back(c == 0 ? 1 : 0);
    }
    markDirty(0);
    return true;
}

bool StringTableEditor::removeKey(usize k) {
    if (!loaded_ || k >= grid_.keys.size()) return false;
    editError_.clear();
    pushUndo();
    for (usize c = 0; c < grid_.cultures.size(); ++c) {
        if (grid_.present[c][k]) markDirty(c);
        grid_.text[c].erase(grid_.text[c].begin() + static_cast<std::ptrdiff_t>(k));
        grid_.note[c].erase(grid_.note[c].begin() + static_cast<std::ptrdiff_t>(k));
        grid_.present[c].erase(grid_.present[c].begin() + static_cast<std::ptrdiff_t>(k));
    }
    grid_.keys.erase(grid_.keys.begin() + static_cast<std::ptrdiff_t>(k));
    return true;
}

bool StringTableEditor::renameKey(usize k, std::string newKey) {
    if (!loaded_ || k >= grid_.keys.size()) return false;
    editError_.clear();
    if (!fmt::validLocKey(newKey)) return refuse("'" + newKey + "' is not a valid key (letters, digits, '.', '-' and '_')");
    if (grid_.keys[k] == newKey) return true;
    if (std::find(grid_.keys.begin(), grid_.keys.end(), newKey) != grid_.keys.end()) return refuse("the key '" + newKey + "' already exists");
    pushUndo();
    grid_.keys[k] = std::move(newKey);
    for (usize c = 0; c < grid_.cultures.size(); ++c) if (grid_.present[c][k]) markDirty(c);
    return true;
}

bool StringTableEditor::setText(usize cul, usize k, std::string text) {
    if (!loaded_ || cul >= grid_.cultures.size() || k >= grid_.keys.size()) return false;
    editError_.clear();
    const bool present = cul == 0 || !text.empty();
    if (grid_.text[cul][k] == text && grid_.present[cul][k] == (present ? 1 : 0)) return true;
    pushUndo();
    grid_.text[cul][k] = std::move(text);
    grid_.present[cul][k] = present ? 1 : 0;
    markDirty(cul);
    return true;
}

bool StringTableEditor::setNote(usize k, std::string note) {
    if (!loaded_ || k >= grid_.keys.size()) return false;
    editError_.clear();
    if (grid_.note[0][k] == note) return true;
    pushUndo();
    grid_.note[0][k] = std::move(note);
    markDirty(0);
    return true;
}

bool StringTableEditor::addCulture(std::string tag) {
    if (!loaded_) return false;
    editError_.clear();
    tag = fmt::normalizeCulture(tag);
    if (tag.empty()) return refuse("not a culture tag (for example de, pt-BR, zh-Hans)");
    if (std::find(grid_.cultures.begin(), grid_.cultures.end(), tag) != grid_.cultures.end()) return refuse("the culture '" + tag + "' already exists");
    std::error_code ec;
    fs::create_directories(toPath(locDir_ + "/" + tag), ec);
    if (ec) return refuse("could not create the folder for '" + tag + "'");
    pushUndo();
    // After the default, in sorted order among the rest.
    usize at = 1;
    while (at < grid_.cultures.size() && grid_.cultures[at] < tag) ++at;
    const usize nk = grid_.keys.size();
    grid_.cultures.insert(grid_.cultures.begin() + static_cast<std::ptrdiff_t>(at), tag);
    grid_.text.insert(grid_.text.begin() + static_cast<std::ptrdiff_t>(at), std::vector<std::string>(nk));
    grid_.note.insert(grid_.note.begin() + static_cast<std::ptrdiff_t>(at), std::vector<std::string>(nk));
    grid_.present.insert(grid_.present.begin() + static_cast<std::ptrdiff_t>(at), std::vector<char>(nk, 0));
    culDirty_.insert(culDirty_.begin() + static_cast<std::ptrdiff_t>(at), 1);
    CultureFile f;
    f.path = locDir_ + "/" + tag + "/" + table_ + ".ocstrings";
    files_[tag] = std::move(f);
    return true;
}

// ---- gather / report --------------------------------------------------------------------------------

bool StringTableEditor::runReport() {
    if (!loaded_) return false;
    editError_.clear();
    if (dirty()) return refuse("save this tab first: the report reads the files on disk");
    lastRun_ = runLocReport(pathStr(toPath(locDir_).parent_path()), defaultTag());
    hasRun_ = true;
    return true;
}

bool StringTableEditor::runGather() {
    if (!loaded_) return false;
    editError_.clear();
    if (dirty()) return refuse("save this tab first: Gather merges into the files on disk");
    lastRun_ = runLocGather(pathStr(toPath(locDir_).parent_path()), defaultTag());
    hasRun_ = true;
    loadFromDisk();         // the default culture's tables may have grown
    return true;
}

// ---- the window -------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

namespace {
const ImVec4 kRed(1.0f, 0.45f, 0.4f, 1.0f);
}

void StringTableEditor::drawToolbar() {
    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[StringTableEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Show missing only", &missingOnly_);

    char kb[128];
    std::snprintf(kb, sizeof kb, "%s", newKey_.c_str());
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputTextWithHint("##newkey", "new key, e.g. Menu.Play", kb, sizeof kb)) newKey_ = kb;
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD " Add key") && addKey(newKey_)) {
        selectedKey_ = static_cast<int>(grid_.keys.size()) - 1;
        newKey_.clear();
    }
    const bool haveKey = selectedKey_ >= 0 && static_cast<usize>(selectedKey_) < grid_.keys.size();
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveKey);
    if (ImGui::Button(ICON_DELETE " Delete key") && removeKey(static_cast<usize>(selectedKey_))) selectedKey_ = -1;
    ImGui::SameLine();
    char rb[128];
    std::snprintf(rb, sizeof rb, "%s", haveKey ? grid_.keys[static_cast<usize>(selectedKey_)].c_str() : "");
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputText("##renamekey", rb, sizeof rb, ImGuiInputTextFlags_EnterReturnsTrue) && haveKey)
        renameKey(static_cast<usize>(selectedKey_), rb);
    ImGui::EndDisabled();

    char cb[32];
    std::snprintf(cb, sizeof cb, "%s", newCulture_.c_str());
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputTextWithHint("##newculture", "de, pt-BR", cb, sizeof cb)) newCulture_ = cb;
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD " Add culture") && addCulture(newCulture_)) newCulture_.clear();
    ImGui::SameLine();
    if (ImGui::Button("Gather")) runGather();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds every key the project uses to the default culture's tables (text = the inline default).");
    ImGui::SameLine();
    if (ImGui::Button("Report")) runReport();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists missing, unused, conflicting and untranslated keys across the project.");
    if (!editError_.empty()) ImGui::TextColored(kRed, "%s", editError_.c_str());
}

void StringTableEditor::drawGrid() {
    const int nc = static_cast<int>(grid_.cultures.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##strings", nc + 2, flags, ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 6.0f))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    for (int c = 0; c < nc; ++c) {
        const std::string label = grid_.cultures[static_cast<usize>(c)] + (c == 0 ? " (default)" : "");
        ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 240.0f);
    }
    ImGui::TableHeadersRow();

    for (usize k = 0; k < grid_.keys.size(); ++k) {
        bool anyMissing = false;
        for (usize c = 0; c < grid_.cultures.size(); ++c) anyMissing = anyMissing || isMissing(c, k);
        if (missingOnly_ && !anyMissing) continue;
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(k));
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable(grid_.keys[k].c_str(), selectedKey_ == static_cast<int>(k))) selectedKey_ = static_cast<int>(k);

        const auto edit = [&](const char* id, std::string current, auto&& apply) {
            char buf[2048];
            std::snprintf(buf, sizeof buf, "%s", current.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(id, buf, sizeof buf)) {
                // One undo step per typing burst.
                if (!cellUndone_) { pushUndo(); cellUndone_ = true; }
                apply(std::string(buf));
            }
            if (ImGui::IsItemDeactivated()) cellUndone_ = false;
        };
        ImGui::TableSetColumnIndex(1);
        edit("##note", grid_.note[0][k], [&](std::string v) { grid_.note[0][k] = std::move(v); markDirty(0); });
        for (usize c = 0; c < grid_.cultures.size(); ++c) {
            ImGui::TableSetColumnIndex(static_cast<int>(c) + 2);
            if (isMissing(c, k)) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(150, 110, 30, 110));
            edit(("##t" + std::to_string(c)).c_str(), grid_.text[c][k], [&](std::string v) {
                grid_.present[c][k] = (c == 0 || !v.empty()) ? 1 : 0;
                grid_.text[c][k] = std::move(v);
                markDirty(c);
            });
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void StringTableEditor::drawReport() {
    if (!hasRun_) return;
    const fmt::LocReport& r = lastRun_.report;
    if (!::aver::editor::sectionHeader(("Report: " + std::to_string(r.missing.size()) + " missing, " + std::to_string(r.unused.size()) +
                                  " unused, " + std::to_string(r.conflicts.size()) + " conflicting, " +
                                  std::to_string(r.untranslated.size()) + " untranslated###report").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (lastRun_.added > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Gather added %zu key(s) to %zu file(s).", lastRun_.added, lastRun_.filesWritten.size());
    if (ImGui::Button("Copy as text")) ImGui::SetClipboardText(lastRun_.text.c_str());
    ImGui::BeginChild("##reportlist", ImVec2(0, 140), true);
    for (const auto& m : r.missing) {
        ImGui::Text("missing [%s] %s:%s  %s", m.culture.c_str(), m.table.c_str(), m.key.c_str(), m.source.c_str());
        if (g_hooks.openAsset) {
            ImGui::SameLine();
            const std::string file = locDir_ + "/" + m.culture + "/" + m.table + ".ocstrings";
            if (ImGui::SmallButton(("Open table##" + m.culture + m.table + m.key).c_str())) g_hooks.openAsset(file);
        }
    }
    for (const auto& u : r.unused) ImGui::TextDisabled("unused [%s] %s:%s", u.culture.c_str(), u.table.c_str(), u.key.c_str());
    for (const auto& c : r.conflicts) ImGui::TextColored(kRed, "conflict %s:%s has %zu different defaults", c.table.c_str(), c.key.c_str(), c.defaults.size());
    for (const auto& u : r.untranslated) ImGui::TextDisabled("untranslated [%s] %s:%s", u.culture.c_str(), u.table.c_str(), u.key.c_str());
    for (const std::string& d : lastRun_.diagnostics) ImGui::TextColored(kRed, "%s", d.c_str());
    ImGui::EndChild();
}

void StringTableEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This table could not be opened: %s", loadError_.c_str());
        return;
    }
    if (changedOnDisk_) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ICON_WARNING " A file of this table changed on disk; your edits here are kept.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload (discard my edits)")) loadFromDisk();
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep mine")) changedOnDisk_ = false;
    }
    drawToolbar();
    drawGrid();
    ImGui::TextDisabled("%zu key(s), %zu culture(s), %zu cell(s) not translated", grid_.keys.size(), grid_.cultures.size(), missingCount());
    for (const std::string& d : diagnostics_) ImGui::TextColored(kRed, "%s", d.c_str());
    drawReport();
}

#else   // AVER_WITH_IMGUI

void StringTableEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeStringTableEditor(const std::string& path) {
    const std::string ext = extOf(path);
    if (ext != ".ocstrings" && ext != ".csv") return nullptr;
    // A .csv is a string table only directly under Localization/<culture>/.
    if (ext == ".csv" && lowerAscii(pathStr(toPath(path).parent_path().parent_path().filename())) != "localization") return nullptr;
    return std::make_unique<StringTableEditor>(path);
}

} // namespace aver::editor
