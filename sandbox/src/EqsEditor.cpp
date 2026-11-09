// The environment-query editor tab. See the header for the wiring.

#include "EqsEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Log.hpp"
#include "aver/synapse/EqsAsset.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "AiEditorParts.hpp"

#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/synapse/SynapseAi.hpp"
#  define AVER_EQSE_AI 1
#  if AVER_WITH_SYNAPSE_AI
#    include "aver/synapse/synapse_ai_abi.h"   // the DLL's single SynapseAi, for the tab itself
#    define AVER_EQSE_LIVE 1
#  endif
#endif

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using namespace synapse;

namespace {

EqsSelectionFn& selectionSource() {
    static EqsSelectionFn fn;
    return fn;
}

bool optIn(const EqsQuery& q, usize i) { return i < q.options.size(); }

} // namespace

void setEqsSelectionSource(EqsSelectionFn fn) { selectionSource() = std::move(fn); }

bool eqsWriteStarter(const std::string& path, std::string* why) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = "cannot write " + path;
        return false;
    }
    f << "# Environment query. A generator makes candidate points; tests filter and score them.\n"
         "# Best-scoring items come first. See docs/AI_QUERIES_SMARTOBJECTS_STATETREE.md.\n"
      << writeEqsQuery(eqsStarter());
    if (!f) {
        if (why) *why = "write failed for " + path;
        return false;
    }
    return true;
}

// ---- the tab: load, save, undo -----------------------------------------------------------------------

EqsEditor::EqsEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

EqsEditor::~EqsEditor() = default;

void EqsEditor::loadFromDisk() {
    EqsQuery q;
    std::string why;
    if (!loadEqsQuery(path_, q, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    query_ = std::move(q);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    option_ = 0;
    history_.clear();
}

std::string EqsEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "###eqse:" + path_;
}

std::vector<std::string> EqsEditor::warnings() const { return validateEqsQuery(query_); }

bool EqsEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    EqsQuery check;
    std::string parseWhy;
    if (!parseEqsQuery(writeEqsQuery(query_), path_, check, &parseWhy)) {
        if (why) *why = "the query would not read back: " + parseWhy;
        return false;
    }
    if (!saveEqsQuery(path_, query_, why)) return false;
    dirty_ = false;
#if AVER_EQSE_LIVE
    // The DLL's single SynapseAi owns the query cache: re-read the file under both spellings of its
    // path. Runs already started keep their copy.
    synapse::EqsSystem& eqs = static_cast<synapse::SynapseAi*>(aver_syn_ai_instance())->eqs();
    eqs.reloadQuery(path_);
    if (const std::string rel = aiRelativeAssetPath(path_); !rel.empty()) eqs.reloadQuery(rel);
#endif
    return true;
}

void EqsEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[EqsEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void EqsEditor::undo() {
    if (!history_.undo(query_)) return;
    dirty_ = true;
    if (option_ >= static_cast<i32>(query_.options.size())) option_ = query_.options.empty() ? 0 : static_cast<i32>(query_.options.size()) - 1;
}

void EqsEditor::redo() {
    if (!history_.redo(query_)) return;
    dirty_ = true;
    if (option_ >= static_cast<i32>(query_.options.size())) option_ = query_.options.empty() ? 0 : static_cast<i32>(query_.options.size()) - 1;
}

// ---- header edits -----------------------------------------------------------------------------------

bool EqsEditor::setDescription(const std::string& text) {
    if (!loaded_ || query_.desc == text) return false;
    pushUndo();
    query_.desc = text;
    dirty_ = true;
    return true;
}

bool EqsEditor::setResultMode(EqsResultMode mode, u32 n, f32 percent) {
    if (!loaded_) return false;
    if (mode == EqsResultMode::Top) n = std::clamp<u32>(n, 1, kEqsMaxItems);
    if (mode == EqsResultMode::Percent || mode == EqsResultMode::RandomPercent) percent = std::clamp(percent, 0.1f, 100.0f);
    if (query_.mode == mode && query_.modeN == n && query_.modeP == percent) return false;
    pushUndo();
    query_.mode = mode;
    query_.modeN = mode == EqsResultMode::Top ? n : 0;
    query_.modeP = (mode == EqsResultMode::Percent || mode == EqsResultMode::RandomPercent) ? percent : 0.0f;
    dirty_ = true;
    return true;
}

bool EqsEditor::setRunAll(bool on) {
    if (!loaded_ || query_.runAll == on) return false;
    pushUndo();
    query_.runAll = on;
    dirty_ = true;
    return true;
}

bool EqsEditor::setOutput(const EqsQuery::Output& out) {
    if (!loaded_) return false;
    if (query_.out.pos == out.pos && query_.out.entity == out.entity && query_.out.score == out.score && query_.out.count == out.count) return false;
    pushUndo();
    query_.out = out;
    dirty_ = true;
    return true;
}

bool EqsEditor::addContext(const std::string& name, const std::string& key) {
    if (!loaded_ || query_.contexts.size() >= kEqsMaxContexts || name.empty() || key.empty()) return false;
    for (const auto& c : query_.contexts)
        if (c.first == name) return false;
    if (name == "querier" || name == "seen" || name == "point") return false;
    pushUndo();
    query_.contexts.emplace_back(name, key);
    dirty_ = true;
    return true;
}

bool EqsEditor::removeContext(usize index) {
    if (!loaded_ || index >= query_.contexts.size()) return false;
    pushUndo();
    query_.contexts.erase(query_.contexts.begin() + static_cast<isize>(index));
    dirty_ = true;
    return true;
}

// ---- options and tests ------------------------------------------------------------------------------

bool EqsEditor::addOption() {
    if (!loaded_ || query_.options.size() >= kEqsMaxOptions) return false;
    pushUndo();
    EqsOption o;
    o.gen = eqsDefaultGenerator(EqsGen::Ring);
    query_.options.push_back(std::move(o));
    option_ = static_cast<i32>(query_.options.size()) - 1;
    dirty_ = true;
    return true;
}

bool EqsEditor::removeOption(usize index) {
    if (!loaded_ || !optIn(query_, index) || query_.options.size() <= 1) return false;   // a query needs an option
    pushUndo();
    query_.options.erase(query_.options.begin() + static_cast<isize>(index));
    option_ = std::min<i32>(option_, static_cast<i32>(query_.options.size()) - 1);
    dirty_ = true;
    return true;
}

bool EqsEditor::moveOption(usize index, i32 delta) {
    if (!loaded_ || !optIn(query_, index)) return false;
    const i32 to = static_cast<i32>(index) + delta;
    if (delta == 0 || to < 0 || static_cast<usize>(to) >= query_.options.size()) return false;
    pushUndo();
    std::swap(query_.options[index], query_.options[static_cast<usize>(to)]);
    if (option_ == static_cast<i32>(index)) option_ = to;
    else if (option_ == to) option_ = static_cast<i32>(index);
    dirty_ = true;
    return true;
}

bool EqsEditor::setGeneratorKind(usize option, EqsGen kind) {
    if (!loaded_ || !optIn(query_, option) || query_.options[option].gen.kind == kind) return false;
    pushUndo();
    EqsGenerator& g = query_.options[option].gen;
    EqsGenerator fresh = eqsDefaultGenerator(kind);
    fresh.centre = g.centre;
    fresh.project = g.project;
    fresh.zOffset = g.zOffset;
    fresh.maxItems = g.maxItems;
    if (kind == EqsGen::Actors) fresh.namePrefix = "Actor";   // exactly one filter is required
    if (kind == EqsGen::Points) fresh.points = {Vec3{0, 0, 0}};
    g = std::move(fresh);
    dirty_ = true;
    return true;
}

bool EqsEditor::setGenerator(usize option, const EqsGenerator& g) {
    if (!loaded_ || !optIn(query_, option)) return false;
    pushUndo();
    query_.options[option].gen = g;
    dirty_ = true;
    return true;
}

bool EqsEditor::addTest(usize option, EqsTestKind kind) {
    if (!loaded_ || !optIn(query_, option) || query_.options[option].tests.size() >= kEqsMaxTests) return false;
    pushUndo();
    EqsTest t = eqsDefaultTest(kind);
    if (kind == EqsTestKind::Tag) t.tags.v = {"Tag.Name"};
    if (kind == EqsTestKind::Custom) { t.name = "MyTest"; t.custom = "MyTest"; }
    query_.options[option].tests.push_back(std::move(t));
    dirty_ = true;
    return true;
}

bool EqsEditor::removeTest(usize option, usize test) {
    if (!loaded_ || !optIn(query_, option) || test >= query_.options[option].tests.size()) return false;
    pushUndo();
    auto& tests = query_.options[option].tests;
    tests.erase(tests.begin() + static_cast<isize>(test));
    dirty_ = true;
    return true;
}

bool EqsEditor::moveTest(usize option, usize test, i32 delta) {
    if (!loaded_ || !optIn(query_, option) || test >= query_.options[option].tests.size()) return false;
    const i32 to = static_cast<i32>(test) + delta;
    auto& tests = query_.options[option].tests;
    if (delta == 0 || to < 0 || static_cast<usize>(to) >= tests.size()) return false;
    pushUndo();
    std::swap(tests[test], tests[static_cast<usize>(to)]);
    dirty_ = true;
    return true;
}

bool EqsEditor::setTestKind(usize option, usize test, EqsTestKind kind) {
    if (!loaded_ || !optIn(query_, option) || test >= query_.options[option].tests.size()) return false;
    EqsTest& t = query_.options[option].tests[test];
    if (t.kind == kind) return false;
    pushUndo();
    EqsTest fresh = eqsDefaultTest(kind);
    fresh.purpose = t.purpose;
    fresh.weight = t.weight;
    fresh.curve = t.curve;
    if (kind == EqsTestKind::Tag) fresh.tags.v = {"Tag.Name"};
    if (kind == EqsTestKind::Custom) { fresh.name = "MyTest"; fresh.custom = "MyTest"; }
    t = std::move(fresh);
    dirty_ = true;
    return true;
}

bool EqsEditor::setTest(usize option, usize test, const EqsTest& t) {
    if (!loaded_ || !optIn(query_, option) || test >= query_.options[option].tests.size()) return false;
    pushUndo();
    query_.options[option].tests[test] = t;
    dirty_ = true;
    return true;
}

// ---- preview ----------------------------------------------------------------------------------------

bool EqsEditor::runPreview(synapse::SynapseAi& ai, u32 querier, u32 target) {
#if AVER_EQSE_AI
    previewMessage_.clear();
    if (!loaded_) return false;
    EqsQuery q;
    std::string why;
    if (!parseEqsQuery(writeEqsQuery(query_), path_, q, &why)) {
        previewMessage_ = why;
        return false;
    }
    if (previewHandle_ != 0) ai.eqs().release(previewHandle_);
    previewHandle_ = 0;
    const u32 id = ai.eqs().registerQuery("preview:" + path_, q);
    EqsRunParams p;
    p.querier = querier;
    p.target = target;
    p.runId = ++runCounter_;
    const u32 handle = ai.eqs().runSync(id, p);
    if (handle == 0) {
        previewMessage_ = "the run was refused";
        return false;
    }
    previewHandle_ = handle;
    ai.eqs().pinNewestDebugRun();
    if (const EqsResult* r = ai.eqs().result(handle); r && r->status == EqsResult::Status::Failed) previewMessage_ = r->reason;
    return true;
#else
    (void)ai; (void)querier; (void)target;
    previewMessage_ = "needs a build with the AI scene module";
    return false;
#endif
}

std::unique_ptr<AssetEditor> makeEqsEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".oceqs") return nullptr;
    return std::make_unique<EqsEditor>(path);
}

