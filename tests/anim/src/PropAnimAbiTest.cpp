// The C seam onto property-track animation (modules/anim.abi): build a clip through the exports, play it on a player, read back the values and the
// events in order, save and load an .ocanim, and the neutral answers for stale handles. The same library Aver.Fabric's AnimationPlayer calls.
#include "aver/anim/propanim_abi.h"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

static void f(double (&v5)[5], double x) { v5[0] = AVER_PANIM_KIND_FLOAT; v5[1] = x; v5[2] = v5[3] = v5[4] = 0; }

int main() {
    AVER_INFO("PropAnimAbiTest");
    check(aver_panim_abi_version() == AVER_PANIM_ABI_VERSION, "the library reports the header's ABI version");

    // ---- a clip ----
    const int clip = aver_panim_create();
    check(clip != 0 && near(aver_panim_length(clip), 1.0) && aver_panim_loop_mode(clip) == AVER_PANIM_LOOP_NONE, "a new clip: length 1, no loop");
    check(aver_panim_set_length(clip, 2.0) && aver_panim_set_loop_mode(clip, AVER_PANIM_LOOP_LINEAR) && !aver_panim_set_loop_mode(clip, 9), "length and loop mode (a bad mode is refused)");
    const int vt = aver_panim_track_add(clip, AVER_PANIM_TRACK_VALUE, -1);
    const int mt = aver_panim_track_add(clip, AVER_PANIM_TRACK_METHOD, -1);
    check(vt == 0 && mt == 1 && aver_panim_track_count(clip) == 2, "two tracks");
    check(aver_panim_track_set_path(clip, vt, "Node:prop") && std::string(aver_panim_track_path(clip, vt)) == "Node:prop", "the path round-trips");
    check(aver_panim_track_type(clip, mt) == AVER_PANIM_TRACK_METHOD && aver_panim_track_type(clip, 7) == -1, "track type, -1 for a bad index");

    double v[5];
    f(v, 10.0);
    check(aver_panim_key_insert(clip, vt, 1.0, v, "", 1.0) == 0, "a key");
    f(v, 0.0);
    check(aver_panim_key_insert(clip, vt, 0.0, v, "", 1.0) == 0, "a key before it goes first (sorted)");
    f(v, 20.0);
    check(aver_panim_key_insert(clip, vt, 1.0, v, "", 1.0) == 1, "a key at the same time replaces it");
    check(aver_panim_key_count(clip, vt) == 2 && near(aver_panim_key_time(clip, vt, 1), 1.0), "two keys");
    double got[5];
    check(aver_panim_key_value(clip, vt, 1, got) && got[0] == AVER_PANIM_KIND_FLOAT && near(got[1], 20.0), "the replaced value");

    double mv[5] = {AVER_PANIM_KIND_STRING, 0, 0, 0, 0};
    const int mk = aver_panim_key_insert(clip, mt, 0.5, mv, "hit", 1.0);
    double arg[5] = {AVER_PANIM_KIND_STRING, 0, 0, 0, 0};
    check(mk == 0 && aver_panim_key_arg_set(clip, mt, mk, 0, arg, "x") && aver_panim_key_arg_count(clip, mt, mk) == 1 && std::string(aver_panim_key_arg_text(clip, mt, mk, 0)) == "x",
          "a method key with an argument");
    check(!aver_panim_key_arg_set(clip, mt, mk, 5, arg, "y"), "an argument past the end is refused");
    check(aver_panim_key_find(clip, vt, 0.7, 0, 0, 0) == 0 && aver_panim_key_find(clip, vt, 0.7, 2, 0, 0) == -1 && aver_panim_key_find(clip, vt, 1.0, 2, 0, 0) == 1, "find: nearest and exact");
    check(aver_panim_sample(clip, vt, 0.5, 0, got) && near(got[1], 10.0), "sample at 0.5 interpolates 0..20");

    // ---- save and load ----
    const std::string file = (std::filesystem::temp_directory_path() / "aver-propanim-abi-test.ocanim").string();
    check(aver_panim_save(clip, file.c_str()), "save to .ocanim");
    const int loaded = aver_panim_load(file.c_str());
    check(loaded != 0 && aver_panim_track_count(loaded) == 2 && std::string(aver_panim_track_path(loaded, 0)) == "Node:prop" && near(aver_panim_length(loaded), 2.0) &&
              aver_panim_loop_mode(loaded) == AVER_PANIM_LOOP_LINEAR,
          "load gives the clip back");
    check(aver_panim_load("no-such-file.ocanim") == 0, "a missing file gives 0");
    const int copy = aver_panim_clone(clip);
    aver_panim_track_remove(copy, 0);
    check(aver_panim_track_count(copy) == 1 && aver_panim_track_count(clip) == 2, "a clone is independent");
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // ---- a player ----
    const int p = aver_pplayer_create();
    check(p != 0 && aver_pplayer_set_clip(p, "walk", clip) && aver_pplayer_has_clip(p, "walk") && aver_pplayer_clip_count(p) == 1 && std::string(aver_pplayer_clip_key(p, 0)) == "walk",
          "a player holds a clip under a key");
    check(!aver_pplayer_play(p, "nope", -1, 1, 0), "playing an unknown animation fails");
    check(aver_pplayer_play(p, "walk", -1.0, 1.0, 0) && aver_pplayer_is_playing(p), "play");
    check(aver_pplayer_event_count(p) == 2 && aver_pplayer_event_type(p, 0) == AVER_PANIM_EVENT_CURRENT_CHANGED && aver_pplayer_event_type(p, 1) == AVER_PANIM_EVENT_STARTED &&
              std::string(aver_pplayer_event_a(p, 1)) == "walk",
          "play reports current_changed then started");
    aver_pplayer_clear_events(p);
    aver_pplayer_advance(p, 0.25);   // the first pass does not move
    aver_pplayer_advance(p, 0.5);
    int calls = 0, applied = 0;
    for (int i = 0; i < aver_pplayer_event_count(p); ++i) {
        if (aver_pplayer_event_type(p, i) == AVER_PANIM_EVENT_METHOD) {
            ++calls;
            check(std::string(aver_pplayer_event_a(p, i)) == "hit" && aver_pplayer_event_arg_count(p, i) == 1 && std::string(aver_pplayer_event_arg_text(p, i, 0)) == "x", "the method call carries its name and argument");
        }
        if (aver_pplayer_event_type(p, i) == AVER_PANIM_EVENT_APPLIED) ++applied;
    }
    check(calls == 1 && applied == 2, "one method call across the two passes, an applied marker per pass");
    check(aver_pplayer_output_count(p) == 1 && std::string(aver_pplayer_target_path(p, aver_pplayer_output_track(p, 0))) == "Node:prop" &&
              aver_pplayer_target_type(p, aver_pplayer_output_track(p, 0)) == AVER_PANIM_TRACK_VALUE && aver_pplayer_output_value(p, 0, got) && near(got[1], 10.0) && !aver_pplayer_output_discrete(p, 0),
          "one blended output: Node:prop = 10 at 0.5 s");
    check(near(aver_pplayer_position(p), 0.5) && near(aver_pplayer_length(p), 2.0) && std::string(aver_pplayer_current(p)) == "walk", "position, length, current");

    aver_pplayer_set_speed_scale(p, 2.0);
    check(near(aver_pplayer_playing_speed(p), 2.0), "playing speed is speed_scale times the custom speed");
    check(aver_pplayer_seek(p, 1.0, 1, 0) && near(aver_pplayer_position(p), 1.0), "seek");
    check(aver_pplayer_stop(p, 0) && !aver_pplayer_is_playing(p) && std::string(aver_pplayer_current(p)).empty() && std::string(aver_pplayer_assigned(p)) == "walk", "stop");

    check(aver_pplayer_set_blend_time(p, "a", "b", 0.5) && near(aver_pplayer_blend_time(p, "a", "b"), 0.5) && near(aver_pplayer_blend_time(p, "b", "a"), 0.0), "blend times");
    check(aver_pplayer_set_next(p, "a", "b") && std::string(aver_pplayer_next(p, "a")) == "b", "next");
    check(aver_pplayer_set_discrete_mode(p, AVER_PANIM_DISCRETE_DOMINANT) && aver_pplayer_discrete_mode(p) == AVER_PANIM_DISCRETE_DOMINANT && !aver_pplayer_set_discrete_mode(p, 5), "discrete mode");

    // ---- tree mode (AnimationTree): the host adds animations, the player mixes them, root motion comes back ----
    {
        const int tc = aver_panim_create();
        aver_panim_set_loop_mode(tc, AVER_PANIM_LOOP_LINEAR);
        const int tv = aver_panim_track_add(tc, AVER_PANIM_TRACK_VALUE, -1);
        const int tr = aver_panim_track_add(tc, AVER_PANIM_TRACK_POSITION3D, -1);
        aver_panim_track_set_path(tc, tv, "N:x");
        aver_panim_track_set_path(tc, tr, "Root");
        double k[5];
        f(k, 0.0);
        aver_panim_key_insert(tc, tv, 0.0, k, "", 1.0);
        f(k, 1.0);
        aver_panim_key_insert(tc, tv, 1.0, k, "", 1.0);
        double p0[5] = {AVER_PANIM_KIND_VEC3, 0, 0, 0, 0}, p1[5] = {AVER_PANIM_KIND_VEC3, 10, 0, 0, 0};
        aver_panim_key_insert(tc, tr, 0.0, p0, "", 1.0);
        aver_panim_key_insert(tc, tr, 1.0, p1, "", 1.0);
        const int tp = aver_pplayer_create();
        check(aver_pplayer_set_clip(tp, "walk", tc) && aver_pplayer_set_root_motion(tp, "Root") && std::string(aver_pplayer_root_motion_path(tp)) == "Root", "a clip and a root-motion path on a player");
        double next[1] = {0};
        int r = aver_pplayer_tree_add(tp, "walk", 0.9, 0.3, 1.0, nullptr, 0, 0, 0, 0, next);
        check(r == 1 && near(next[0], 0.2), "tree_add: the playhead wraps and the answer says so (bit 0)");
        check(aver_pplayer_tree_mix(tp, 0), "tree_mix");
        double out20[20];
        const int flags = aver_pplayer_root_motion(tp, out20);
        check(flags == 1 && near(out20[0], 3.0) && near(out20[10], 2.0), "root motion over a wrapped step: 1 + 2, the accumulator is the value at the new playhead");
        int outputs = aver_pplayer_output_count(tp);
        check(outputs == 1 && std::string(aver_pplayer_target_path(tp, aver_pplayer_output_track(tp, 0))) == "N:x", "the root track is not an output; the value track is");
        double ov[5];
        aver_pplayer_output_value(tp, 0, ov);
        check(near(ov[1], 0.2), "the value track follows the playhead");
        check(aver_pplayer_event_count(tp) == 2, "applied and mixer_applied came back as events");
        aver_pplayer_clear_events(tp);
        const double w[2] = {1.0, 0.0};
        r = aver_pplayer_tree_add(tp, "walk", 0.0, 0.1, 1.0, w, 2, 0, 0, 0, next);
        aver_pplayer_tree_mix(tp, 0);
        check(r == 0 && aver_pplayer_output_count(tp) == 1, "per-track weights are accepted");
        check(aver_pplayer_tree_add(tp, "nope", 0, 0.1, 1.0, nullptr, 0, 0, 0, 0, next) == -1 && aver_pplayer_tree_add(0, "walk", 0, 0.1, 1.0, nullptr, 0, 0, 0, 0, next) == -1,
              "an unknown clip or a stale player is refused");
        aver_pplayer_destroy(tp);
        check(aver_pplayer_root_motion(tp, out20) == -1 && !aver_pplayer_tree_mix(tp, 0), "a destroyed player answers neutrally");
        aver_panim_destroy(tc);
    }

    // ---- stale handles ----
    aver_pplayer_destroy(p);
    aver_panim_destroy(clip);
    check(!aver_pplayer_play(p, "walk", -1, 1, 0) && aver_pplayer_event_count(p) == 0 && !aver_pplayer_is_playing(p) && near(aver_pplayer_position(p), 0.0), "a destroyed player answers neutrally");
    check(aver_panim_track_count(clip) == 0 && near(aver_panim_length(clip), 0.0) && aver_panim_track_type(clip, 0) == -1 && aver_panim_key_insert(clip, 0, 0, nullptr, "", 1) == -1,
          "a destroyed clip answers neutrally");
    aver_panim_destroy(loaded);
    aver_panim_destroy(copy);

    if (g_failures == 0) AVER_INFO("=== the property animation seam passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
