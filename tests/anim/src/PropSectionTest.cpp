// Markers and sections of the property-track player, through the plain-C seam (modules/anim.abi) and the .ocanim MRKR chunk:
// named times on a clip that survive a save, a section that confines playback (start at its edge, end at its edge, wrap inside it,
// bounce inside it), sections named by markers, set_section on a running clip, and the refusals. No GPU.
#include "aver/anim/propanim_abi.h"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <filesystem>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static int makeClip(double length, int loop) {
    const int clip = aver_panim_create();
    aver_panim_set_length(clip, length);
    aver_panim_set_loop_mode(clip, loop);
    const int t = aver_panim_track_add(clip, AVER_PANIM_TRACK_VALUE, -1);
    aver_panim_track_set_path(clip, t, "Node:x");
    double v[5] = {AVER_PANIM_KIND_FLOAT, 0, 0, 0, 0};
    aver_panim_key_insert(clip, t, 0.0, v, "", 1.0);
    v[1] = length;
    aver_panim_key_insert(clip, t, length, v, "", 1.0);
    return clip;
}

int main() {
    AVER_INFO("PropSectionTest");
    check(AVER_PANIM_ABI_VERSION >= 2 && aver_panim_abi_version() == AVER_PANIM_ABI_VERSION, "the ABI is version 2 or later");

    AVER_INFO("=== markers on a clip ===");
    const int clip = makeClip(4.0, AVER_PANIM_LOOP_NONE);
    check(aver_panim_marker_add(clip, "windup", 0.5) && aver_panim_marker_add(clip, "strike", 2.0) && aver_panim_marker_add(clip, "recover", 3.0), "three markers");
    check(!aver_panim_marker_add(clip, "strike", 2.5) && !aver_panim_marker_add(clip, "", 1.0), "a taken or empty name is refused");
    double t = -1;
    check(aver_panim_marker_count(clip) == 3 && aver_panim_marker_time(clip, "strike", &t) && near(t, 2.0) && std::string(aver_panim_marker_name(clip, 0)) == "windup", "count, time, name by index");
    check(!aver_panim_marker_time(clip, "nope", &t) && std::string(aver_panim_marker_name(clip, 9)).empty(), "a missing marker and a bad index answer neutrally");
    check(aver_panim_marker_set_time(clip, "recover", 3.5) && aver_panim_marker_time(clip, "recover", &t) && near(t, 3.5), "a marker moves");
    check(aver_panim_marker_set_color(clip, "strike", 1, 0, 0, 1), "a colour is set");
    double rgba[4] = {};
    check(aver_panim_marker_color(clip, "strike", rgba) && near(rgba[0], 1) && near(rgba[1], 0), "and read back");

    AVER_INFO("=== they survive a save ===");
    const std::string file = (std::filesystem::temp_directory_path() / "aver-propsection-test.ocanim").string();
    check(aver_panim_save(clip, file.c_str()), "save");
    const int loaded = aver_panim_load(file.c_str());
    check(loaded != 0 && aver_panim_marker_count(loaded) == 3 && aver_panim_marker_time(loaded, "strike", &t) && near(t, 2.0) &&
              aver_panim_marker_color(loaded, "strike", rgba) && near(rgba[0], 1),
          "load: the markers and the colour are back");
    {
        aver::fmt::OcAnimation plain;
        std::string why;
        check(aver::fmt::loadOcAnim(file, plain, &why) && plain.markers.size() == 3 && plain.marker("windup") != nullptr, "the format-level reader sees them too");
        aver::fmt::OcAnimation none = plain;
        none.markers.clear();
        std::vector<aver::u8> a, b;
        check(aver::fmt::writeOcAnim(none, a, &why), "a clip without markers writes");
        aver::fmt::OcAnimation again;
        check(aver::fmt::parseOcAnim(a.data(), a.size(), again, &why) && again.markers.empty() && aver::fmt::writeOcAnim(again, b, &why) && a == b,
              "and rewrites byte-identically (no MRKR chunk)");
        aver::fmt::OcAnimation dup = plain;
        dup.markers.push_back(dup.markers[0]);
        check(!aver::fmt::writeOcAnim(dup, a, &why), "two markers of one name are refused on write");
    }
    check(aver_panim_marker_remove(clip, "windup") && aver_panim_marker_count(clip) == 2 && !aver_panim_marker_remove(clip, "windup"), "a marker is removed once");
    aver_panim_marker_add(clip, "windup", 0.5);

    AVER_INFO("=== a section on a non-looping clip ===");
    const int p = aver_pplayer_create();
    aver_pplayer_set_clip(p, "swing", clip);
    check(!aver_pplayer_has_section(p), "no section to begin with");
    check(aver_pplayer_play_section(p, "swing", 1.0, 3.0, -1, 1, 0) && aver_pplayer_has_section(p), "play_section starts");
    check(near(aver_pplayer_position(p), 1.0) && near(aver_pplayer_section_start(p), 1.0) && near(aver_pplayer_section_end(p), 3.0), "at the section's start");
    aver_pplayer_advance(p, 0.0);
    aver_pplayer_advance(p, 1.0);
    check(near(aver_pplayer_position(p), 2.0) && aver_pplayer_is_playing(p), "it advances inside the section");
    aver_pplayer_advance(p, 5.0);
    check(near(aver_pplayer_position(p), 3.0) && !aver_pplayer_is_playing(p), "and ends at the section's end, not the clip's");

    AVER_INFO("=== from the end, backwards ===");
    check(aver_pplayer_play_section(p, "swing", 1.0, 3.0, -1, -1, 1) && near(aver_pplayer_position(p), 3.0), "from_end starts at the section's end");
    aver_pplayer_advance(p, 0.0);
    aver_pplayer_advance(p, 5.0);
    check(near(aver_pplayer_position(p), 1.0) && !aver_pplayer_is_playing(p), "backwards it stops at the section's start");

    AVER_INFO("=== a section wraps inside itself when the clip loops ===");
    const int loopClip = makeClip(4.0, AVER_PANIM_LOOP_LINEAR);
    aver_panim_marker_add(loopClip, "a", 1.0);
    aver_panim_marker_add(loopClip, "b", 2.0);
    aver_pplayer_set_clip(p, "loop", loopClip);
    check(aver_pplayer_play_section_markers(p, "loop", "a", "b", -1, 1, 0) && near(aver_pplayer_position(p), 1.0), "markers name the edges");
    aver_pplayer_advance(p, 0.0);
    aver_pplayer_advance(p, 1.25);
    check(near(aver_pplayer_position(p), 1.25) && aver_pplayer_is_playing(p), "the playhead wrapped to 1 + 0.25");
    aver_pplayer_advance(p, 1.0);
    check(near(aver_pplayer_position(p), 1.25), "and keeps wrapping");

    AVER_INFO("=== set_section on the running clip, then reset ===");
    check(aver_pplayer_set_section(p, 0.0, 1.5) && near(aver_pplayer_section_start(p), 0.0) && near(aver_pplayer_section_end(p), 1.5), "the section changes in place");
    check(near(aver_pplayer_position(p), 1.25), "a playhead already inside is kept");
    check(aver_pplayer_set_section_markers(p, "b", "") && near(aver_pplayer_section_start(p), 2.0) && near(aver_pplayer_section_end(p), 4.0) && near(aver_pplayer_position(p), 2.0),
          "an open end by markers reaches the clip's end, and the playhead is brought inside");
    check(aver_pplayer_reset_section(p) && !aver_pplayer_has_section(p) && near(aver_pplayer_section_end(p), 4.0), "reset_section opens it again");

    AVER_INFO("=== refusals ===");
    check(!aver_pplayer_play_section(p, "loop", 3.0, 2.0, -1, 1, 0) && !aver_pplayer_play_section(p, "loop", 2.0, 2.0, -1, 1, 0), "end <= start is refused");
    check(!aver_pplayer_play_section_markers(p, "loop", "missing", "b", -1, 1, 0) && !aver_pplayer_play_section(p, "nope", 0, 1, -1, 1, 0), "a missing marker or animation is refused");
    check(!aver_pplayer_set_section(p, 9.0, 12.0) , "a section past the clip's end is refused");
    aver_pplayer_stop(p, 0);
    check(!aver_pplayer_set_section(p, 0.0, 1.0) && !aver_pplayer_has_section(p), "with nothing playing there is no section to set");
    check(!aver_pplayer_play_section(9999, "x", 0, 1, -1, 1, 0) && aver_pplayer_section_end(9999) == 0.0, "a stale player answers neutrally");

    AVER_INFO("=== playing another animation drops the section ===");
    check(aver_pplayer_play_section(p, "swing", 1.0, 2.0, -1, 1, 0) && aver_pplayer_has_section(p), "a section");
    check(aver_pplayer_play(p, "loop", -1, 1, 0) && !aver_pplayer_has_section(p) && near(aver_pplayer_position(p), 0.0), "play() of another animation plays it whole");

    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (g_failures == 0) AVER_INFO("PropSectionTest: all passed");
    else AVER_ERROR("PropSectionTest: {} FAILED", g_failures);
    return g_failures ? 1 : 0;
}
