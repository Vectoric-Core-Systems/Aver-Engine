#include "ProjectBrowser.hpp"
#include "ProjectScaffold.hpp"

#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"
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

// "C:\...\OpenConstructor\OpenConstructor.ocproject" -> "OpenConstructor"
std::string displayName(const std::string& manifestPath) {
    return std::filesystem::path(manifestPath).stem().string();
}

void setBuf(char* dst, usize cap, const std::string& s) {
    const usize n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

} // namespace

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
            // A project the user moved or deleted is not a recent project, it is a dead row that
            // errors when clicked, so it is dropped on read rather than shown and then refused.
            if (!line.empty() && fileExists(line)) recents_.push_back(line);
        }
    }

    // Default the New Project location to the projects root beside the engine, per the
    // engine-perpendicular-to-projects layout: projects are SIBLINGS of the engine, never inside.
    setBuf(locBuf_, sizeof locBuf_, documentsDir() + "\\Aver Projects");
}

void ProjectBrowser::saveRecents() const {
    createDirectories(userDataDir());
    std::string text;
    for (const std::string& p : recents_) text += p + "\n";
    if (!writeFileText(recentsPath(), text))
        AVER_WARN("[Editor] could not write the recent project list to {}", recentsPath());
}

void ProjectBrowser::forget(const std::string& manifestPath) {
    for (usize i = 0; i < recents_.size(); ++i) {
        if (recents_[i] == manifestPath) { recents_.erase(recents_.begin() + static_cast<isize>(i)); return; }
    }
}

bool ProjectBrowser::open(const std::string& manifestPath, std::string* err) {
    fmt::ProjectDesc desc;
    if (!fmt::loadOcproject(manifestPath, desc, err)) return false;
    project_ = desc;

    // Store the absolute path the loader resolved, so a project opened via a relative argument
    // and the same project opened from the browser are one entry, not two.
    forget(project_.manifestPath);
    recents_.insert(recents_.begin(), project_.manifestPath);
    if (recents_.size() > kMaxRecents) recents_.resize(kMaxRecents);
    saveRecents();

    AVER_INFO("[Editor] project '{}' loaded from {}", project_.name, project_.manifestPath);
    return true;
}

BrowserAction ProjectBrowser::draw(f32 dpi, ImFont* medium) {
#if !AVER_WITH_IMGUI
    (void)dpi; (void)medium;
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
    // Fully opaque: the 3D scene is still rasterising behind this and must not show through.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.086f, 0.086f, 0.094f, 1.0f));
    ImGui::Begin("##projectbrowser", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- branding ----
    // Drawn, not blitted: branding/logo.png is a raster and the RHI has no texture-upload path to
    // ImGui, so the mark is vector-drawn here rather than adding a texture API for one image.
    {
        const f32 badge = 46.0f * dpi;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + badge, p.y + badge), ImGui::GetColorU32(accent), 8.0f * dpi);
        const char* mark = "AE";
        const ImVec2 ts = ImGui::CalcTextSize(mark);
        dl->AddText(ImVec2(p.x + (badge - ts.x) * 0.5f, p.y + (badge - ts.y) * 0.5f),
                    IM_COL32(20, 18, 16, 255), mark);
        ImGui::Dummy(ImVec2(badge, badge));
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
    if (recents_.empty()) {
        ImGui::Dummy(ImVec2(0, 6.0f * dpi));
        ImGui::TextDisabled("  No projects opened yet.");
        ImGui::TextDisabled("  Use New Project to scaffold one, or Open Project to");
        ImGui::TextDisabled("  point the editor at an existing .ocproject.");
    }
    for (int i = 0; i < (int)recents_.size(); ++i) {
        ImGui::PushID(i);
        const std::string label = "  " + displayName(recents_[i]);
        // Two lines of text in one row, so the row is sized for both and the name sits on the
        // upper half rather than being vertically centred over the path.
        if (ImGui::Selectable(label.c_str(), recentSel_ == i,
                              ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 40.0f * dpi))) {
            recentSel_ = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                std::string err;
                if (open(recents_[i], &err)) action = BrowserAction::Open;
                else { error_ = err; forget(recents_[i]); saveRecents(); recentSel_ = -1; }
            }
        }
        // The path under the name: two projects can share a name, and the folder is the only
        // thing that tells them apart.
        const ImVec2 rmin = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rmin.x + 12.0f * dpi, rmin.y + 20.0f * dpi),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), recents_[i].c_str());
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
    // Opened here, inside the browser window's ID stack, so BeginPopupModal below finds it.
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
            // No folder picker in the platform layer, so this picks a manifest and takes the
            // folder that CONTAINS its project folder - i.e. the projects root.
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

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return action;
#endif
}

} // namespace aver::editor
