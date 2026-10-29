// The C seam onto property-track animation (propanim_abi.h).

#include "aver/anim/propanim_abi.h"

#include "aver/anim/PropAnim.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

using namespace aver;
using namespace aver::fmt;
using aver::anim::PropPlayer;

static_assert(int(OcPropKind::Text) == 10 && int(OcPropTrackType::Animation) == 8 && int(OcPropInterp::CubicAngle) == 4 &&
              int(OcPropUpdate::Capture) == 2 && int(OcPropLoop::PingPong) == 2, "propanim_abi.h mirrors these enums");
static_assert(int(anim::PropEvent::MixerApplied) == AVER_PANIM_EVENT_MIXER_APPLIED && int(anim::PropEvent::Started) == AVER_PANIM_EVENT_STARTED &&
              int(anim::PropEvent::AnimStop) == AVER_PANIM_EVENT_ANIM_STOP && int(anim::PropEvent::Applied) == AVER_PANIM_EVENT_APPLIED,
              "propanim_abi.h mirrors the event types");
static_assert(int(anim::PropDiscrete::ForceContinuous) == AVER_PANIM_DISCRETE_FORCE, "discrete modes");

namespace {

using ClipPtr = std::shared_ptr<OcAnimation>;

std::map<i32, ClipPtr> g_clips;
std::map<i32, std::unique_ptr<PropPlayer>> g_players;
i32 g_nextClip = 1, g_nextPlayer = 1;
std::string g_scratch, g_sampleText;

ClipPtr clipOf(i32 h) {
    auto it = g_clips.find(h);
    return it == g_clips.end() ? nullptr : it->second;
}
PropPlayer* playerOf(i32 h) {
    auto it = g_players.find(h);
    return it == g_players.end() ? nullptr : it->second.get();
}
OcPropTrack* trackOf(i32 h, i32 t) {
    ClipPtr c = clipOf(h);
    if (!c || t < 0 || usize(t) >= c->propTracks.size()) return nullptr;
    return &c->propTracks[usize(t)];
}
OcPropKey* keyOf(i32 h, i32 t, i32 k) {
    OcPropTrack* tr = trackOf(h, t);
    if (!tr || k < 0 || usize(k) >= tr->keys.size()) return nullptr;
    return &tr->keys[usize(k)];
}
const char* text(const std::string& s) { g_scratch = s; return g_scratch.c_str(); }
const char* str(const char* s) { return s ? s : ""; }

OcPropValue readValue(const double* v5, const char* s) {
    OcPropValue v;
    if (v5) {
        const int k = int(v5[0]);
        v.kind = (k >= 0 && k <= int(OcPropKind::Text)) ? static_cast<OcPropKind>(k) : OcPropKind::Nil;
        for (int i = 0; i < 4; ++i) v.n[i] = v5[1 + i];
    }
    if (v.kind == OcPropKind::String || v.kind == OcPropKind::Text) v.s = str(s);
    return v;
}
void writeValue(const OcPropValue& v, double* v5) {
    if (!v5) return;
    v5[0] = double(int(v.kind));
    for (int i = 0; i < 4; ++i) v5[1 + i] = v.n[i];
}

const anim::PropEvent* eventOf(i32 p, i32 i) {
    PropPlayer* pl = playerOf(p);
    if (!pl || i < 0 || usize(i) >= pl->events().size()) return nullptr;
    return &pl->events()[usize(i)];
}

} // namespace

