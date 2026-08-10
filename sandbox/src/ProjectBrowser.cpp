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

constexpr usize kMaxRecents = 10;

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
    recents_.clear();

    std::string text;
    if (readFileText(recentsPath(), text)) {
        usize pos = 0;
        while (pos <= text.size() && recents_.size() < kMaxRecents) {
            usize nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && fileExists(line)) recents_.push_back(line);
        }
    }

    setBuf(locBuf_, sizeof locBuf_, documentsDir() + "\\Aver Projects");
}

// Writes the recent project list to disk.
void ProjectBrowser::saveRecents() const {
    createDirectories(userDataDir());
    std::string text;
    for (const std::string& p : recents_) text += p + "\n";
    if (!writeFileText(recentsPath(), text))
        AVER_WARN("[Editor] could not write the recent project list to {}", recentsPath());
}

// Drops one path from the recent list.
void ProjectBrowser::forget(const std::string& manifestPath) {
    for (usize i = 0; i < recents_.size(); ++i) {
        if (recents_[i] == manifestPath) { recents_.erase(recents_.begin() + static_cast<isize>(i)); return; }
    }
}

// Rebuilds the card list: the recent projects first, in their order, then every other .ocproject
// found one level under the projects folder.
//
// THE FOLDER IS SCANNED, not just the recent list, because "recent" is per-machine state in
// AppData and the projects are not. Copy a project onto another machine, or reinstall, and the
// recent list is empty while the work is right there in Documents\Aver Projects.
void ProjectBrowser::rescan() {
    cards_.clear();
    cardsDirty_ = false;

    const auto push = [this](const std::string& path, bool recent) {
        for (const Card& c : cards_) if (c.path == path) return;   // recents win; no duplicates
        Card c;
        c.path = path;
        c.name = displayName(path);
        c.recent = recent;
        // The version only: a full load would validate the ENGINE line and reject the very
        // projects this screen exists to show, which is the opposite of useful in a browser.
        fmt::ProjectDesc d;
        if (fmt::loadOcproject(path, d, nullptr)) c.version = d.createdWith;
        cards_.push_back(std::move(c));
    };

    for (const std::string& r : recents_) push(r, true);

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(std::string(locBuf_));
    if (std::filesystem::is_directory(root, ec)) {
        for (const auto& sub : std::filesystem::directory_iterator(root, ec)) {
            if (!sub.is_directory(ec)) continue;
            for (const auto& f : std::filesystem::directory_iterator(sub.path(), ec)) {
                if (f.is_regular_file(ec) && f.path().extension() == ".ocproject")
                    push(f.path().string(), false);
            }
        }
    }
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
    forget(path);
    saveRecents();
    cardsDirty_ = true;
    recentSel_ = -1;
    return false;
}

// Loads an .ocproject and moves it to the top of the recent list. False on failure.
bool ProjectBrowser::open(const std::string& manifestPath, std::string* err) {
    fmt::ProjectDesc desc;
    if (!fmt::loadOcproject(manifestPath, desc, err)) return false;
    project_ = desc;
    adoptVersionStamp(project_);

    forget(project_.manifestPath);
    recents_.insert(recents_.begin(), project_.manifestPath);
    if (recents_.size() > kMaxRecents) recents_.resize(kMaxRecents);
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
    if (cardsDirty_) rescan();
    if (cards_.empty()) {
        ImGui::Dummy(ImVec2(0, 6.0f * dpi));
        ImGui::TextDisabled("  No projects yet.");
        ImGui::TextDisabled("  Use New Project to scaffold one, or Open Project to");
        ImGui::TextDisabled("  point the editor at an existing .ocproject.");
    }
    for (int i = 0; i < (int)cards_.size(); ++i) {
        const Card& card = cards_[i];
        ImGui::PushID(i);
        const std::string label = "  " + card.name;
        if (ImGui::Selectable(label.c_str(), recentSel_ == i,
                              ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 40.0f * dpi))) {
            recentSel_ = i;
            if (ImGui::IsMouseDoubleClicked(0) && openOrOfferUpgrade(card.path))
                action = BrowserAction::Open;
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
        openNewModal_ = true;
    }
    if (ImGui::Button("Open Project...", wide)) {
        std::string picked;
        const std::string start = std::string(locBuf_);
        if (openFileDialog("Open Aver project", "Aver project (*.ocproject)", "*.ocproject",
                           directoryExists(start) ? start : std::string(), picked)) {
            std::string err;
            if (open(picked, &err)) action = BrowserAction::Open;
            else error_ = err;
        }
    }
    ImGui::Dummy(ImVec2(0, 10.0f * dpi));
    ImGui::TextDisabled("...or type a path");
    ImGui::PushItemWidth(-1);
    const bool submitted = ImGui::InputTextWithHint("##path", "C:\\...\\Name.ocproject", pathBuf_,
                                                    sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    const bool goPressed = ImGui::Button("Open Path", wide);
    if ((submitted || goPressed) && pathBuf_[0]) {
        std::string err;
        if (open(pathBuf_, &err)) action = BrowserAction::Open;
        else error_ = err;
    }
    ImGui::EndChild();

    if (!error_.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", error_.c_str());

    // ---- footer ----
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 38.0f * dpi);
    ImGui::TextDisabled("A project is optional - the editor runs without one.");
    ImGui::SameLine();
    const f32 btn = 150.0f * dpi;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (btn * 2 + 100.0f * dpi + 30.0f * dpi));
    ImGui::BeginDisabled(recentSel_ < 0 || recentSel_ >= (int)recents_.size());
    if (ImGui::Button("Open Selected", ImVec2(btn, 0))) {
        std::string err;
        if (open(recents_[recentSel_], &err)) action = BrowserAction::Open;
        else { error_ = err; forget(recents_[recentSel_]); saveRecents(); recentSel_ = -1; }
    }
    ImGui::EndDisabled();
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
            if (scaffoldProject(locBuf_, nameBuf_, created, &newError_)) {
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
            const std::string copied = copyProjectTree(upgradePath_, std::string(kEngineVersion), &err);
            if (copied.empty()) {
                upgradeError_ = err;
            } else if (!open(copied, &err)) {
                upgradeError_ = err;
            } else {
                cardsDirty_ = true;
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
            if (!open(upgradePath_, &err)) {
                upgradeError_ = err;
            } else {
                cardsDirty_ = true;
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
