// Accessibility host wiring shared by the game and the editor (GameLocalization's arrangement): the options are read
// from Settings.ini before the first frame, captions and narration run once a frame, and the player's choices are
// applied to the post settings and the camera AFTER the level's own blend, so a level cannot undo them. The state
// lives in Aver.Framework (framework_access_abi.h); this file is the glue each host calls. The UI and audio calls
// are header-only and compiled where AVER_WITH_UI_ABI / AVER_WITH_AUDIO_ABI are defined (the hosts, not Core).
// docs/ACCESSIBILITY.md section 3.3.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

#ifndef AVER_MODULE_FRAMEWORK
#  define AVER_MODULE_FRAMEWORK 0
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_access_abi.h"
#endif
#if AVER_WITH_UI_ABI
#  include "aver/ui/ui_widget_abi.h"
#endif
#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif

namespace aver::game {

struct AccessibilityInit {
    std::string settingsPath;   // runtime: userDataDir()\<Project>\Settings.ini ; editor: <project>\Saved\Settings.ini
    std::string contentDir;     // the project's Content folder: captions come from <content>/Captions
};

// The effective options for one frame ("reduce motion" already applied).
struct AccessSnapshot {
    f32 colourVisionPacked = 0;   // rhi::packColourVision; 0 = off
    f32 uiScale = 1, textScale = 1, highContrast = 0;
    f32 shake = 1, bob = 1;                       // camera feel scales
    f32 motionBlurAllowed = 1, lensAllowed = 1;   // 0 = the player turned it off (or reduce motion)
};

// What a frame produced for the UI.
struct AccessCaption {
    std::string speaker, text;    // raw references; the UI tree localizes them
    u32 rgba = 0xFFFFFFFFu;       // 0xRRGGBBAA
    i32 dir = 0;                  // -1 left, 0 none, 1 right, 2 behind
    f32 alpha = 1.0f;
};
struct AccessFrame {
    AccessSnapshot snap;
    bool changed = true;          // the options moved since the previous frame (true on the first)
    std::vector<AccessCaption> captions;
    f32 captionSizeScale = 1.0f, captionBgOpacity = 0.6f;
    bool captionsTop = false;
};

// Loads the options from the INI text (defaults when the file is missing, narration defaulting on when the OS
// screen-reader flag is set), then the project's Captions folder. Safe to call again for another project. A build
// without the framework does nothing.
void initAccessibility(const AccessibilityInit&);

// The effective options now.
AccessSnapshot accessibilitySnapshot();

// Pure: the player's colour filter, and the motion-effect switches, over blended post settings. A snapshot at its
// defaults changes nothing.
void applyAccessibilityToPost(rhi::PostSettings&, const AccessSnapshot&);

// Once a frame, after the game's own update. `unscaledDt` is the real frame time (captions follow audio, which keeps
// playing while the game is paused). The listener is the camera: position and unit forward/right vectors.
void accessibilityFrame(f32 unscaledDt, const f32 listenerPos[3], const f32 listenerFwd[3], const f32 listenerRight[3],
                        AccessFrame& out);
// The same from a camera pose (yaw/pitch in radians, +X forward at yaw 0, +Y right).
void accessibilityFrame(f32 unscaledDt, const Vec3& camPos, f32 yaw, f32 pitch, AccessFrame& out);

// Camera shake and head bob over the final camera, with the effective scales. Identity when nothing is active.
void applyAccessibilityCamera(Vec3& pos, f32& yaw, f32& pitch, f32 dt);

#if AVER_WITH_UI_ABI
// Pushes text scale, high contrast, UI scale and the caption lines into the UI ABI. Call BEFORE aver_ui_widgets_frame.
inline void pushAccessibilityToUi(const AccessFrame& f) {
    if (f.changed) {
        aver_ui_set_access(f.snap.textScale, f.snap.highContrast > 0.5f ? 1 : 0);
        aver_ui_set_dpi(-1, 0.0f, 0.0f, f.snap.uiScale);   // mode and reference sizes untouched, userScale only
    }
    static std::vector<AverUiCaptionLine> lines;
    lines.clear();
    for (const AccessCaption& c : f.captions) lines.push_back(AverUiCaptionLine{c.speaker.c_str(), c.text.c_str(), c.rgba, c.dir, c.alpha});
    aver_ui_captions_set(lines.data(), static_cast<int32_t>(lines.size()), f.captionSizeScale, f.captionBgOpacity, f.captionsTop ? 1 : 0);
}
#  if AVER_MODULE_FRAMEWORK
// Narrates focus and value changes (after the text-resolver install).
inline void installAccessibilityUi() { aver_ui_set_announcer(&aver_fw_speech_announce_ui); }
#  endif
#endif

#if AVER_WITH_AUDIO_ABI && AVER_MODULE_FRAMEWORK
// Captions from every sound the audio ABI starts, and voices that end cut their lines (after aver_audio_init).
inline void installAccessibilityAudio() {
    aver_audio_set_play_observer(&aver_fw_access_audio_observer, nullptr);
    aver_fw_caption_set_voice_probe(&aver_audio_playing);
}
#endif

} // namespace aver::game