extern "C" {

int32_t aver_panim_abi_version(void) { return AVER_PANIM_ABI_VERSION; }

int32_t aver_panim_create(void) {
    const i32 h = g_nextClip++;
    g_clips[h] = std::make_shared<OcAnimation>();
    g_clips[h]->duration = 1.0f;
    g_clips[h]->propClip = true;
    return h;
}
void aver_panim_destroy(int32_t clip) { g_clips.erase(clip); }
int32_t aver_panim_clone(int32_t clip) {
    ClipPtr c = clipOf(clip);
    if (!c) return 0;
    const i32 h = g_nextClip++;
    g_clips[h] = std::make_shared<OcAnimation>(*c);
    return h;
}
int32_t aver_panim_load(const char* path) {
    auto c = std::make_shared<OcAnimation>();
    if (!loadOcAnim(str(path), *c, nullptr)) return 0;
    const i32 h = g_nextClip++;
    g_clips[h] = std::move(c);
    return h;
}
int32_t aver_panim_save(int32_t clip, const char* path) {
    ClipPtr c = clipOf(clip);
    return c && saveOcAnim(str(path), *c, nullptr) ? 1 : 0;
}
int32_t aver_panim_set_length(int32_t clip, double s) {
    ClipPtr c = clipOf(clip);
    if (!c || !(s >= 0.0)) return 0;
    c->duration = float(s);
    return 1;
}
double aver_panim_length(int32_t clip) { ClipPtr c = clipOf(clip); return c ? double(c->duration) : 0.0; }
int32_t aver_panim_set_loop_mode(int32_t clip, int32_t mode) {
    ClipPtr c = clipOf(clip);
    if (!c || mode < 0 || mode > 2) return 0;
    c->propLoop = static_cast<OcPropLoop>(mode);
    return 1;
}
int32_t aver_panim_loop_mode(int32_t clip) { ClipPtr c = clipOf(clip); return c ? int(c->propLoop) : 0; }
int32_t aver_panim_set_step(int32_t clip, double s) {
    ClipPtr c = clipOf(clip);
    if (!c || !(s > 0.0)) return 0;
    c->propStep = float(s);
    return 1;
}
double aver_panim_step(int32_t clip) { ClipPtr c = clipOf(clip); return c ? double(c->propStep) : 0.0; }

int32_t aver_panim_track_count(int32_t clip) { ClipPtr c = clipOf(clip); return c ? int(c->propTracks.size()) : 0; }
int32_t aver_panim_track_add(int32_t clip, int32_t type, int32_t at) {
    ClipPtr c = clipOf(clip);
    if (!c || type < 0 || type > int(OcPropTrackType::Animation)) return -1;
    OcPropTrack t;
    t.type = static_cast<OcPropTrackType>(type);
    // Godot's default interpolation for a transform track is linear; a method track has none.
    const usize pos = (at < 0 || usize(at) >= c->propTracks.size()) ? c->propTracks.size() : usize(at);
    c->propTracks.insert(c->propTracks.begin() + isize(pos), std::move(t));
    return int(pos);
}
int32_t aver_panim_track_remove(int32_t clip, int32_t track) {
    if (!trackOf(clip, track)) return 0;
    ClipPtr c = clipOf(clip);
    c->propTracks.erase(c->propTracks.begin() + track);
    return 1;
}
int32_t aver_panim_track_move(int32_t clip, int32_t track, int32_t to) {
    if (!trackOf(clip, track)) return 0;
    ClipPtr c = clipOf(clip);
    if (to < 0 || usize(to) >= c->propTracks.size()) return 0;
    OcPropTrack t = std::move(c->propTracks[usize(track)]);
    c->propTracks.erase(c->propTracks.begin() + track);
    c->propTracks.insert(c->propTracks.begin() + to, std::move(t));
    return 1;
}
int32_t aver_panim_track_type(int32_t clip, int32_t track) { OcPropTrack* t = trackOf(clip, track); return t ? int(t->type) : -1; }
int32_t aver_panim_track_set_path(int32_t clip, int32_t track, const char* path) {
    OcPropTrack* t = trackOf(clip, track);
    if (!t) return 0;
    t->path = str(path);
    return 1;
}
const char* aver_panim_track_path(int32_t clip, int32_t track) { OcPropTrack* t = trackOf(clip, track); return text(t ? t->path : std::string()); }
int32_t aver_panim_track_set_interp(int32_t clip, int32_t track, int32_t v) {
    OcPropTrack* t = trackOf(clip, track);
    if (!t || v < 0 || v > int(OcPropInterp::CubicAngle)) return 0;
    t->interp = static_cast<OcPropInterp>(v);
    return 1;
}
int32_t aver_panim_track_interp(int32_t clip, int32_t track) { OcPropTrack* t = trackOf(clip, track); return t ? int(t->interp) : 0; }
int32_t aver_panim_track_set_update(int32_t clip, int32_t track, int32_t v) {
    OcPropTrack* t = trackOf(clip, track);
    if (!t || v < 0 || v > int(OcPropUpdate::Capture)) return 0;
    t->update = static_cast<OcPropUpdate>(v);
    return 1;
}
int32_t aver_panim_track_update(int32_t clip, int32_t track) { OcPropTrack* t = trackOf(clip, track); return t ? int(t->update) : 0; }
int32_t aver_panim_track_set_flags(int32_t clip, int32_t track, int32_t wrap, int32_t enabled, int32_t imported) {
    OcPropTrack* t = trackOf(clip, track);
    if (!t) return 0;
    t->loopWrap = wrap != 0;
    t->enabled = enabled != 0;
    t->imported = imported != 0;
    return 1;
}
int32_t aver_panim_track_flags(int32_t clip, int32_t track) {
    OcPropTrack* t = trackOf(clip, track);
    return t ? (t->loopWrap ? 1 : 0) | (t->enabled ? 2 : 0) | (t->imported ? 4 : 0) : -1;
}

int32_t aver_panim_key_count(int32_t clip, int32_t track) { OcPropTrack* t = trackOf(clip, track); return t ? int(t->keys.size()) : 0; }
int32_t aver_panim_key_insert(int32_t clip, int32_t track, double time, const double* v5, const char* s, double transition) {
    OcPropTrack* t = trackOf(clip, track);
    if (!t || !std::isfinite(time)) return -1;
    OcPropKey key;
    key.time = time;
    key.transition = transition;
    key.value = readValue(v5, s);
    // An existing key at the same time is replaced, as Godot's track_insert_key does.
    usize i = 0;
    while (i < t->keys.size() && t->keys[i].time < time - 1e-5) ++i;
    if (i < t->keys.size() && std::fabs(t->keys[i].time - time) <= 1e-5) { t->keys[i] = std::move(key); return int(i); }
    t->keys.insert(t->keys.begin() + isize(i), std::move(key));
    return int(i);
}
int32_t aver_panim_key_remove(int32_t clip, int32_t track, int32_t key) {
    if (!keyOf(clip, track, key)) return 0;
    OcPropTrack* t = trackOf(clip, track);
    t->keys.erase(t->keys.begin() + key);
    return 1;
}
double aver_panim_key_time(int32_t clip, int32_t track, int32_t key) { OcPropKey* k = keyOf(clip, track, key); return k ? k->time : 0.0; }
int32_t aver_panim_key_set_time(int32_t clip, int32_t track, int32_t key, double time) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k || !std::isfinite(time)) return -1;
    OcPropTrack* t = trackOf(clip, track);
    OcPropKey moved = std::move(*k);
    moved.time = time;
    t->keys.erase(t->keys.begin() + key);
    usize i = 0;
    while (i < t->keys.size() && t->keys[i].time <= time) ++i;
    t->keys.insert(t->keys.begin() + isize(i), std::move(moved));
    return int(i);
}
double aver_panim_key_transition(int32_t clip, int32_t track, int32_t key) { OcPropKey* k = keyOf(clip, track, key); return k ? k->transition : 1.0; }
int32_t aver_panim_key_set_transition(int32_t clip, int32_t track, int32_t key, double v) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    k->transition = v;
    return 1;
}
int32_t aver_panim_key_value(int32_t clip, int32_t track, int32_t key, double* v5) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    writeValue(k->value, v5);
    return 1;
}
const char* aver_panim_key_text(int32_t clip, int32_t track, int32_t key) { OcPropKey* k = keyOf(clip, track, key); return text(k ? k->value.s : std::string()); }
int32_t aver_panim_key_set_value(int32_t clip, int32_t track, int32_t key, const double* v5, const char* s) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    k->value = readValue(v5, s);
    return 1;
}
int32_t aver_panim_key_find(int32_t clip, int32_t track, double time, int32_t mode, int32_t backward, int32_t limit) {
    OcPropTrack* t = trackOf(clip, track);
    ClipPtr c = clipOf(clip);
    if (!t || !c) return -1;
    return anim::propFindKey(*t, time, mode == 0 ? anim::PropFind::Nearest : anim::PropFind::Exact, backward != 0, limit ? double(c->duration) : 1e30);
}
int32_t aver_panim_sample(int32_t clip, int32_t track, double time, int32_t backward, double* v5) {
    OcPropTrack* t = trackOf(clip, track);
    ClipPtr c = clipOf(clip);
    if (!t || !c || t->keys.empty()) return 0;
    OcPropValue v;
    if (t->type == OcPropTrackType::Bezier) { v.kind = OcPropKind::Float; v.n[0] = anim::propSampleBezier(*t, time); }
    else {
        anim::PropSample s = anim::propSample(*c, *t, time, backward != 0);
        if (!s.ok) return 0;
        v = s.value;
    }
    g_sampleText = v.s;
    writeValue(v, v5);
    return 1;
}
const char* aver_panim_sample_text(void) { return text(g_sampleText); }
int32_t aver_panim_key_set_handles(int32_t clip, int32_t track, int32_t key, double ix, double iy, double ox, double oy) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    k->handle[0] = ix; k->handle[1] = iy; k->handle[2] = ox; k->handle[3] = oy;
    return 1;
}
int32_t aver_panim_key_handles(int32_t clip, int32_t track, int32_t key, double* out4) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k || !out4) return 0;
    for (int i = 0; i < 4; ++i) out4[i] = k->handle[i];
    return 1;
}
int32_t aver_panim_key_arg_count(int32_t clip, int32_t track, int32_t key) { OcPropKey* k = keyOf(clip, track, key); return k ? int(k->args.size()) : 0; }
int32_t aver_panim_key_arg_set(int32_t clip, int32_t track, int32_t key, int32_t index, const double* v5, const char* s) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k || index < 0 || usize(index) > k->args.size()) return 0;
    if (usize(index) == k->args.size()) k->args.emplace_back();
    k->args[usize(index)] = readValue(v5, s);
    return 1;
}
int32_t aver_panim_key_arg_get(int32_t clip, int32_t track, int32_t key, int32_t index, double* v5) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k || index < 0 || usize(index) >= k->args.size()) return 0;
    writeValue(k->args[usize(index)], v5);
    return 1;
}
const char* aver_panim_key_arg_text(int32_t clip, int32_t track, int32_t key, int32_t index) {
    OcPropKey* k = keyOf(clip, track, key);
    return text(k && index >= 0 && usize(index) < k->args.size() ? k->args[usize(index)].s : std::string());
}
int32_t aver_panim_key_args_clear(int32_t clip, int32_t track, int32_t key) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    k->args.clear();
    return 1;
}
int32_t aver_panim_key_set_offsets(int32_t clip, int32_t track, int32_t key, double start, double end) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k) return 0;
    k->startOffset = start;
    k->endOffset = end;
    return 1;
}
int32_t aver_panim_key_offsets(int32_t clip, int32_t track, int32_t key, double* out2) {
    OcPropKey* k = keyOf(clip, track, key);
    if (!k || !out2) return 0;
    out2[0] = k->startOffset;
    out2[1] = k->endOffset;
    return 1;
}