// ================================================================================= drawing ==========

#if AVER_WITH_IMGUI

namespace {

constexpr f32 kDefaultFormFraction = 0.62f;
constexpr const char* kPrefSplit = "eqsEditor.split";

const char* const kGenNames[] = {"grid", "ring", "around", "nav", "actors", "slots", "points"};
const char* const kTestNames[] = {"distance", "dot", "trace", "path", "tag", "random", "custom"};
const char* const kPurposeNames[] = {"filter", "score", "both"};
const char* const kCurveNames[] = {"linear", "inverse", "square", "invsquare", "sqrt", "step"};
const char* const kProjectNames[] = {"none", "nav", "ground"};
const char* const kResultNames[] = {"all", "best", "top", "percent", "randompercent"};
const char* const kMatchNames[] = {"any", "all", "none"};

std::string fmtG(const Vec3& p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%g %g %g", static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
    return buf;
}

// A text field that applies on deactivation. Returns true when `out` changed.
bool textField(const char* label, std::string& out, f32 width = 0.0f) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s", out.c_str());
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    ImGui::InputText(label, buf, sizeof buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && out != buf) {
        out = buf;
        return true;
    }
    return false;
}

} // namespace

void EqsEditor::drawHeader() {
    // Each widget edits the query in place; the undo snapshot is taken when one is first touched.
    if (std::string d = query_.desc; textField("Description", d) && d != query_.desc) setDescription(d);

    int mode = static_cast<int>(query_.mode);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("Result", kResultNames[mode])) {
        for (int m = 0; m < 5; ++m)
            if (ImGui::Selectable(kResultNames[m], m == mode))
                setResultMode(static_cast<EqsResultMode>(m), query_.modeN ? query_.modeN : 3u, query_.modeP > 0 ? query_.modeP : 25.0f);
        ImGui::EndCombo();
    }
    if (query_.mode == EqsResultMode::Top) {
        int n = static_cast<int>(query_.modeN);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("##topn", &n, 0, 0) && n > 0) setResultMode(EqsResultMode::Top, static_cast<u32>(n), 0.0f);
    } else if (query_.mode == EqsResultMode::Percent || query_.mode == EqsResultMode::RandomPercent) {
        f32 pc = query_.modeP;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("##pct", &pc, 0.5f, 0.1f, 100.0f, "%.1f%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) setResultMode(query_.mode, 0, pc);
    }
    bool runAll = query_.runAll;
    if (ImGui::Checkbox("Run every option (merge)", &runAll)) setRunAll(runAll);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: options run in order and the first that leaves an item wins.");

    EqsQuery::Output out = query_.out;
    bool outChanged = false;
    ImGui::TextUnformatted("Blackboard outputs (RunEqsQuery writes these on the user's board)");
    outChanged |= textField("pos", out.pos, 150.0f);
    ImGui::SameLine();
    outChanged |= textField("entity", out.entity, 150.0f);
    outChanged |= textField("score", out.score, 150.0f);
    ImGui::SameLine();
    outChanged |= textField("count", out.count, 150.0f);
    if (outChanged) setOutput(out);

    ImGui::TextUnformatted("Contexts (name = blackboard key)");
    int removeAt = -1;
    for (usize i = 0; i < query_.contexts.size(); ++i) {
        ImGui::BulletText("%s = %s", query_.contexts[i].first.c_str(), query_.contexts[i].second.c_str());
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) removeContext(static_cast<usize>(removeAt));
    textField("##ctxname", newContextName_, 110.0f);
    ImGui::SameLine();
    textField("##ctxkey", newContextKey_, 140.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add context") && addContext(newContextName_, newContextKey_)) { newContextName_.clear(); newContextKey_.clear(); }
    ImGui::TextDisabled("Built in: querier, target, seen, point");
}

void EqsEditor::drawGeneratorForm(usize oi) {
    EqsGenerator g = query_.options[oi].gen;
    bool changed = false;
    const auto drag = [&](const char* label, f32& v, f32 speed, f32 lo, f32 hi) {
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat(label, &v, speed, lo, hi);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    };
    const auto inti = [&](const char* label, u32& v, u32 lo, u32 hi) {
        int n = static_cast<int>(v);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt(label, &n, 0, 0)) { v = static_cast<u32>(std::clamp<int>(n, static_cast<int>(lo), static_cast<int>(hi))); changed = true; }
    };
    const auto ctxCombo = [&](const char* label, std::string& v) {
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo(label, v.empty() ? "(none)" : v.c_str())) {
            if (v.size() && std::string(label).find("facing") != std::string::npos && ImGui::Selectable("(none)")) { v.clear(); changed = true; }
            const char* builtins[] = {"querier", "target", "seen", "point"};
            for (const char* b : builtins) if (ImGui::Selectable(b, v == b)) { v = b; changed = true; }
            for (const auto& c : query_.contexts) if (ImGui::Selectable(c.first.c_str(), v == c.first)) { v = c.first; changed = true; }
            ImGui::EndCombo();
        }
    };

    int kind = static_cast<int>(g.kind);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("Generator", kGenNames[kind])) {
        for (int k = 0; k < 7; ++k)
            if (ImGui::Selectable(kGenNames[k], k == kind)) { setGeneratorKind(oi, static_cast<EqsGen>(k)); ImGui::EndCombo(); return; }
        ImGui::EndCombo();
    }
    ctxCombo("centre", g.centre);
    int project = static_cast<int>(g.project);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("project", kProjectNames[project])) {
        for (int p = 0; p < 3; ++p)
            if (ImGui::Selectable(kProjectNames[p], p == project)) { g.project = static_cast<EqsProject>(p); changed = true; }
        ImGui::EndCombo();
    }
    drag("z offset", g.zOffset, 1.0f, -10000.0f, 10000.0f);
    inti("max items", g.maxItems, 1, kEqsMaxItems);

    switch (g.kind) {
        case EqsGen::Grid:
            drag("half x", g.halfX, 5.0f, 0.0f, 100000.0f);
            drag("half y", g.halfY, 5.0f, 0.0f, 100000.0f);
            drag("step", g.step, 1.0f, 10.0f, 100000.0f);
            break;
        case EqsGen::Ring:
            drag("inner", g.inner, 5.0f, 0.0f, 100000.0f);
            drag("outer", g.outer, 5.0f, 0.0f, 100000.0f);
            inti("rings", g.rings, 1, 32);
            inti("count", g.count, 1, 256);
            drag("arc start", g.arcStart, 1.0f, -3600.0f, 3600.0f);
            drag("arc end", g.arcEnd, 1.0f, -3600.0f, 3600.0f);
            ctxCombo("facing", g.facing);
            break;
        case EqsGen::Around: {
            f32 arc = g.arcEnd - g.arcStart;
            drag("radius", g.radius, 5.0f, 1.0f, 100000.0f);
            inti("count", g.count, 1, 256);
            drag("arc", arc, 1.0f, 1.0f, 360.0f);
            g.arcStart = -arc * 0.5f;
            g.arcEnd = arc * 0.5f;
            ctxCombo("facing", g.facing);
            break;
        }
        case EqsGen::Nav: {
            drag("radius", g.radius, 5.0f, 1.0f, 100000.0f);
            inti("stride", g.stride, 1, 16);
            bool same = g.sameRegion;
            if (ImGui::Checkbox("same region as the centre", &same)) { g.sameRegion = same; changed = true; }
            break;
        }
        case EqsGen::Actors: {
            int by = !g.className.empty() ? 0 : g.tagBits != 0 ? 1 : 2;
            const char* byNames[] = {"class", "tag bits", "name prefix"};
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo("filter by", byNames[by])) {
                for (int b = 0; b < 3; ++b)
                    if (ImGui::Selectable(byNames[b], b == by)) {
                        g.className.clear(); g.tagBits = 0; g.namePrefix.clear();
                        if (b == 0) g.className = "Actor"; else if (b == 1) g.tagBits = 1; else g.namePrefix = "Actor";
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            if (by == 0) { if (textField("class", g.className, 160.0f)) changed = true; }
            else if (by == 2) { if (textField("name prefix", g.namePrefix, 160.0f)) changed = true; }
            else { int bits = static_cast<int>(g.tagBits); ImGui::SetNextItemWidth(110.0f); if (ImGui::InputInt("tag bits", &bits, 0, 0) && bits > 0) { g.tagBits = static_cast<u32>(bits); changed = true; } }
            drag("radius (0 = any)", g.radius, 5.0f, 0.0f, 1000000.0f);
            bool d = g.derived, x = g.excludeQuerier;
            if (ImGui::Checkbox("include derived classes", &d)) { g.derived = d; changed = true; }
            if (ImGui::Checkbox("exclude the querier", &x)) { g.excludeQuerier = x; changed = true; }
            break;
        }
        case EqsGen::Slots: {
            std::string tags = g.slotTags.csv();
            if (textField("slot tags", tags, 200.0f)) { g.slotTags = TagList::parse(tags); changed = true; }
            drag("radius", g.radius, 5.0f, 1.0f, 100000.0f);
            bool all = g.matchAll, freeOnly = g.freeOnly;
            if (ImGui::Checkbox("match all tags", &all)) { g.matchAll = all; changed = true; }
            if (ImGui::Checkbox("free slots only", &freeOnly)) { g.freeOnly = freeOnly; changed = true; }
            break;
        }
        case EqsGen::Points: {
            ImGui::TextUnformatted("Points (x y z, one per line)");
            std::string text;
            for (const Vec3& p : g.points) text += fmtG(p) + "\n";
            char buf[2048];
            std::snprintf(buf, sizeof buf, "%s", text.c_str());
            ImGui::InputTextMultiline("##points", buf, sizeof buf, ImVec2(-FLT_MIN, 90.0f));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::vector<Vec3> pts;
                const char* s = buf;
                while (*s && pts.size() < 256) {
                    float x = 0, y = 0, z = 0;
                    int used = 0;
                    if (std::sscanf(s, "%f %f %f%n", &x, &y, &z, &used) == 3) { pts.push_back(Vec3{x, y, z}); s += used; }
                    else if (std::sscanf(s, "%f,%f,%f%n", &x, &y, &z, &used) == 3) { pts.push_back(Vec3{x, y, z}); s += used; }
                    else break;
                    while (*s == '\n' || *s == '\r' || *s == ' ' || *s == ';') ++s;
                }
                if (!pts.empty()) { g.points = std::move(pts); changed = true; }
            }
            break;
        }
    }
    if (changed) {
        // In-place edits above are on a copy; one undo snapshot per committed change.
        setGenerator(oi, g);
    }
}

