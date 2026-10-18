#ifndef AVER_FRAMEWORK_ACCESS_ABI_H
#define AVER_FRAMEWORK_ACCESS_ABI_H

/* Accessibility C ABI (Aver.Framework): the option store, captions, speech and camera feel.
 * Separate header with its own version so framework_abi.h is untouched (framework_loc_abi.h is the precedent).
 * Conventions as there: int32_t/float/const char* only, UTF-8, single game thread, a returned const char* is valid
 * until the next call that returns a string. All state lives in this DLL, so no provider relay is needed.
 * Semantics: docs/ACCESSIBILITY.md. */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_ACCESS_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_access_abi_version(void);

/* ---- options ---- */

/* Settings.ini text (key=value lines): every known key is applied, the rest take their defaults, unknown keys are
 * ignored. Returns the number of keys read. Not marked dirty. Bumps the revision. */
AVER_FW_ABI int32_t     aver_fw_access_load_ini(const char* utf8Text);
AVER_FW_ABI int32_t     aver_fw_access_key_count(void);                  /* the static table; dynamic toggles are not listed */
AVER_FW_ABI const char* aver_fw_access_key_at(int32_t i);                /* "" out of range */
/* out6 = kind (0 toggle, 1 slider, 2 choice), min, max, step, default, current. 0 for an unknown key. */
AVER_FW_ABI int32_t     aver_fw_access_key_info(const char* key, float* out6);
AVER_FW_ABI const char* aver_fw_access_key_choices(const char* key);     /* newline-separated loc refs, "" if none */
AVER_FW_ABI float       aver_fw_access_get(const char* key);             /* 0 for an unknown key */
/* 1 accepted (clamped and snapped), 0 unknown key. A changed value marks the key dirty, bumps the revision and posts
 * the framework event "Access.Changed" (string key, float value). */
AVER_FW_ABI int32_t     aver_fw_access_set(const char* key, float value);
AVER_FW_ABI void        aver_fw_access_reset_defaults(void);
AVER_FW_ABI int32_t     aver_fw_access_revision(void);
/* Drains one accepted change per call (i is reserved, pass 0): copies the key into keyOut (cap bytes) and the value
 * into valueOut. 0 when nothing is pending. */
AVER_FW_ABI int32_t     aver_fw_access_take_dirty(int32_t i, char* keyOut, int32_t cap, float* valueOut);
/* false hides an action's toggle-instead-of-hold from the player and ignores a saved choice. */
AVER_FW_ABI void        aver_fw_access_set_toggle_allowed(const char* utf8Action, int32_t allowed);
AVER_FW_ABI int32_t     aver_fw_access_toggle_allowed(const char* utf8Action);
/* out8 = colourVisionPacked, uiScale, textScale, highContrast, shake, bob, motionBlurAllowed, lensAllowed
 * (reduce motion already applied). */
AVER_FW_ABI int32_t     aver_fw_access_effective(float* out8);

/* ---- captions ---- */

/* Loads every *.occaptions under utf8Dir in name order (a later duplicate cue id replaces the earlier one) and
 * replaces the cue set. Returns the cue count, -1 if the folder is missing (the set is left empty). */
AVER_FW_ABI int32_t     aver_fw_caption_load_dir(const char* utf8Dir);
AVER_FW_ABI int32_t     aver_fw_caption_reload_path(const char* utf8AbsPath);   /* 1 if it was a caption file (hot reload) */
AVER_FW_ABI void        aver_fw_caption_clear(void);                             /* drops cues and active captions */
AVER_FW_ABI int32_t     aver_fw_caption_cue_count(void);
AVER_FW_ABI int32_t     aver_fw_caption_show(const char* cue, int32_t positional, float x, float y, float z);   /* 1 shown */
AVER_FW_ABI int32_t     aver_fw_caption_show_text(const char* speakerRef, const char* textRef, float seconds, uint32_t rgba, int32_t kind);
AVER_FW_ABI void        aver_fw_caption_stop(const char* cue);                   /* null or "" = all */
/* AverAudioPlayObserver (audio_abi.h): the host installs it with aver_audio_set_play_observer. */
AVER_FW_ABI void        aver_fw_access_audio_observer(void* user, const char* path, int32_t voice, int32_t bus, int32_t positional,
                                                      float x, float y, float z, int32_t looping);
/* alive(voice) != 0 while the voice plays (the host passes aver_audio_playing). Without one, voice-bound lines time out. */
AVER_FW_ABI void        aver_fw_caption_set_voice_probe(int32_t (*alive)(int32_t voice));
/* dt is the UNSCALED frame time. The listener arrays are xyz (null = origin, +X forward, +Y right). Also narrates new
 * lines when access.speech.captions is on. */
AVER_FW_ABI void        aver_fw_caption_update(float dt, const float* listenerPos3, const float* listenerFwd3, const float* listenerRight3);
typedef struct AverFwCaptionLine { const char* speaker; const char* text; uint32_t rgba; int32_t kind; int32_t dir; float alpha; } AverFwCaptionLine;
AVER_FW_ABI int32_t     aver_fw_caption_line_count(void);
AVER_FW_ABI int32_t     aver_fw_caption_line_at(int32_t i, AverFwCaptionLine* out);      /* raw refs; valid until the next update */
AVER_FW_ABI int32_t     aver_fw_caption_new_line_at(int32_t i, AverFwCaptionLine* out);  /* lines that appeared this update */
AVER_FW_ABI int32_t     aver_fw_caption_new_line_count(void);

/* ---- speech ---- */

AVER_FW_ABI int32_t     aver_fw_speech_available(void);                                  /* an OS voice exists (may block briefly the first time) */
AVER_FW_ABI int32_t     aver_fw_speech_say(const char* utf8OrRef, int32_t interrupt);    /* source "script"; gated by access.speech.enabled */
AVER_FW_ABI int32_t     aver_fw_speech_announce_ui(const char* utf8, int32_t interrupt); /* AverUiAnnounceFn: source "ui" */
AVER_FW_ABI void        aver_fw_speech_stop(void);
AVER_FW_ABI int32_t     aver_fw_speech_speaking(void);
AVER_FW_ABI void        aver_fw_speech_tick(void);                                       /* applies rate/volume on a revision change; feeds the queue */
AVER_FW_ABI int32_t     aver_fw_speech_os_screen_reader(void);

/* ---- camera feel ---- */

AVER_FW_ABI void        aver_fw_camera_shake_add(float trauma);
AVER_FW_ABI void        aver_fw_camera_bob_set(float amount01, float speedCmPerSec);
/* Applies shake and bob (the effective scales) to pos (xyz) and yaw/pitch (radians). 1 if anything changed. */
AVER_FW_ABI int32_t     aver_fw_camera_fx_apply(float dt, float* inoutPos3, float* inoutYawPitchRad2);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ACCESS_ABI_H */