// ---- player ----

int32_t aver_pplayer_create(void) {
    const i32 h = g_nextPlayer++;
    g_players[h] = std::make_unique<PropPlayer>();
    return h;
}
void aver_pplayer_destroy(int32_t player) { g_players.erase(player); }
int32_t aver_pplayer_set_clip(int32_t player, const char* key, int32_t clip) {
    PropPlayer* p = playerOf(player);
    ClipPtr c = clipOf(clip);
    if (!p || !c) return 0;
    p->setClip(str(key), c);
    return 1;
}
int32_t aver_pplayer_remove_clip(int32_t player, const char* key) { PropPlayer* p = playerOf(player); return p && p->removeClip(str(key)) ? 1 : 0; }
int32_t aver_pplayer_has_clip(int32_t player, const char* key) { PropPlayer* p = playerOf(player); return p && p->clip(str(key)) ? 1 : 0; }
int32_t aver_pplayer_clip_count(int32_t player) { PropPlayer* p = playerOf(player); return p ? int(p->clipKeys().size()) : 0; }
const char* aver_pplayer_clip_key(int32_t player, int32_t index) {
    PropPlayer* p = playerOf(player);
    if (!p) return text({});
    const auto keys = p->clipKeys();
    return text(index >= 0 && usize(index) < keys.size() ? keys[usize(index)] : std::string());
}
int32_t aver_pplayer_set_blend_time(int32_t player, const char* from, const char* to, double seconds) {
    PropPlayer* p = playerOf(player);
    if (!p || !(seconds >= 0.0)) return 0;
    p->setBlendTime(str(from), str(to), seconds);
    return 1;
}
double aver_pplayer_blend_time(int32_t player, const char* from, const char* to) { PropPlayer* p = playerOf(player); return p ? p->blendTime(str(from), str(to)) : 0.0; }
int32_t aver_pplayer_set_next(int32_t player, const char* from, const char* to) {
    PropPlayer* p = playerOf(player);
    if (!p) return 0;
    p->setNext(str(from), str(to));
    return 1;
}
const char* aver_pplayer_next(int32_t player, const char* from) { PropPlayer* p = playerOf(player); return text(p ? p->next(str(from)) : std::string()); }

