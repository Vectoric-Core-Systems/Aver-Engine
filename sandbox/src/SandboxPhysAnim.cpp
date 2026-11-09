// Editor: the Physical Animation section of the Simulate and Animate mode panels (WAVE2_UE W6 step 8). For the selected character: pick a bone, a
// push and a direction, then Apply Hit runs the same RagdollSystem::applyHit a weapon would; Die and Get Up run the life machine. A profile from the
// project can be assigned to the character for the session (it is not saved with the level: the entity gets one by script or graph node).
//
// The feel of a reaction is not judged here: this only reaches the code. It needs the physics world running (Play in Simulate mode) and a character
// that has been posed at least once; the ragdoll system refuses a hit otherwise and the section says so.

#include "SandboxApp.hpp"

#include <filesystem>

#if AVER_MODULE_RAGDOLL

#include "aver/ragdoll/PhysAnimProfile.hpp"

namespace aver {

// A starter .ocphysanim in the selected folder: the engine defaults, no region (the tab adds them).
void SandboxApp::cbCreatePhysAnim() {
    const std::filesystem::path target = cbFreeAssetPath("NewPhysAnim", ".ocphysanim");
    if (target.empty()) return;
    std::string why;
    if (!ragdoll::savePhysAnimProfile(target.string(), editor::physAnimStarter(target.stem().string()), &why)) {
        drawerUi_.cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new physical animation profile failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

}  // namespace aver

#endif

#if AVER_MODULE_RAGDOLL && AVER_MODULE_SCENE && AVER_WITH_IMGUI

#include "PhysAnimEditModel.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/HitEvent.hpp"
#include "aver/ragdoll/RagdollSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace aver {

namespace {

struct HitUi {
    int bone = 0;
    f32 impulse = 3000.0f;
    int direction = 0;       // 0 camera forward, 1 character forward (+Y), 2 up, 3 back
    bool radial = false;
    f32 radius = 150.0f;
    int profileChoice = -1;
    std::vector<std::string> profiles;
    std::string profileRoot;
    std::string status;
};

HitUi& hitUi() {
    static HitUi u;
    return u;
}

void scanProfiles(HitUi& u, const std::string& contentDir) {
    namespace fs = std::filesystem;
    u.profileRoot = contentDir;
    u.profiles.clear();
    u.profileChoice = -1;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(contentDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (u.profiles.size() >= 200) break;
        if (it->is_regular_file(ec) && it->path().extension() == ".ocphysanim") u.profiles.push_back(it->path().string());
    }
    std::sort(u.profiles.begin(), u.profiles.end());
}

}  // namespace

void SandboxApp::buildPhysAnimSection() {
    HitUi& u = hitUi();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("PHYSICAL ANIMATION");
    scene::World& world = scene::World::instance();
    const scene::Entity e = (sel_ == kSelScene && world.valid(selEntity_)) ? selEntity_ : scene::kInvalidEntity;
    const fmt::OcSkeleton* skel = e != scene::kInvalidEntity ? anim::animSystem().posedSkeleton(e) : nullptr;
    if (!skel || skel->bones.empty()) {
        ImGui::TextWrapped("Select a skinned character that has been posed (it plays an animation) to hit it. The physics world has to be running: Play in Simulate mode.");
        return;
    }
    ragdoll::RagdollSystem& rs = ragdoll::ragdollSystem();

    // The profile this character reacts with.
    if (u.profileRoot != project_.contentDir()) scanProfiles(u, project_.contentDir());
    const char* cur = u.profileChoice >= 0 ? u.profiles[static_cast<usize>(u.profileChoice)].c_str() : "(defaults)";
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##physprofile", std::filesystem::path(cur).filename().string().c_str())) {
        if (ImGui::Selectable("(defaults)", u.profileChoice < 0)) { u.profileChoice = -1; rs.setProfile(e, nullptr); u.status = "Using the default profile."; }
        for (int i = 0; i < static_cast<int>(u.profiles.size()); ++i) {
            if (!ImGui::Selectable(std::filesystem::path(u.profiles[static_cast<usize>(i)]).filename().string().c_str(), u.profileChoice == i)) continue;
            auto p = std::make_shared<ragdoll::PhysAnimProfile>();
            std::string why;
            if (ragdoll::loadPhysAnimProfile(u.profiles[static_cast<usize>(i)], *p, &why)) { u.profileChoice = i; rs.setProfile(e, p); u.status = "Profile set for this session."; }
            else u.status = "Could not read the profile: " + why;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The .ocphysanim this character reacts with. Session only, not saved with the level.");

    u.bone = std::max(0, std::min(u.bone, static_cast<int>(skel->bones.size()) - 1));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##hitbone", skel->bones[static_cast<usize>(u.bone)].name.c_str())) {
        for (int i = 0; i < static_cast<int>(skel->bones.size()); ++i)
            if (ImGui::Selectable(skel->bones[static_cast<usize>(i)].name.c_str(), u.bone == i)) u.bone = i;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##hitimpulse", &u.impulse, 100.0f, 20000.0f, "push %.0f kg*cm/s");
    const char* dirs[] = {"From the camera", "Character forward", "Up", "Backwards"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##hitdir", &u.direction, dirs, 4);
    ImGui::Checkbox("Radial (explosion)", &u.radial);
    if (u.radial) { ImGui::SetNextItemWidth(-1.0f); ImGui::SliderFloat("##hitradius", &u.radius, 20.0f, 1000.0f, "reach %.0f cm"); }

    if (ImGui::Button("Apply Hit", ImVec2(-1, 0))) {
        Vec3 dir = camForward();
        if (u.direction == 1) dir = Vec3{0, 1, 0};
        else if (u.direction == 2) dir = Vec3{0, 0, 1};
        else if (u.direction == 3) dir = Vec3{0, -1, 0};
        const Mat4& m = world.worldMatrix(e);
        HitEvent h;
        h.entity = static_cast<u32>(e);
        h.boneHash = fnv1a64(std::string_view(skel->bones[static_cast<usize>(u.bone)].name));
        h.impulse = dir * u.impulse;
        h.point = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
        h.radius = u.radial ? u.radius : 0.0f;
        u.status = rs.applyHit(h) ? "Hit applied." : "Nothing could react: the physics world is not running, or the character has not been posed yet.";
    }
    uiReg_.track("mode.physAnim.applyHit");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs RagdollSystem::applyHit on the selected character: the bone and its neighbours blend into\n"
                          "the simulation along their envelopes, then the animation takes the body back.");
    if (ImGui::Button("Die", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4.0f, 0))) u.status = rs.die(e) ? "Dying." : "Could not start the ragdoll.";
    ImGui::SameLine();
    if (ImGui::Button("Get Up", ImVec2(-1, 0))) u.status = rs.getUp(e) ? "Getting up." : "Not lying dead, or no get-up montage in the profile.";
    if (!u.status.empty()) ImGui::TextWrapped("%s", u.status.c_str());
}

}  // namespace aver

#endif
