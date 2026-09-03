// Project browser: the full-screen start screen with the recent project list, the open/new
// actions and the New Project modal.

#include "ProjectBrowser.hpp"
#include "ProjectScaffold.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"
#if AVER_MODULE_UPGRADE
#include "aver/upgrade/Upgrade.hpp"
#endif
#include "aver/core/Log.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {
namespace {

std::string recentsPath() { return userDataDir() + "\\recent.txt"; }

// ADOPTS a project that records no CREATEDWITH: stamps it with this engine and writes the manifest
// back. Never prompts, never migrates.
//
// EMPTY MEANS CURRENT, and that is a policy choice rather than a reading of the file. Every project
// that existed before the field did has no value here, and treating "no stamp" as "very old" would
// make every author on earth answer an upgrade prompt for a project that is very likely fine. So
// the first open under 0.2 records 0.2 and says nothing, and only a project stamped with an OLDER
// SERIES is ever asked to migrate.
//
// The cost of that choice, stated plainly because it is real: a project genuinely made by an 0.1
// install is adopted as 0.2 without running the 0.1 -> 0.2 step, so an author who hit the F# starter
// bug still has to fix it by hand or with --upgrade-project. The alternative was prompting everyone,
// which is worse for far more people.
//
// A FAILED WRITE IS NOT A FAILED OPEN. A read-only project, a file open elsewhere, a network share
// that blinked -- none of those are reasons to refuse to open somebody's work. The stamp is missing
// again next time, which costs one line in the log and nothing else.
void adoptVersionStamp(fmt::ProjectDesc& p) {
    if (!p.createdWith.empty() || p.manifestPath.empty()) return;

    std::string existing;
    if (!readFileText(p.manifestPath, existing)) {
        AVER_WARN("[Editor] '{}' records no engine version and could not be re-read to stamp one", p.name);
        return;
    }
    p.createdWith = std::string(kEngineVersion);
    const std::string text = fmt::writeOcproject(p, existing);
    if (!writeFileText(p.manifestPath, text)) {
        AVER_WARN("[Editor] '{}': could not write the {} version stamp; it will be stamped next open",
                  p.name, kEngineVersion);
        return;
    }
    AVER_INFO("[Editor] '{}' recorded no engine version; adopted as {}", p.name, kEngineVersion);
}

// ONE PLACE THE UPGRADE MODULE IS ASKED ANYTHING, so a build without it degrades in one spot
// rather than through an #if at every call site.
//
// WITHOUT Aver.Upgrade THERE IS NO OLDER PROJECT. That is not a stub: the browser must never offer
// a migration this build could not carry out, so with no chain compiled in, every project opens as
// it always did. The version tag still draws -- it is just a string from the manifest -- and only
// the prompt disappears.
#if AVER_MODULE_UPGRADE
bool madeByOlderSeries(const std::string& stamp) {
    upgrade::Version v{};
    return upgrade::parseVersion(stamp, v) && upgrade::olderSeries(v, upgrade::engineVersion());
}
#else
bool madeByOlderSeries(const std::string&) { return false; }
#endif

// The name to show for a manifest path: the file stem.
std::string displayName(const std::string& manifestPath) {
    return std::filesystem::path(manifestPath).stem().string();
}

// Copies a string into a fixed char buffer, truncating and always null-terminating.
void setBuf(char* dst, usize cap, const std::string& s) {
    const usize n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

} // namespace

// Loads the recent project list, dropping entries whose file is gone, and seeds the new-project
// location.
void ProjectBrowser::init() {
    std::string text;
    if (!readFileText(recentsPath(), text)) text.clear();
    // fileExists is the keep-predicate: a project that has gone from disk is dropped on read.
    list_.parse(text, [](const std::string& p) { return fileExists(p); });

    setBuf(locBuf_, sizeof locBuf_, documentsDir() + "\\Aver Projects");

    // Once per session: the shipped templates directory cannot change while the editor is running,
    // unlike the projects folder (rescan() re-reads that one whenever the cards go stale). A
    // missing, empty or entirely-malformed templates\ all resolve to the same empty vector here --
    // see listTemplates()'s own comment -- so New Project falls back to Blank-only with no error.
    templates_ = listTemplates();
    if (!templates_.empty())
        AVER_INFO("[Editor] {} project template(s) found", templates_.size());
}

// Writes the recent project list to disk.
void ProjectBrowser::saveRecents() const {
    createDirectories(userDataDir());
    if (!writeFileText(recentsPath(), list_.serialise()))
        AVER_WARN("[Editor] could not write the recent project list to {}", recentsPath());
}

// Finds every project worth showing: the recent list, plus every .ocproject one level under the
// projects folder.
//
// THE FOLDER IS SCANNED, not just the recent list, because "recent" is per-machine state in
// AppData and the projects are not. Copy a project onto another machine, or reinstall, and the
// recent list is empty while the work is right there in Documents\Aver Projects.
//
// THE FILESYSTEM HALF ONLY. Ordering, de-duplication and what becomes of the selection all live in
// RecentProjects::rebuild, where a test can reach them; this walks the disk and reads the stamps.
void ProjectBrowser::rescan() {
    std::vector<ProjectCard> found;

    const auto describe = [&found](const std::string& path) {
        ProjectCard c;
        c.path = path;
        c.name = displayName(path);
        // The manifest is read for the version anyway, so take the NAME from it too. The file stem
        // is a poor label on its own: the convention for a project's manifest is `Game.ocproject`,
        // so a list built from stems showed six consecutive rows reading "Game" and made the recent
        // list unusable for exactly the projects that follow the convention. The stem stays as the
        // fallback for a manifest that will not load or records no name.
        //
        // The version only, otherwise: a full load would validate the ENGINE line and reject the
        // very projects this screen exists to show, which is the opposite of useful in a browser.
        fmt::ProjectDesc d;
        if (fmt::loadOcproject(path, d, nullptr)) {
            c.version = d.createdWith;
            if (!d.name.empty()) c.name = d.name;
        }
        found.push_back(std::move(c));
    };

    for (const std::string& r : list_.recents()) describe(r);

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(std::string(locBuf_));
    if (std::filesystem::is_directory(root, ec)) {
        for (const auto& sub : std::filesystem::directory_iterator(root, ec)) {
            if (!sub.is_directory(ec)) continue;
            for (const auto& f : std::filesystem::directory_iterator(sub.path(), ec)) {
                if (f.is_regular_file(ec) && f.path().extension() == ".ocproject")
                    describe(f.path().string());
            }
        }
    }

    list_.rebuild(found);
}

// Opens a project unless it predates this engine's series, in which case the author is asked first.
bool ProjectBrowser::openOrOfferUpgrade(const std::string& path, std::string* errOut) {
    fmt::ProjectDesc peek;
    // A manifest that will not load at all is not an upgrade question -- let open() report why.
    if (fmt::loadOcproject(path, peek, nullptr) && madeByOlderSeries(peek.createdWith)) {
        upgradeModal_ = true;
        upgradePath_  = path;
        upgradeName_  = displayName(path);
        upgradeFrom_  = peek.createdWith;
        upgradeError_.clear();
        upgradeConfirmBuf_[0] = '\0';
        return false;
    }
    std::string err;
    if (open(path, &err)) return true;
    error_ = err;
    if (errOut) *errOut = err;
    // A project that would not open is dropped from the list. forget() marks the cards stale
    // itself, so the two can no longer disagree about what row means what.
    list_.forget(path);
    saveRecents();
    return false;
}

// Loads an .ocproject and moves it to the top of the recent list. False on failure.
bool ProjectBrowser::open(const std::string& manifestPath, std::string* err) {
    fmt::ProjectDesc desc;
    if (!fmt::loadOcproject(manifestPath, desc, err)) return false;
    project_ = desc;
    adoptVersionStamp(project_);

    list_.remember(project_.manifestPath);
    saveRecents();

    AVER_INFO("[Editor] project '{}' loaded from {}", project_.name, project_.manifestPath);
    return true;
}

// Draws the browser for one frame and returns what the user asked for.
BrowserAction ProjectBrowser::draw(f32 dpi, ImFont* medium, u64 logoTex, f32 logoAspect) {
#if !AVER_WITH_IMGUI
    (void)dpi; (void)medium; (void)logoTex; (void)logoAspect;
    return BrowserAction::Skip;
#else
    BrowserAction action = BrowserAction::Stay;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec4 accent(0.95f, 0.42f, 0.13f, 1.0f);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f * dpi, 24.0f * dpi));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.086f, 0.086f, 0.094f, 1.0f));
    ImGui::Begin("##projectbrowser", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- branding ----
    {
        const f32 badge = 46.0f * dpi;
        if (logoTex) {
            const f32 w = logoAspect >= 1.0f ? badge : badge * logoAspect;
            const f32 h = logoAspect >= 1.0f ? badge / logoAspect : badge;
            ImGui::Image(static_cast<ImTextureID>(logoTex), ImVec2(w, h));
        } else {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + badge, p.y + badge), ImGui::GetColorU32(accent), 8.0f * dpi);
            const char* mark = "AE";
            const ImVec2 ts = ImGui::CalcTextSize(mark);
            dl->AddText(ImVec2(p.x + (badge - ts.x) * 0.5f, p.y + (badge - ts.y) * 0.5f),
                        IM_COL32(20, 18, 16, 255), mark);
            ImGui::Dummy(ImVec2(badge, badge));
        }
        ImGui::SameLine(0, 14.0f * dpi);
        ImGui::BeginGroup();
        if (medium) ImGui::PushFont(medium, 0.0f);
        ImGui::TextUnformatted("AVER ENGINE");
        if (medium) ImGui::PopFont();
        ImGui::TextDisabled("%.*s %.*s  |  Project Browser",
                            (int)kEngineName.size(), kEngineName.data(),
                            (int)kEngineVersion.size(), kEngineVersion.data());
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, 8.0f * dpi));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8.0f * dpi));

    const f32 footerH = 46.0f * dpi;
    const f32 bodyH = ImGui::GetContentRegionAvail().y - footerH;
    const f32 rightW = 260.0f * dpi;

    // ---- recent projects ----
    ImGui::BeginChild("##recents", ImVec2(ImGui::GetContentRegionAvail().x - rightW - 16.0f * dpi, bodyH));
    if (medium) ImGui::PushFont(medium, 0.0f);
    ImGui::TextUnformatted("RECENT PROJECTS");
    if (medium) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::BeginChild("##recentlist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (list_.dirty()) rescan();
    if (list_.cards().empty()) {
        ImGui::Dummy(ImVec2(0, 6.0f * dpi));
        ImGui::TextDisabled("  No projects yet.");
        ImGui::TextDisabled("  Use New Project to scaffold one, or Open Project to");
        ImGui::TextDisabled("  point the editor at an existing .ocproject.");
    }
    for (int i = 0; i < (int)list_.cards().size(); ++i) {
        const ProjectCard& card = list_.cards()[static_cast<usize>(i)];
        ImGui::PushID(i);
        const std::string label = "  " + card.name;
        if (ImGui::Selectable(label.c_str(), list_.selection() == i,
                              ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 40.0f * dpi))) {
            list_.select(i);
            if (ImGui::IsMouseDoubleClicked(0) && openOrOfferUpgrade(card.path))
                action = BrowserAction::Open;
        }
        // Right-click a row to drop it. RecentProjects::forget has been public since the class
        // existed and was reachable from exactly one place: the FAILURE path, when a project would
        // not open. A project you simply no longer want listed could only be removed by deleting it
        // from disk or hand-editing recent.txt.
        //
        // It removes the ENTRY, never the project, and the wording says so -- this menu sits one
        // slip away from reading as "delete", and the two are not the same thing at all.
        if (ImGui::BeginPopupContextItem()) {
            ImGui::TextDisabled("%s", card.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from this list")) {
                list_.forget(card.path);
                saveRecents();
                // Still on disk, so the folder scan may legitimately bring it straight back as a
                // non-recent card. That is correct: the list is recents PLUS what is in the projects
                // folder, and forgetting is about the recent half.
            }
            ImGui::TextDisabled("The project itself is left on disk.");
            ImGui::EndPopup();
        }

        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(rmin.x + 12.0f * dpi, rmin.y + 20.0f * dpi),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), card.path.c_str());

        // THE VERSION TAG, top right. A project with no stamp shows nothing rather than "unknown":
        // an empty stamp means "adopted as current on first open", so saying anything would be
        // reporting a state the author is never asked to act on. A project from an OLDER series is
        // tinted, because that one WILL ask a question when it is opened and the card is the only
        // warning before the click.
        if (!card.version.empty()) {
            const bool old = madeByOlderSeries(card.version);
            const ImVec2 ts = ImGui::CalcTextSize(card.version.c_str());
            const ImVec2 at(rmax.x - ts.x - 12.0f * dpi, rmin.y + 6.0f * dpi);
            dl->AddText(at, old ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        card.version.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    // ---- actions ----
    ImGui::SameLine(0, 16.0f * dpi);
    ImGui::BeginChild("##actions", ImVec2(rightW, bodyH));
    if (medium) ImGui::PushFont(medium, 0.0f);
    ImGui::TextUnformatted("START");
    if (medium) ImGui::PopFont();
    ImGui::Spacing();
    const ImVec2 wide(-1, 32.0f * dpi);
    if (ImGui::Button("New Project...", wide)) {
        newError_.clear();
        setBuf(nameBuf_, sizeof nameBuf_, "");
        newTemplateSel_ = -1;   // always reopens on Blank -- today's only choice stays the default
        openNewModal_ = true;
    }
    if (ImGui::Button("Open Project...", wide)) {
        std::string picked;
        const std::string start = std::string(locBuf_);
        if (openFileDialog("Open Aver project", "Aver project (*.ocproject)", "*.ocproject",
                           directoryExists(start) ? start : std::string(), picked)) {
            // openOrOfferUpgrade, not open: this used to be one of THREE controls that walked past
            // the upgrade gate, so a project from an older series opened here loaded unmigrated and
            // could then be saved over. The header's contract says every path passes the same gate.
            if (openOrOfferUpgrade(picked)) action = BrowserAction::Open;
        }
    }
    ImGui::Dummy(ImVec2(0, 10.0f * dpi));
    ImGui::TextDisabled("...or type a path");
    ImGui::PushItemWidth(-1);
    const bool submitted = ImGui::InputTextWithHint("##path", "C:\\...\\Name.ocproject", pathBuf_,
                                                    sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    // Disabled on an empty field rather than accepting a click that silently does nothing.
    ImGui::BeginDisabled(!pathBuf_[0]);
    const bool goPressed = ImGui::Button("Open Path", wide);
    ImGui::EndDisabled();
    if (!pathBuf_[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Type the path to a .ocproject first");
    if ((submitted || goPressed) && pathBuf_[0]) {
        // The third of the three paths that skipped the upgrade gate. See "Open Project..." above.
        if (openOrOfferUpgrade(pathBuf_)) action = BrowserAction::Open;
    }
    ImGui::EndChild();

    if (!error_.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", error_.c_str());

    // ---- footer ----
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 38.0f * dpi);
    ImGui::TextDisabled("A project is optional - the editor runs without one.");
    ImGui::SameLine();
    const f32 btn = 150.0f * dpi;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (btn * 2 + 100.0f * dpi + 30.0f * dpi));
    // THROUGH selectedPath(), NOT AN INDEX. This used to read `recents_[recentSel_]` while the
    // selection indexed `cards_` -- so it refused to open any project the folder scan had found
    // rather than the recent list, and after a failed open it could open a DIFFERENT project than
    // the row that was highlighted. Both were the same missing invariant; see RecentProjects.hpp.
    //
    // Same gate as a double-click, too: openOrOfferUpgrade rather than open, so a legacy project
    // reaches the upgrade prompt whichever control opened it.
    const std::string selected = list_.selectedPath();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button("Open Selected", ImVec2(btn, 0))) {
        if (openOrOfferUpgrade(selected)) action = BrowserAction::Open;
    }
    ImGui::EndDisabled();
    if (selected.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Pick a project from the list first");
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(btn, 0))) action = BrowserAction::Skip;
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(90.0f * dpi, 0))) action = BrowserAction::Quit;

    // ---- New Project modal ----
    // Opened inside the browser window's ID stack so BeginPopupModal below finds it.
    if (openNewModal_) { ImGui::OpenPopup("New Project"); openNewModal_ = false; }
    ImGui::SetNextWindowSize(ImVec2(620.0f * dpi, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextDisabled("Projects are created OUTSIDE the engine folder, as siblings of it.");
        ImGui::Separator();
        ImGui::Spacing();

        // ---- template picker: Blank Project, then one card per shipped template ----
        // Cards, not a dropdown: with one template today and room for a handful more, seeing every
        // option (and its description) at once is more useful than a name to pick blind from a list.
        // Adding a second template is only ever a new templates\<Id>\ directory -- nothing here
        // changes to grow the row.
        ImGui::TextUnformatted("Template");
        ImGui::Spacing();
        {
            const f32 cardW = 172.0f * dpi, cardH = 72.0f * dpi, gap = 10.0f * dpi;
            const f32 avail = ImGui::GetContentRegionAvail().x;
            f32 lineX = 0.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            auto card = [&](int sel, const std::string& title, const std::string& desc) {
                if (lineX > 0.0f) {
                    if (lineX + cardW <= avail) ImGui::SameLine(0, gap);
                    else lineX = 0.0f;
                }
                ImGui::PushID(sel);
                const bool selected = newTemplateSel_ == sel;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.95f, 0.42f, 0.13f, 0.35f));
                if (ImGui::Selectable("##tmplcard", selected, ImGuiSelectableFlags_None,
                                      ImVec2(cardW, cardH)))
                    newTemplateSel_ = sel;
                if (selected) ImGui::PopStyleColor();
                const ImVec2 rmin = ImGui::GetItemRectMin();
                dl->AddText(ImVec2(rmin.x + 10.0f * dpi, rmin.y + 10.0f * dpi),
                           ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
                dl->AddText(ImVec2(rmin.x + 10.0f * dpi, rmin.y + 32.0f * dpi),
                           ImGui::GetColorU32(ImGuiCol_TextDisabled), desc.c_str());
                ImGui::PopID();
                lineX += cardW + gap;
            };

            card(-1, "Blank Project", "An empty project with a starter level.");
            for (int i = 0; i < (int)templates_.size(); ++i)
                card(i, templates_[i].name, templates_[i].description);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("Name");
        ImGui::PushItemWidth(-1);
        ImGui::InputText("##newname", nameBuf_, sizeof nameBuf_);
        ImGui::PopItemWidth();

        ImGui::Spacing();
        ImGui::TextUnformatted("Location");
        ImGui::PushItemWidth(-(110.0f * dpi));
        ImGui::InputText("##newloc", locBuf_, sizeof locBuf_);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(100.0f * dpi, 0))) {
            std::string picked;
            if (openFileDialog("Pick any project in the target location", "Aver project (*.ocproject)",
                               "*.ocproject", std::string(locBuf_), picked)) {
                const std::filesystem::path root = std::filesystem::path(picked).parent_path().parent_path();
                setBuf(locBuf_, sizeof locBuf_, root.string());
            }
        }

        ImGui::Spacing();
        if (nameBuf_[0])
            ImGui::TextDisabled("Creates: %s\\%s\\%s.ocproject", locBuf_, nameBuf_, nameBuf_);
        else
            ImGui::TextDisabled("Creates: <location>\\<Name>\\<Name>.ocproject");

        if (!newError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", newError_.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Create Project", ImVec2(160.0f * dpi, 0))) {
            fmt::ProjectDesc created;
            newError_.clear();
            // Blank is UNCHANGED: this is the exact call it has always been, still the only branch
            // reachable when newTemplateSel_ is left at its default of -1.
            const bool ok = (newTemplateSel_ < 0)
                ? scaffoldProject(locBuf_, nameBuf_, created, &newError_)
                : scaffoldProjectFromTemplate(locBuf_, nameBuf_, templates_[newTemplateSel_],
                                              created, &newError_);
            if (ok) {
                std::string err;
                if (open(created.manifestPath, &err)) {
                    action = BrowserAction::Open;
                    ImGui::CloseCurrentPopup();
                } else {
                    newError_ = err;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * dpi, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Upgrade modal ----
    // Raised instead of opening a project made by an older SERIES. Three ways out and no fourth:
    // work on a copy, convert this one, or go back. Nothing is touched until one is chosen.
    if (upgradeModal_) { ImGui::OpenPopup("Upgrade Project"); upgradeModal_ = false; }
    ImGui::SetNextWindowSize(ImVec2(620.0f * dpi, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Upgrade Project", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("'%s' was made with Aver Engine %s. This is %.*s.",
                           upgradeName_.c_str(), upgradeFrom_.c_str(),
                           (int)kEngineVersion.size(), kEngineVersion.data());
        ImGui::Spacing();
        ImGui::TextDisabled("Upgrading rewrites files the engine owns. It cannot be undone.");
        ImGui::Separator();
        ImGui::Spacing();

        // COPY FIRST AND COPY DEFAULT, because it is the only choice that cannot lose work. The
        // migration chain has no rollback: a step that fails leaves the project as it found it.
        if (ImGui::Button("Upgrade a Copy", ImVec2(190.0f * dpi, 32.0f * dpi))) {
            std::string err;
            // The copy is named for the version it is ABOUT to become, which is only true once
            // the migration below has actually run against it.
            const std::string copied = copyProjectTree(upgradePath_, std::string(kEngineVersion), &err);
            if (copied.empty()) {
                upgradeError_ = err;
            } else if (!migrateProject(copied, &err)) {
                // The ORIGINAL is untouched and the half-migrated copy is left on disk under its
                // own name, which is the whole reason this path exists: there is no rollback in
                // the chain, so the thing that can be inspected afterwards must not be the only
                // copy of the author's work.
                upgradeError_ = err;
            } else if (!open(copied, &err)) {
                upgradeError_ = err;
            } else {
                // No dirty flag to set by hand: open() remembers the project, and remembering
                // marks the cards stale. That used to be two separate things a caller had to keep
                // in step, and forgetting it here is how the selection came to mean another row.
                upgradePath_.clear();
                action = BrowserAction::Open;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("copies the folder, then opens the copy");

        ImGui::Dummy(ImVec2(0, 8.0f * dpi));
        ImGui::TextDisabled("Or convert this project in place. Type its name to confirm:");
        ImGui::PushItemWidth(260.0f * dpi);
        ImGui::InputTextWithHint("##confirm", upgradeName_.c_str(), upgradeConfirmBuf_,
                                 sizeof upgradeConfirmBuf_);
        ImGui::PopItemWidth();
        const bool typed = upgradeName_ == upgradeConfirmBuf_;
        ImGui::SameLine();
        ImGui::BeginDisabled(!typed);
        if (ImGui::Button("Convert in Place", ImVec2(170.0f * dpi, 0))) {
            std::string err;
            if (!migrateProject(upgradePath_, &err)) {
                upgradeError_ = err;
            } else if (!open(upgradePath_, &err)) {
                upgradeError_ = err;
            } else {
                // No dirty flag to set by hand: open() remembers the project, and remembering
                // marks the cards stale. That used to be two separate things a caller had to keep
                // in step, and forgetting it here is how the selection came to mean another row.
                upgradePath_.clear();
                action = BrowserAction::Open;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();

        if (!upgradeError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", upgradeError_.c_str());
        }

        ImGui::Dummy(ImVec2(0, 6.0f * dpi));
        ImGui::Separator();
        if (ImGui::Button("Cancel", ImVec2(110.0f * dpi, 0))) {
            upgradeError_.clear();
            upgradePath_.clear();         // the question is answered: nothing pending
            ImGui::CloseCurrentPopup();   // back to the browser, project untouched
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return action;
#endif
}

} // namespace aver::editor
