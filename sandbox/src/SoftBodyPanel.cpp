// The plastic soft-body test panel. See the header for the three-line hook.
#include "SoftBodyPanel.hpp"

#include <algorithm>
#include <cmath>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

void softBodyPanelRebuild(SoftBodyPanelState& s) {
    softbody::Cage c;
    const u32 m = softbody::addMaterial(c, s.material);
    const u32 bays = 5;
    const f32 bay = s.barLengthCm / static_cast<f32>(bays);
    const f32 height = bay;
    std::vector<u32> top, bot;
    for (u32 i = 0; i <= bays; ++i) {
        bot.push_back(softbody::addParticle(c, Vec3{bay * static_cast<f32>(i), 0, 0}, i == 0));
        top.push_back(softbody::addParticle(c, Vec3{bay * static_cast<f32>(i), 0, height}, i == 0));
    }
    for (u32 i = 0; i <= bays; ++i) {
        softbody::addBeam(c, bot[i], top[i], m);
        if (i == bays) break;
        softbody::addBeam(c, bot[i], bot[i + 1], m);
        softbody::addBeam(c, top[i], top[i + 1], m);
        softbody::addBeam(c, bot[i], top[i + 1], m);
        softbody::addBeam(c, top[i], bot[i + 1], m);
    }
    softbody::build(c);
    s.cage = std::move(c);
    s.tipTop = top.back();
    s.tipBot = bot.back();
    s.built = true;
    s.settled = true;
    s.accumulator = 0.0f;
}

void softBodyPanelHit(SoftBodyPanelState& s, f32 along) {
    if (!s.built) softBodyPanelRebuild(s);
    softbody::Impact im;
    im.point = Vec3{s.barLengthCm * std::clamp(along, 0.0f, 1.0f), 0, 10.0f};
    im.direction = Vec3{0, 0, -1};
    im.depthCm = s.hitDepthCm;
    im.radiusCm = s.barLengthCm * 0.45f;
    softbody::applyImpact(s.cage, im);
    s.settled = false;
}

void softBodyPanelRepair(SoftBodyPanelState& s) {
    if (!s.built) return;
    softbody::repair(s.cage);
    s.settled = false;
}

int softBodyPanelAdvance(SoftBodyPanelState& s, f32 realDt) {
    if (!s.built || !s.running || s.settled) return 0;
    s.accumulator += std::clamp(realDt, 0.0f, 0.25f);
    int ran = 0;
    while (s.accumulator >= s.cfg.dt && ran < 4) {
        const softbody::StepResult r = softbody::step(s.cage, s.cfg);
        s.accumulator -= s.cfg.dt;
        ++ran;
        if (r.settled) { s.settled = true; s.accumulator = 0.0f; break; }
    }
    return ran;
}

f32 softBodyPanelMaxSet(const SoftBodyPanelState& s) {
    f32 worst = 0.0f;
    for (const softbody::Beam& b : s.cage.beams) worst = std::max(worst, b.plastic);
    return worst;
}

#if AVER_WITH_IMGUI

void softBodyPanelDraw(SoftBodyPanelState& s, bool* open) {
    if (open && !*open) return;
    if (!ImGui::Begin("Soft Body (plastic)", open)) { ImGui::End(); return; }
    if (!s.built) softBodyPanelRebuild(s);
    softBodyPanelAdvance(s, ImGui::GetIO().DeltaTime);

    bool changed = false;
    softbody::Material& m = s.material;
    changed |= ImGui::SliderFloat("Yield force (N)", &m.bendForceN, 100.0f, 20000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Break force (N)", &m.breakForceN, 1000.0f, 60000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Plastic stiffness (N/cm)", &m.plasticStiffness, 50.0f, 5000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Max set before tear (cm)", &m.maxBend, 0.5f, 40.0f, "%.1f");
    changed |= ImGui::SliderFloat("Break strain (0 = off)", &m.breakStrain, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Hardening", &m.hardening, 0.0f, 2.0f, "%.2f");
    int behavior = static_cast<int>(m.behavior);
    if (ImGui::Combo("Behavior", &behavior, "Deform (bends)\0Fracture (snaps)\0Shatter (glass)\0")) {
        m.behavior = static_cast<softbody::Behavior>(behavior);
        changed = true;
    }
    if (changed) softBodyPanelRebuild(s);

    ImGui::SliderFloat("Hit depth (cm)", &s.hitDepthCm, 1.0f, 100.0f, "%.0f");
    if (ImGui::Button("Hit tip"))    softBodyPanelHit(s, 1.0f);
    ImGui::SameLine();
    if (ImGui::Button("Hit middle")) softBodyPanelHit(s, 0.5f);
    ImGui::SameLine();
    if (ImGui::Button("Repair"))     softBodyPanelRepair(s);
    ImGui::SameLine();
    ImGui::Checkbox("Run", &s.running);

    ImGui::Text("permanent set %.2f cm   broken %u   pieces %u   %s", softBodyPanelMaxSet(s),
                softbody::brokenBeamCount(s.cage), softbody::countPieces(s.cage),
                s.settled ? "settled" : "moving");

    // Side view: X across, Z up.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 avail = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const f32 scale = std::min(avail * 0.9f / s.barLengthCm, 4.0f);
    ImGui::Dummy(ImVec2(avail, s.barLengthCm * scale * 0.9f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 base(origin.x + 12.0f, origin.y + s.barLengthCm * scale * 0.55f);
    auto toScreen = [&](const Vec3& p) { return ImVec2(base.x + p.x * scale, base.y - p.z * scale); };
    for (const softbody::Beam& b : s.cage.beams) {
        if (b.broken) continue;
        const softbody::Material& bm = s.cage.materials[b.material];
        const f32 creep = std::clamp(b.plastic / std::max(bm.maxBend, 0.01f), 0.0f, 1.0f);
        const ImU32 col = IM_COL32(static_cast<int>(80 + 175 * creep), static_cast<int>(200 - 140 * creep), 90, 255);
        dl->AddLine(toScreen(s.cage.particles[b.a].pos), toScreen(s.cage.particles[b.b].pos), col, 2.0f);
    }
    for (const softbody::Particle& p : s.cage.particles)
        dl->AddCircleFilled(toScreen(p.pos), p.pinned ? 4.0f : 2.5f, p.pinned ? IM_COL32(220, 220, 80, 255) : IM_COL32(230, 230, 230, 255));
    ImGui::End();
}

#else

void softBodyPanelDraw(SoftBodyPanelState&, bool*) {}

#endif

} // namespace aver::editor
