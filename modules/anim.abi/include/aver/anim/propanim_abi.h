#pragma once
/* Aver.Anim.Abi -- the plain-C seam onto property-track animation (aver/anim/PropAnim.hpp), so scripts and
 * managed extensions can drive it. Conventions as docs/ABI.md section 13: setters return 1 on success and 0 on a rejected request
 * (stale handle, bad index), getters return the value or a neutral one (0, -1 for "none", "" for text) for a stale handle.
 * Handles are int32, 0 invalid. Single game thread. Every const char* return is valid until the next call into this library.
 *
 * VALUES. A value crosses as double v[5] = {kind, n0, n1, n2, n3} plus a string: kind is aver::fmt::OcPropKind (0 nil, 1 bool,
 * 2 int, 3 float, 4 vec2, 5 vec3, 6 vec4, 7 quat, 8 color, 9 string, 10 text); the string is the value for kinds 9 and 10 (text
 * is a variant in its text form).
 * ENUMS (mirrored by hand in the extension's managed half, extensions/FabricConnector/managed/Aver.Fabric/Nodes/AnimationNative.cs, and checked by AbiEnumTest):
 *   track type 0 value, 1 position3d, 2 rotation3d, 3 scale3d, 4 blend_shape, 5 method, 6 bezier, 7 audio, 8 animation
 *   interpolation 0 nearest, 1 linear, 2 cubic, 3 linear_angle, 4 cubic_angle
 *   update 0 continuous, 1 discrete, 2 capture;  loop 0 none, 1 linear, 2 ping-pong;  discrete mode 0 dominant, 1 recessive, 2 force
 *   event 0 started, 1 finished, 2 changed, 3 current_changed, 4 looped, 5 method, 6 audio_play, 7 audio_stop, 8 anim_play,
 *        9 anim_stop, 10 applied, 11 mixer_applied
 */
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PANIM_ABI_BUILD)
#    define AVER_PANIM_API __declspec(dllexport)
#  else
#    define AVER_PANIM_API __declspec(dllimport)
#  endif
#else
#  define AVER_PANIM_API
#endif

#define AVER_PANIM_ABI_VERSION 1

#define AVER_PANIM_KIND_NIL    0
#define AVER_PANIM_KIND_BOOL   1
#define AVER_PANIM_KIND_INT    2
#define AVER_PANIM_KIND_FLOAT  3
#define AVER_PANIM_KIND_VEC2   4
#define AVER_PANIM_KIND_VEC3   5
#define AVER_PANIM_KIND_VEC4   6
#define AVER_PANIM_KIND_QUAT   7
#define AVER_PANIM_KIND_COLOR  8
#define AVER_PANIM_KIND_STRING 9
#define AVER_PANIM_KIND_TEXT   10

#define AVER_PANIM_TRACK_VALUE      0
#define AVER_PANIM_TRACK_POSITION3D 1
#define AVER_PANIM_TRACK_ROTATION3D 2
#define AVER_PANIM_TRACK_SCALE3D    3
#define AVER_PANIM_TRACK_BLEND_SHAPE 4
#define AVER_PANIM_TRACK_METHOD     5
#define AVER_PANIM_TRACK_BEZIER     6
#define AVER_PANIM_TRACK_AUDIO      7
#define AVER_PANIM_TRACK_ANIMATION  8

#define AVER_PANIM_INTERP_NEAREST      0
#define AVER_PANIM_INTERP_LINEAR       1
#define AVER_PANIM_INTERP_CUBIC        2
#define AVER_PANIM_INTERP_LINEAR_ANGLE 3
#define AVER_PANIM_INTERP_CUBIC_ANGLE  4

#define AVER_PANIM_UPDATE_CONTINUOUS 0
#define AVER_PANIM_UPDATE_DISCRETE   1
#define AVER_PANIM_UPDATE_CAPTURE    2

#define AVER_PANIM_LOOP_NONE     0
#define AVER_PANIM_LOOP_LINEAR   1
#define AVER_PANIM_LOOP_PINGPONG 2

#define AVER_PANIM_DISCRETE_DOMINANT   0
#define AVER_PANIM_DISCRETE_RECESSIVE  1
#define AVER_PANIM_DISCRETE_FORCE      2