int32_t aver_pplayer_set_speed_scale(int32_t player, double v) { PropPlayer* p = playerOf(player); if (!p) return 0; p->speedScale = v; return 1; }
double aver_pplayer_speed_scale(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->speedScale : 1.0; }
int32_t aver_pplayer_set_default_blend(int32_t player, double v) { PropPlayer* p = playerOf(player); if (!p) return 0; p->defaultBlendTime = std::max(0.0, v); return 1; }
double aver_pplayer_default_blend(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->defaultBlendTime : 0.0; }
int32_t aver_pplayer_set_deterministic(int32_t player, int32_t on) { PropPlayer* p = playerOf(player); if (!p) return 0; p->deterministic = on != 0; return 1; }
int32_t aver_pplayer_deterministic(int32_t player) { PropPlayer* p = playerOf(player); return p && p->deterministic ? 1 : 0; }
int32_t aver_pplayer_set_active(int32_t player, int32_t on) { PropPlayer* p = playerOf(player); if (!p) return 0; p->active = on != 0; return 1; }
int32_t aver_pplayer_active(int32_t player) { PropPlayer* p = playerOf(player); return p && p->active ? 1 : 0; }
int32_t aver_pplayer_set_discrete_mode(int32_t player, int32_t mode) {
    PropPlayer* p = playerOf(player);
    if (!p || mode < 0 || mode > 2) return 0;
    p->discrete = static_cast<anim::PropDiscrete>(mode);
    return 1;
}
int32_t aver_pplayer_discrete_mode(int32_t player) { PropPlayer* p = playerOf(player); return p ? int(p->discrete) : 1; }

