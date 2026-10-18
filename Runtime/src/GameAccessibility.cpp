#include "aver/game/GameAccessibility.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/ColourVision.hpp"

#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/AccessKeys.hpp"
#endif

#include <cmath>

namespace aver::game {

void initAccessibility(const AccessibilityInit& in) {
#if AVER_MODULE_FRAMEWORK
    std::string text;
    if (!in.settingsPath.empty()) readFileText(in.settingsPath, text);   // missing is fine: defaults
    const int32_t read = aver_fw_access_load_ini(text.c_str());
    int32_t cues = -1;
    if (!in.contentDir.empty()) cues = aver_fw_caption_load_dir((in.contentDir + AVER_PATH_SEP "Captions").c_str());
    AVER_INFO("[Access] {} option(s) read from '{}'; {} caption cue(s)", read, in.settingsPath, cues < 0 ? 0 : cues);
#else
    (void)in;
#endif
}

AccessSnapshot accessibilitySnapshot() {
    AccessSnapshot s;
#if AVER_MODULE_FRAMEWORK
    float e[8] = {};
    if (aver_fw_access_effective(e)) {
        s.colourVisionPacked = e[0]; s.uiScale = e[1]; s.textScale = e[2]; s.highContrast = e[3];
        s.shake = e[4]; s.bob = e[5]; s.motionBlurAllowed = e[6]; s.lensAllowed = e[7];
    }
#endif
    return s;
}

void applyAccessibilityToPost(rhi::PostSettings& p, const AccessSnapshot& s) {
    if (s.colourVisionPacked > 0.5f) {
        p.colourVision = rhi::colourVisionMode(s.colourVisionPacked) | (rhi::colourVisionSimulate(s.colourVisionPacked) ? 4u : 0u);
        p.colourVisionStrength = rhi::colourVisionStrength(s.colourVisionPacked);
    } else {
        p.colourVision = 0;
    }
    if (s.motionBlurAllowed < 0.5f) p.motionBlur = 0.0f;
    if (s.lensAllowed < 0.5f) { p.chromaticAberration = 0.0f; p.grain = 0.0f; p.lensFlare = 0.0f; }
}

void accessibilityFrame(f32 dt, const f32 pos[3], const f32 fwd[3], const f32 right[3], AccessFrame& out) {
    static u32 s_revision = ~0u;
    out.captions.clear();
#if AVER_MODULE_FRAMEWORK
    out.snap = accessibilitySnapshot();
    const u32 rev = static_cast<u32>(aver_fw_access_revision());
    out.changed = rev != s_revision;
    s_revision = rev;

    aver_fw_caption_update(dt, pos, fwd, right);
    const int32_t n = aver_fw_caption_line_count();
    for (int32_t i = 0; i < n; ++i) {
        AverFwCaptionLine l{};
        if (!aver_fw_caption_line_at(i, &l)) continue;
        AccessCaption c;
        c.speaker = l.speaker ? l.speaker : "";
        c.text = l.text ? l.text : "";
        c.rgba = l.rgba;
        c.dir = l.dir;
        c.alpha = l.alpha;
        out.captions.push_back(std::move(c));
    }
    out.captionSizeScale = fw::access::captionSizeScale(aver_fw_access_get("access.captions.size"));
    out.captionBgOpacity = aver_fw_access_get("access.captions.bgOpacity");
    out.captionsTop = aver_fw_access_get("access.captions.position") > 0.5f;
    aver_fw_speech_tick();
#else
    (void)dt; (void)pos; (void)fwd; (void)right;
    out.snap = AccessSnapshot{};
    out.changed = s_revision == ~0u;
    s_revision = 0;
#endif
}

void accessibilityFrame(f32 dt, const Vec3& camPos, f32 yaw, f32 pitch, AccessFrame& out) {
    const f32 pos[3] = {camPos.x, camPos.y, camPos.z};
    const f32 fwd[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const f32 right[3] = {-std::sin(yaw), std::cos(yaw), 0.0f};
    accessibilityFrame(dt, pos, fwd, right, out);
}

void applyAccessibilityCamera(Vec3& pos, f32& yaw, f32& pitch, f32 dt) {
#if AVER_MODULE_FRAMEWORK
    float p[3] = {pos.x, pos.y, pos.z};
    float yp[2] = {yaw, pitch};
    if (aver_fw_camera_fx_apply(dt, p, yp)) {
        pos = Vec3{p[0], p[1], p[2]};
        yaw = yp[0];
        pitch = yp[1];
    }
#else
    (void)pos; (void)yaw; (void)pitch; (void)dt;
#endif
}

} // namespace aver::game