#define AVER_PANIM_EVENT_STARTED         0
#define AVER_PANIM_EVENT_FINISHED        1
#define AVER_PANIM_EVENT_CHANGED         2
#define AVER_PANIM_EVENT_CURRENT_CHANGED 3
#define AVER_PANIM_EVENT_LOOPED          4
#define AVER_PANIM_EVENT_METHOD          5
#define AVER_PANIM_EVENT_AUDIO_PLAY      6
#define AVER_PANIM_EVENT_AUDIO_STOP      7
#define AVER_PANIM_EVENT_ANIM_PLAY       8
#define AVER_PANIM_EVENT_ANIM_STOP       9
#define AVER_PANIM_EVENT_APPLIED         10
#define AVER_PANIM_EVENT_MIXER_APPLIED   11

#ifdef __cplusplus
extern "C" {
#endif

AVER_PANIM_API int32_t aver_panim_abi_version(void);

/* ---- a clip (an Animation resource) -------------------------------------------------------------------------------- */
AVER_PANIM_API int32_t aver_panim_create(void);
AVER_PANIM_API void    aver_panim_destroy(int32_t clip);                 /* players that hold the clip keep it alive */
AVER_PANIM_API int32_t aver_panim_clone(int32_t clip);                   /* a deep copy; 0 for a stale handle */
AVER_PANIM_API int32_t aver_panim_load(const char* path);                /* an .ocanim file; 0 when unreadable */
AVER_PANIM_API int32_t aver_panim_save(int32_t clip, const char* path);
AVER_PANIM_API int32_t aver_panim_set_length(int32_t clip, double seconds);
AVER_PANIM_API double  aver_panim_length(int32_t clip);                  /* 0 for a stale handle */
AVER_PANIM_API int32_t aver_panim_set_loop_mode(int32_t clip, int32_t mode);
AVER_PANIM_API int32_t aver_panim_loop_mode(int32_t clip);
AVER_PANIM_API int32_t aver_panim_set_step(int32_t clip, double seconds);
AVER_PANIM_API double  aver_panim_step(int32_t clip);

AVER_PANIM_API int32_t aver_panim_track_count(int32_t clip);
AVER_PANIM_API int32_t aver_panim_track_add(int32_t clip, int32_t type, int32_t atPosition); /* index, -1 on failure; atPosition < 0 appends */
AVER_PANIM_API int32_t aver_panim_track_remove(int32_t clip, int32_t track);
AVER_PANIM_API int32_t aver_panim_track_move(int32_t clip, int32_t track, int32_t toPosition);
AVER_PANIM_API int32_t aver_panim_track_type(int32_t clip, int32_t track);   /* -1 for a bad index */
AVER_PANIM_API int32_t aver_panim_track_set_path(int32_t clip, int32_t track, const char* path);
AVER_PANIM_API const char* aver_panim_track_path(int32_t clip, int32_t track);
AVER_PANIM_API int32_t aver_panim_track_set_interp(int32_t clip, int32_t track, int32_t interp);
AVER_PANIM_API int32_t aver_panim_track_interp(int32_t clip, int32_t track);
AVER_PANIM_API int32_t aver_panim_track_set_update(int32_t clip, int32_t track, int32_t update);
AVER_PANIM_API int32_t aver_panim_track_update(int32_t clip, int32_t track);
AVER_PANIM_API int32_t aver_panim_track_set_flags(int32_t clip, int32_t track, int32_t loopWrap, int32_t enabled, int32_t imported);
AVER_PANIM_API int32_t aver_panim_track_flags(int32_t clip, int32_t track);  /* bit 0 loop_wrap, 1 enabled, 2 imported; -1 for a bad index */

AVER_PANIM_API int32_t aver_panim_key_count(int32_t clip, int32_t track);
/* Inserts (or replaces the key within 1e-5 s of `time`); returns the key index, -1 on failure. */
AVER_PANIM_API int32_t aver_panim_key_insert(int32_t clip, int32_t track, double time, const double* v5, const char* text, double transition);
AVER_PANIM_API int32_t aver_panim_key_remove(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API double  aver_panim_key_time(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API int32_t aver_panim_key_set_time(int32_t clip, int32_t track, int32_t key, double time);   /* re-sorts; returns the new index, -1 on failure */
AVER_PANIM_API double  aver_panim_key_transition(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API int32_t aver_panim_key_set_transition(int32_t clip, int32_t track, int32_t key, double transition);
AVER_PANIM_API int32_t aver_panim_key_value(int32_t clip, int32_t track, int32_t key, double* v5);        /* v5 gets 5 doubles */
AVER_PANIM_API const char* aver_panim_key_text(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API int32_t aver_panim_key_set_value(int32_t clip, int32_t track, int32_t key, const double* v5, const char* text);
/* mode 0 nearest (the last key at or before `time`, going backward the first at or after), 1 or 2 exact; limit keeps to keys inside the clip. -1 when none. */
AVER_PANIM_API int32_t aver_panim_key_find(int32_t clip, int32_t track, double time, int32_t mode, int32_t backward, int32_t limit);
/* The value of a value / transform / blend-shape / bezier track at `time` (its interpolation, update mode and the clip's loop mode applied);
 * 0 when the track is empty. The string of a text value is read with aver_panim_sample_text straight after. */
AVER_PANIM_API int32_t aver_panim_sample(int32_t clip, int32_t track, double time, int32_t backward, double* v5);
AVER_PANIM_API const char* aver_panim_sample_text(void);
AVER_PANIM_API int32_t aver_panim_key_set_handles(int32_t clip, int32_t track, int32_t key, double inX, double inY, double outX, double outY);
AVER_PANIM_API int32_t aver_panim_key_handles(int32_t clip, int32_t track, int32_t key, double* out4);
AVER_PANIM_API int32_t aver_panim_key_arg_count(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API int32_t aver_panim_key_arg_set(int32_t clip, int32_t track, int32_t key, int32_t index, const double* v5, const char* text); /* index == count appends */
AVER_PANIM_API int32_t aver_panim_key_arg_get(int32_t clip, int32_t track, int32_t key, int32_t index, double* v5);
AVER_PANIM_API const char* aver_panim_key_arg_text(int32_t clip, int32_t track, int32_t key, int32_t index);
AVER_PANIM_API int32_t aver_panim_key_args_clear(int32_t clip, int32_t track, int32_t key);
AVER_PANIM_API int32_t aver_panim_key_set_offsets(int32_t clip, int32_t track, int32_t key, double start, double end);
AVER_PANIM_API int32_t aver_panim_key_offsets(int32_t clip, int32_t track, int32_t key, double* out2);

/* ---- a player (the engine half of AnimationPlayer) ------------------------------------------------------------------ */
AVER_PANIM_API int32_t aver_pplayer_create(void);
AVER_PANIM_API void    aver_pplayer_destroy(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_set_clip(int32_t player, const char* key, int32_t clip);    /* shares the clip with the caller */
AVER_PANIM_API int32_t aver_pplayer_remove_clip(int32_t player, const char* key);
AVER_PANIM_API int32_t aver_pplayer_has_clip(int32_t player, const char* key);
AVER_PANIM_API int32_t aver_pplayer_clip_count(int32_t player);
AVER_PANIM_API const char* aver_pplayer_clip_key(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_set_blend_time(int32_t player, const char* from, const char* to, double seconds);
AVER_PANIM_API double  aver_pplayer_blend_time(int32_t player, const char* from, const char* to);   /* the blend time: 0 when none */
AVER_PANIM_API int32_t aver_pplayer_set_next(int32_t player, const char* from, const char* to);
AVER_PANIM_API const char* aver_pplayer_next(int32_t player, const char* from);

AVER_PANIM_API int32_t aver_pplayer_set_speed_scale(int32_t player, double scale);
AVER_PANIM_API double  aver_pplayer_speed_scale(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_set_default_blend(int32_t player, double seconds);
AVER_PANIM_API double  aver_pplayer_default_blend(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_set_deterministic(int32_t player, int32_t on);
AVER_PANIM_API int32_t aver_pplayer_deterministic(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_set_active(int32_t player, int32_t on);
AVER_PANIM_API int32_t aver_pplayer_active(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_set_discrete_mode(int32_t player, int32_t mode);
AVER_PANIM_API int32_t aver_pplayer_discrete_mode(int32_t player);

AVER_PANIM_API int32_t aver_pplayer_play(int32_t player, const char* name, double customBlend, double customSpeed, int32_t fromEnd);
AVER_PANIM_API int32_t aver_pplayer_stop(int32_t player, int32_t keepState);
AVER_PANIM_API int32_t aver_pplayer_pause(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_queue(int32_t player, const char* name);
AVER_PANIM_API int32_t aver_pplayer_clear_queue(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_queue_count(int32_t player);
AVER_PANIM_API const char* aver_pplayer_queue_at(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_seek(int32_t player, double seconds, int32_t update, int32_t updateOnly);
AVER_PANIM_API int32_t aver_pplayer_advance(int32_t player, double delta);
AVER_PANIM_API int32_t aver_pplayer_set_assigned(int32_t player, const char* name);

AVER_PANIM_API int32_t aver_pplayer_is_playing(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_has_current(int32_t player);
AVER_PANIM_API const char* aver_pplayer_assigned(int32_t player);
AVER_PANIM_API const char* aver_pplayer_current(int32_t player);
AVER_PANIM_API double  aver_pplayer_position(int32_t player);
AVER_PANIM_API double  aver_pplayer_length(int32_t player);
AVER_PANIM_API double  aver_pplayer_playing_speed(int32_t player);
AVER_PANIM_API double  aver_pplayer_custom_speed(int32_t player);

/* Capture: the targets of the named clip in capture mode, then the host's snapshot of their current values. */
AVER_PANIM_API int32_t aver_pplayer_capture_track_count(int32_t player, const char* name);
AVER_PANIM_API int32_t aver_pplayer_capture_track_type(int32_t player, const char* name, int32_t index);
AVER_PANIM_API const char* aver_pplayer_capture_track_path(int32_t player, const char* name, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_capture_begin(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_capture_add(int32_t player, int32_t type, const char* path, const double* v5, const char* text);
AVER_PANIM_API int32_t aver_pplayer_capture_end(int32_t player, double duration);

/* Results, in order, until cleared. */
AVER_PANIM_API int32_t aver_pplayer_event_count(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_event_type(int32_t player, int32_t index);
AVER_PANIM_API const char* aver_pplayer_event_a(int32_t player, int32_t index);
AVER_PANIM_API const char* aver_pplayer_event_b(int32_t player, int32_t index);
AVER_PANIM_API const char* aver_pplayer_event_path(int32_t player, int32_t index);
AVER_PANIM_API double  aver_pplayer_event_t0(int32_t player, int32_t index);
AVER_PANIM_API double  aver_pplayer_event_t1(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_event_arg_count(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_event_arg(int32_t player, int32_t index, int32_t arg, double* v5);
AVER_PANIM_API const char* aver_pplayer_event_arg_text(int32_t player, int32_t index, int32_t arg);
AVER_PANIM_API int32_t aver_pplayer_clear_events(int32_t player);

AVER_PANIM_API int32_t aver_pplayer_output_count(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_output_track(int32_t player, int32_t index);       /* the target id, for track_type / track_path */
AVER_PANIM_API int32_t aver_pplayer_output_discrete(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_output_value(int32_t player, int32_t index, double* v5);
AVER_PANIM_API const char* aver_pplayer_output_text(int32_t player, int32_t index);
AVER_PANIM_API int32_t aver_pplayer_target_type(int32_t player, int32_t target);       /* -1 for a bad id */
AVER_PANIM_API const char* aver_pplayer_target_path(int32_t player, int32_t target);

/* ---- tree mode (AnimationTree, G5 A2) ------------------------------------------------------------------------------
 * The host evaluates the graph and hands over the animations it wants mixed this pass; the player blends them exactly as it does for
 * its own playback. tree_add moves the instance's playhead from `prev` by `delta` (signed, already scaled) under the clip's loop mode,
 * writes the new playhead to *outNext and returns bit 0 = it wrapped or bounced, bit 1 = a ping-pong bounce flipped the direction of
 * play; -1 for an unknown key. A seeked instance sits at `prev` (an absolute time) and moves nothing; external marks a seek made by the
 * user (nearest-key rule for discrete keys); includeStart makes the key sitting exactly at `prev` part of the pass (a restarted playback). trackWeights (one per track of the clip, may be null) scales the instance per track.
 * tree_mix runs the pass over what was added (and empties it); the results are read with the event / output calls above.
 * Root motion: the position / rotation / scale tracks at `path` are kept out of the outputs; root_motion fills out20 with the deltas of the
 * pass {position[3], rotation[4], scale[3]} then the blended absolute values {position[3], rotation[4], scale[3]} (engine space) and
 * returns bit 0 = a position track took part, bit 1 rotation, bit 2 scale (-1 for a stale handle). */
AVER_PANIM_API int32_t aver_pplayer_tree_add(int32_t player, const char* key, double prev, double delta, double weight, const double* trackWeights, int32_t trackWeightCount, int32_t seeked, int32_t external, int32_t includeStart, double* outNext);
AVER_PANIM_API int32_t aver_pplayer_tree_mix(int32_t player, int32_t updateOnly);
AVER_PANIM_API int32_t aver_pplayer_set_root_motion(int32_t player, const char* path);
AVER_PANIM_API const char* aver_pplayer_root_motion_path(int32_t player);
AVER_PANIM_API int32_t aver_pplayer_root_motion(int32_t player, double* out20);

#ifdef __cplusplus
}
#endif