int32_t aver_pplayer_play(int32_t player, const char* name, double blend, double speed, int32_t fromEnd) {
    PropPlayer* p = playerOf(player);
    return p && p->play(str(name), blend, speed, fromEnd != 0) ? 1 : 0;
}
int32_t aver_pplayer_stop(int32_t player, int32_t keepState) { PropPlayer* p = playerOf(player); if (!p) return 0; p->stop(keepState != 0); return 1; }
int32_t aver_pplayer_pause(int32_t player) { PropPlayer* p = playerOf(player); if (!p) return 0; p->pause(); return 1; }
int32_t aver_pplayer_queue(int32_t player, const char* name) {
    PropPlayer* p = playerOf(player);
    if (!p || !p->clip(str(name))) return 0;
    p->queue(str(name));
    return 1;
}
int32_t aver_pplayer_clear_queue(int32_t player) { PropPlayer* p = playerOf(player); if (!p) return 0; p->clearQueue(); return 1; }
int32_t aver_pplayer_queue_count(int32_t player) { PropPlayer* p = playerOf(player); return p ? int(p->queued().size()) : 0; }
const char* aver_pplayer_queue_at(int32_t player, int32_t index) {
    PropPlayer* p = playerOf(player);
    return text(p && index >= 0 && usize(index) < p->queued().size() ? p->queued()[usize(index)] : std::string());
}
int32_t aver_pplayer_seek(int32_t player, double seconds, int32_t update, int32_t updateOnly) {
    PropPlayer* p = playerOf(player);
    if (!p || !std::isfinite(seconds)) return 0;
    p->seek(seconds, update != 0, updateOnly != 0);
    return 1;
}
int32_t aver_pplayer_advance(int32_t player, double delta) {
    PropPlayer* p = playerOf(player);
    if (!p || !std::isfinite(delta)) return 0;
    p->advance(delta);
    return 1;
}
int32_t aver_pplayer_set_assigned(int32_t player, const char* name) {
    PropPlayer* p = playerOf(player);
    if (!p || !p->clip(str(name))) return 0;
    p->setAssigned(str(name));
    return 1;
}