void EqsEditor::drawTestForm(usize oi, usize ti) {
    EqsTest t = query_.options[oi].tests[ti];
    bool changed = false;
    const auto drag = [&](const char* label, f32& v, f32 speed, f32 lo, f32 hi) {
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat(label, &v, speed, lo, hi);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    };
    const auto ctxCombo = [&](const char* label, std::string& v) {
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo(label, v.c_str())) {
            const char* builtins[] = {"querier", "target", "seen", "point"};
            for (const char* b : builtins) if (ImGui::Selectable(b, v == b)) { v = b; changed = true; }
            for (const auto& c : query_.contexts) if (ImGui::Selectable(c.first.c_str(), v == c.first)) { v = c.first; changed = true; }
            ImGui::EndCombo();
        }
    };
    const auto metricCombo = [&](const char* const* names, int count) {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("metric", t.metric.c_str())) {
            for (int i = 0; i < count; ++i) if (ImGui::Selectable(names[i], t.metric == names[i])) { t.metric = names[i]; changed = true; }
            ImGui::EndCombo();
        }
    };

    int kind = static_cast<int>(t.kind);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::BeginCombo("kind", kTestNames[kind])) {
        for (int k = 0; k < 7; ++k)
            if (ImGui::Selectable(kTestNames[k], k == kind)) { setTestKind(oi, ti, static_cast<EqsTestKind>(k)); ImGui::EndCombo(); return; }
        ImGui::EndCombo();
    }
    switch (t.kind) {
        case EqsTestKind::Distance: {
            ctxCombo("to", t.to);
            const char* m[] = {"3d", "2d", "z", "absz"};
            metricCombo(m, 4);
            break;
        }
        case EqsTestKind::Dot: {
            ctxCombo("from", t.from);
            const char* m[] = {"3d", "2d"};
            metricCombo(m, 2);
            const char* refs[] = {"forward", "up", "toward", "dir"};
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::BeginCombo("ref", t.ref.c_str())) {
                for (const char* r : refs) if (ImGui::Selectable(r, t.ref == r)) { t.ref = r; if (t.ref == "toward" && t.refCtx.empty()) t.refCtx = "target"; changed = true; }
                ImGui::EndCombo();
            }
            if (t.ref == "toward") ctxCombo("toward", t.refCtx);
            if (t.ref == "dir") {
                f32 d[3] = {t.refDir.x, t.refDir.y, t.refDir.z};
                ImGui::SetNextItemWidth(220.0f);
                ImGui::DragFloat3("direction", d, 0.05f);
                if (ImGui::IsItemDeactivatedAfterEdit()) { t.refDir = Vec3{d[0], d[1], d[2]}; changed = true; }
            }
            break;
        }
        case EqsTestKind::Trace:
            ctxCombo("from", t.from);
            drag("from z", t.fromZ, 1.0f, -10000.0f, 10000.0f);
            drag("to z", t.toZ, 1.0f, -10000.0f, 10000.0f);
            drag("radius (0 = ray)", t.radius, 0.5f, 0.0f, 10000.0f);
            drag("gap", t.gap, 0.5f, 0.0f, 10000.0f);
            break;
        case EqsTestKind::Path: {
            ctxCombo("from", t.from);
            const char* m[] = {"length", "reachable"};
            metricCombo(m, 2);
            int maxExp = static_cast<int>(t.maxExp);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputInt("max expansions", &maxExp, 0, 0) && maxExp > 0) { t.maxExp = static_cast<u32>(maxExp); changed = true; }
            bool partial = t.partialOk;
            if (ImGui::Checkbox("accept partial paths", &partial)) { t.partialOk = partial; changed = true; }
            break;
        }
        case EqsTestKind::Tag: {
            const bool byNames = !t.tags.v.empty() || t.bits == 0;
            std::string names = t.tags.csv();
            if (byNames) {
                if (textField("tag names", names, 200.0f)) { t.tags = TagList::parse(names); if (t.tags.v.empty()) t.tags.v = {"Tag.Name"}; t.bits = 0; changed = true; }
            } else {
                int bits = static_cast<int>(t.bits);
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::InputInt("tag bits", &bits, 0, 0) && bits > 0) { t.bits = static_cast<u32>(bits); changed = true; }
            }
            if (ImGui::SmallButton(byNames ? "use entity tag bits" : "use tag names")) {
                if (byNames) { t.tags.v.clear(); t.bits = 1; } else { t.bits = 0; t.tags.v = {"Tag.Name"}; }
                changed = true;
            }
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::BeginCombo("match", kMatchNames[t.match])) {
                for (int m = 0; m < 3; ++m) if (ImGui::Selectable(kMatchNames[m], m == t.match)) { t.match = m; changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case EqsTestKind::Random: {
            int seed = static_cast<int>(t.seed);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputInt("seed", &seed, 0, 0) && seed >= 0) { t.seed = static_cast<u32>(seed); changed = true; }
            break;
        }
        case EqsTestKind::Custom: {
            std::string nm = t.custom;
            if (textField("managed test", nm, 180.0f) && !nm.empty()) { t.custom = nm; t.name = nm; changed = true; }
            drag("p0", t.p[0], 0.1f, -1e6f, 1e6f);
            ImGui::SameLine();
            drag("p1", t.p[1], 0.1f, -1e6f, 1e6f);
            drag("p2", t.p[2], 0.1f, -1e6f, 1e6f);
            ImGui::SameLine();
            drag("p3", t.p[3], 0.1f, -1e6f, 1e6f);
            if (textField("text", t.text, 180.0f)) changed = true;
            break;
        }
    }
    ImGui::Separator();
    int purpose = static_cast<int>(t.purpose);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::BeginCombo("purpose", kPurposeNames[purpose])) {
        for (int p = 0; p < 3; ++p) if (ImGui::Selectable(kPurposeNames[p], p == purpose)) { t.purpose = static_cast<EqsPurpose>(p); changed = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    drag("weight", t.weight, 0.05f, -100.0f, 100.0f);
    int curve = static_cast<int>(t.curve);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::BeginCombo("curve", kCurveNames[curve])) {
        for (int c = 0; c < 6; ++c) if (ImGui::Selectable(kCurveNames[c], c == curve)) { t.curve = static_cast<EqsCurve>(c); changed = true; }
        ImGui::EndCombo();
    }
    bool hasLo = t.hasLo, hasHi = t.hasHi;
    if (ImGui::Checkbox("lo", &hasLo)) { t.hasLo = hasLo; changed = true; }
    if (t.hasLo) { ImGui::SameLine(); drag("##lo", t.lo, 0.5f, -1e9f, 1e9f); }
    ImGui::SameLine();
    if (ImGui::Checkbox("hi", &hasHi)) { t.hasHi = hasHi; changed = true; }
    if (t.hasHi) { ImGui::SameLine(); drag("##hi", t.hi, 0.5f, -1e9f, 1e9f); }
    if (eqsTestIsBoolean(t)) {
        bool want = t.want;
        if (ImGui::Checkbox("keep items where it is true", &want)) { t.want = want; changed = true; }
    } else {
        bool hasMin = std::isfinite(t.min), hasMax = std::isfinite(t.max);
        if (ImGui::Checkbox("min", &hasMin)) { t.min = hasMin ? 0.0f : -INFINITY; changed = true; }
        if (hasMin) { ImGui::SameLine(); f32 v = t.min; drag("##min", v, 0.5f, -1e9f, 1e9f); t.min = v; }
        ImGui::SameLine();
        if (ImGui::Checkbox("max", &hasMax)) { t.max = hasMax ? 1000.0f : INFINITY; changed = true; }
        if (hasMax) { ImGui::SameLine(); f32 v = t.max; drag("##max", v, 0.5f, -1e9f, 1e9f); t.max = v; }
    }
    if (t.kind != EqsTestKind::Custom && textField("label", t.name, 140.0f)) changed = true;
    if (changed) setTest(oi, ti, t);
}