int32_t aver_pplayer_is_playing(int32_t player) { PropPlayer* p = playerOf(player); return p && p->isPlaying() ? 1 : 0; }
int32_t aver_pplayer_has_current(int32_t player) { PropPlayer* p = playerOf(player); return p && p->hasCurrent() ? 1 : 0; }
const char* aver_pplayer_assigned(int32_t player) { PropPlayer* p = playerOf(player); return text(p ? p->assigned() : std::string()); }
const char* aver_pplayer_current(int32_t player) { PropPlayer* p = playerOf(player); return text(p ? p->current() : std::string()); }
double aver_pplayer_position(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->position() : 0.0; }
double aver_pplayer_length(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->length() : 0.0; }
double aver_pplayer_playing_speed(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->playingSpeed() : 0.0; }
double aver_pplayer_custom_speed(int32_t player) { PropPlayer* p = playerOf(player); return p ? p->customSpeed() : 1.0; }

// ---- capture ----
namespace {
std::vector<std::pair<std::pair<OcPropTrackType, std::string>, OcPropValue>> g_capture;
}
int32_t aver_pplayer_capture_track_count(int32_t player, const char* name) {
    PropPlayer* p = playerOf(player);
    return p ? int(p->captureTracks(str(name)).size()) : 0;
}
int32_t aver_pplayer_capture_track_type(int32_t player, const char* name, int32_t index) {
    PropPlayer* p = playerOf(player);
    if (!p) return -1;
    const auto v = p->captureTracks(str(name));
    return index >= 0 && usize(index) < v.size() ? int(v[usize(index)].first) : -1;
}
const char* aver_pplayer_capture_track_path(int32_t player, const char* name, int32_t index) {
    PropPlayer* p = playerOf(player);
    if (!p) return text({});
    const auto v = p->captureTracks(str(name));
    return text(index >= 0 && usize(index) < v.size() ? v[usize(index)].second : std::string());
}
int32_t aver_pplayer_capture_begin(int32_t player) {
    if (!playerOf(player)) return 0;
    g_capture.clear();
    return 1;
}
int32_t aver_pplayer_capture_add(int32_t player, int32_t type, const char* path, const double* v5, const char* s) {
    if (!playerOf(player) || type < 0 || type > int(OcPropTrackType::Animation)) return 0;
    g_capture.push_back({{static_cast<OcPropTrackType>(type), str(path)}, readValue(v5, s)});
    return 1;
}
int32_t aver_pplayer_capture_end(int32_t player, double duration) {
    PropPlayer* p = playerOf(player);
    if (!p) return 0;
    p->capture(g_capture, duration, 0, 0);
    g_capture.clear();
    return 1;
}

// ---- results ----
int32_t aver_pplayer_event_count(int32_t player) { PropPlayer* p = playerOf(player); return p ? int(p->events().size()) : 0; }
int32_t aver_pplayer_event_type(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return e ? int(e->type) : -1; }
const char* aver_pplayer_event_a(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return text(e ? e->a : std::string()); }
const char* aver_pplayer_event_b(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return text(e ? e->b : std::string()); }
const char* aver_pplayer_event_path(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return text(e ? e->path : std::string()); }
double aver_pplayer_event_t0(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return e ? e->t0 : 0.0; }
double aver_pplayer_event_t1(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return e ? e->t1 : 0.0; }
int32_t aver_pplayer_event_arg_count(int32_t player, int32_t i) { const auto* e = eventOf(player, i); return e ? int(e->args.size()) : 0; }
int32_t aver_pplayer_event_arg(int32_t player, int32_t i, int32_t arg, double* v5) {
    const auto* e = eventOf(player, i);
    if (!e || arg < 0 || usize(arg) >= e->args.size()) return 0;
    writeValue(e->args[usize(arg)], v5);
    return 1;
}
const char* aver_pplayer_event_arg_text(int32_t player, int32_t i, int32_t arg) {
    const auto* e = eventOf(player, i);
    return text(e && arg >= 0 && usize(arg) < e->args.size() ? e->args[usize(arg)].s : std::string());
}
int32_t aver_pplayer_clear_events(int32_t player) { PropPlayer* p = playerOf(player); if (!p) return 0; p->clearResults(); return 1; }

int32_t aver_pplayer_output_count(int32_t player) { PropPlayer* p = playerOf(player); return p ? int(p->outputs().size()) : 0; }
static const anim::PropOutput* outputOf(i32 player, i32 i) {
    PropPlayer* p = playerOf(player);
    return p && i >= 0 && usize(i) < p->outputs().size() ? &p->outputs()[usize(i)] : nullptr;
}
int32_t aver_pplayer_output_track(int32_t player, int32_t i) { const auto* o = outputOf(player, i); return o ? int(o->id) : -1; }
int32_t aver_pplayer_output_discrete(int32_t player, int32_t i) { const auto* o = outputOf(player, i); return o && o->discrete ? 1 : 0; }
int32_t aver_pplayer_output_value(int32_t player, int32_t i, double* v5) {
    const auto* o = outputOf(player, i);
    if (!o) return 0;
    writeValue(o->value, v5);
    return 1;
}
const char* aver_pplayer_output_text(int32_t player, int32_t i) { const auto* o = outputOf(player, i); return text(o ? o->value.s : std::string()); }
int32_t aver_pplayer_target_type(int32_t player, int32_t target) {
    PropPlayer* p = playerOf(player);
    return p && target >= 0 && usize(target) < p->trackCount() ? int(p->trackType(u32(target))) : -1;
}
const char* aver_pplayer_target_path(int32_t player, int32_t target) {
    PropPlayer* p = playerOf(player);
    return text(p && target >= 0 && usize(target) < p->trackCount() ? p->trackPath(u32(target)) : std::string());
}

// ---- tree mode ----

int32_t aver_pplayer_tree_add(int32_t player, const char* key, double prev, double delta, double weight, const double* trackWeights,
                              int32_t trackWeightCount, int32_t seeked, int32_t external, int32_t includeStart, double* outNext) {
    PropPlayer* p = playerOf(player);
    if (!p || !std::isfinite(prev) || !std::isfinite(delta) || !std::isfinite(weight)) return -1;
    std::vector<double> tw;
    if (trackWeights && trackWeightCount > 0) tw.assign(trackWeights, trackWeights + trackWeightCount);
    const anim::PropTreeStep s = p->treeAdd(str(key), prev, delta, weight, tw, seeked != 0, external != 0, includeStart != 0);
    if (!s.ok) return -1;
    if (outNext) *outNext = s.next;
    return (s.looped ? 1 : 0) | (s.flipped ? 2 : 0);
}
int32_t aver_pplayer_tree_mix(int32_t player, int32_t updateOnly) {
    PropPlayer* p = playerOf(player);
    if (!p) return 0;
    p->treeMix(updateOnly != 0);
    return 1;
}
int32_t aver_pplayer_set_root_motion(int32_t player, const char* path) {
    PropPlayer* p = playerOf(player);
    if (!p) return 0;
    p->setRootMotionPath(str(path));
    return 1;
}
const char* aver_pplayer_root_motion_path(int32_t player) {
    PropPlayer* p = playerOf(player);
    return text(p ? p->rootMotionPath() : std::string());
}
int32_t aver_pplayer_root_motion(int32_t player, double* out20) {
    PropPlayer* p = playerOf(player);
    if (!p || !out20) return -1;
    const anim::PropRootMotion& r = p->rootMotion();
    int k = 0;
    for (int i = 0; i < 3; ++i) out20[k++] = r.position[i];
    for (int i = 0; i < 4; ++i) out20[k++] = r.rotation[i];
    for (int i = 0; i < 3; ++i) out20[k++] = r.scale[i];
    for (int i = 0; i < 3; ++i) out20[k++] = r.positionAccum[i];
    for (int i = 0; i < 4; ++i) out20[k++] = r.rotationAccum[i];
    for (int i = 0; i < 3; ++i) out20[k++] = r.scaleAccum[i];
    return (r.hasPosition ? 1 : 0) | (r.hasRotation ? 2 : 0) | (r.hasScale ? 4 : 0);
}

} // extern "C"