void EqsEditor::drawOptions() {
    // option list
    for (usize i = 0; i < query_.options.size(); ++i) {
        const EqsOption& o = query_.options[i];
        const std::string label = "Option " + std::to_string(i + 1) + ": " + kGenNames[static_cast<int>(o.gen.kind)] + ", " + std::to_string(o.tests.size()) + " test(s)";
        if (ImGui::Selectable(label.c_str(), option_ == static_cast<i32>(i))) option_ = static_cast<i32>(i);
    }
    if (ImGui::SmallButton("+ option")) addOption();
    ImGui::SameLine();
    ImGui::BeginDisabled(!optIn(query_, static_cast<usize>(std::max(option_, 0))) || query_.options.size() <= 1);
    if (ImGui::SmallButton("remove")) removeOption(static_cast<usize>(option_));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("earlier")) moveOption(static_cast<usize>(std::max(option_, 0)), -1);
    ImGui::SameLine();
    if (ImGui::SmallButton("later")) moveOption(static_cast<usize>(std::max(option_, 0)), 1);

    if (!optIn(query_, static_cast<usize>(std::max(option_, 0)))) return;
    const usize oi = static_cast<usize>(std::max(option_, 0));
    ImGui::Separator();
    ImGui::PushID(static_cast<int>(oi));
    drawGeneratorForm(oi);
    ImGui::Separator();
    ImGui::TextUnformatted("Tests (run cheapest first; each one only sees the survivors)");
    int removeAt = -1, moveAt = -1, moveDelta = 0;
    for (usize ti = 0; ti < query_.options[oi].tests.size(); ++ti) {
        const EqsTest& t = query_.options[oi].tests[ti];
        ImGui::PushID(static_cast<int>(ti));
        const std::string head = std::to_string(ti + 1) + ". " + eqsTestName(t.kind) + (t.name.empty() ? "" : " (" + t.name + ")");
        if (::aver::editor::sectionHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            drawTestForm(oi, ti);
            if (ImGui::SmallButton("up")) { moveAt = static_cast<int>(ti); moveDelta = -1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("down")) { moveAt = static_cast<int>(ti); moveDelta = 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(ti);
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) removeTest(oi, static_cast<usize>(removeAt));
    else if (moveAt >= 0) moveTest(oi, static_cast<usize>(moveAt), moveDelta);
    if (ImGui::BeginCombo("##addtest", "Add test")) {
        for (int k = 0; k < 7; ++k)
            if (ImGui::Selectable(kTestNames[k])) { addTest(oi, static_cast<EqsTestKind>(k)); break; }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void EqsEditor::drawPreview() {
    ImGui::TextWrapped("Run on selection: the first selected entity is the querier, the second (if any) the target. The run stays "
                       "in the viewport (Show > AI > Queries) until replaced.");
#if AVER_EQSE_LIVE
    const std::vector<u32> sel = selectionSource() ? selectionSource()() : std::vector<u32>();
    scene::World& w = scene::World::instance();
    const auto nameOf = [&](u32 e) { return (e != 0 && w.valid(static_cast<scene::Entity>(e))) ? std::string(w.name(static_cast<scene::Entity>(e))) : std::string("(none)"); };
    const u32 querier = sel.empty() ? 0 : sel[0];
    const u32 target = sel.size() > 1 ? sel[1] : 0;
    ImGui::Text("Querier: %s    Target: %s", nameOf(querier).c_str(), nameOf(target).c_str());
    ImGui::BeginDisabled(querier == 0);
    if (ImGui::Button("Run on selection")) runPreview(*static_cast<synapse::SynapseAi*>(aver_syn_ai_instance()), querier, target);
    ImGui::EndDisabled();
    if (!previewMessage_.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s", previewMessage_.c_str());
    if (previewHandle_ != 0) {
        synapse::SynapseAi& ai = *static_cast<synapse::SynapseAi*>(aver_syn_ai_instance());
        if (const EqsResult* r = ai.eqs().result(previewHandle_)) {
            ImGui::Text("%zu item(s); generated %u, passed %u, %u evaluations%s", r->items.size(), r->generated, r->passed, r->evaluations, r->truncated ? ", truncated" : "");
            if (ImGui::BeginTable("##eqsres", static_cast<int>(4 + r->testCount), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                ImGui::TableSetupColumn("score", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                ImGui::TableSetupColumn("position");
                ImGui::TableSetupColumn("entity", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                for (u32 t = 0; t < r->testCount; ++t) ImGui::TableSetupColumn(("t" + std::to_string(t + 1)).c_str(), ImGuiTableColumnFlags_WidthFixed, 56.0f);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < r->items.size() && i < 500; ++i) {
                    const EqsItem& it = r->items[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%zu", i + 1);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", static_cast<double>(it.score));
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f, %.0f, %.0f", static_cast<double>(it.pos.x), static_cast<double>(it.pos.y), static_cast<double>(it.pos.z));
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", it.entity);
                    for (u32 t = 0; t < r->testCount; ++t) {
                        ImGui::TableSetColumnIndex(static_cast<int>(4 + t));
                        const f32 v = r->testValues[i * r->testCount + t];
                        if (std::isnan(v)) ImGui::TextDisabled("-"); else ImGui::Text("%.2f", static_cast<double>(v));
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("The last run's result is gone; run it again.");
        }
    }
#else
    ImGui::TextDisabled("Needs a build with the AI scene module.");
#endif
}

void EqsEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[EqsEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
            if (canUndo() && keybinds().pressed(CommandId::EditUndo, io)) undo();
            if (canRedo() && keybinds().pressed(CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    const std::vector<std::string> w = warnings();
    if (!w.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f), "%zu warning%s", w.size(), w.size() == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const std::string& s : w) ImGui::TextUnformatted(s.c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::Separator();

    const f32 dpi = fontScale();
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 minForm = 320.0f * dpi, minSide = 280.0f * dpi;
    const f32 formW = splitPaneWidth(split_, kPrefSplit, kDefaultFormFraction, avail, minForm, minSide);
    if (ImGui::BeginChild("##eqsform", ImVec2(formW, 0), ImGuiChildFlags_Borders)) {
        drawHeader();
        ImGui::Separator();
        drawOptions();
    }
    ImGui::EndChild();
    drawSplitHandle(split_, "##eqssplit", kPrefSplit, avail, minForm, minSide, 6.0f * dpi);
    if (ImGui::BeginChild("##eqspreview", ImVec2(0, 0), ImGuiChildFlags_Borders)) drawPreview();
    ImGui::EndChild();
}

void EqsEditor::resetLayout() { resetSplitPane(split_, kPrefSplit, kDefaultFormFraction); }

#else   // AVER_WITH_IMGUI

void EqsEditor::draw(Engine& e) { (void)e; }
void EqsEditor::resetLayout() {}

#endif  // AVER_WITH_IMGUI

} // namespace aver::editor
