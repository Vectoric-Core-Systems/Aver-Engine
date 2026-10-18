// AccessKeys.hpp: the key table (ranges, defaults, labels), clamp and snap, the Settings.ini reader, dirty tracking and
// the effective-options reduction. Header-only logic, no I/O. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/AccessKeys.hpp"

#include <cmath>
#include <set>
#include <string>

using namespace aver;
using namespace aver::fw::access;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

static void testTable() {
    AVER_INFO("=== the key table ===");
    std::set<std::string> seen;
    bool defaultsInRange = true, unique = true, labelled = true, stepsOk = true;
    for (usize i = 0; i < accessKeyCount(); ++i) {
        const AccessKey& k = kAccessKeys[i];
        if (k.def < k.lo || k.def > k.hi) { defaultsInRange = false; AVER_ERROR("   default out of range: {}", k.key); }
        if (!seen.insert(k.key).second) { unique = false; AVER_ERROR("   duplicate key: {}", k.key); }
        if (k.tab && !(k.label && k.label[0] == '@')) { labelled = false; AVER_ERROR("   no label ref: {}", k.key); }
        if (k.tab && k.tab[0] != '@') { labelled = false; AVER_ERROR("   no tab ref: {}", k.key); }
        if (k.kind == AccessKind::Choice && !(k.choices && k.choices[0] == '@')) { labelled = false; AVER_ERROR("   no choices: {}", k.key); }
        if (k.kind != AccessKind::Toggle && !(k.step > 0)) stepsOk = false;
        // The default lies on the step grid, so clamping it changes nothing.
        if (!near(accessClamp(k, k.def), k.def)) { stepsOk = false; AVER_ERROR("   default not on its grid: {}", k.key); }
    }
    check(defaultsInRange, "every key's default lies in its range");
    check(unique, "no two keys share a name");
    check(labelled, "every key with a tab has a label ref, a tab ref and (for a choice) choice refs");
    check(stepsOk, "every slider and choice has a step and its default on the grid");
    check(accessFind("graphics.uiScale") != nullptr && accessFind("access.captions.size") != nullptr, "known keys are found");
    check(accessFind("access.nothing") == nullptr && accessFind("") == nullptr, "unknown keys are not");
    check(accessFind("access.colour.simulate")->tab == nullptr, "the simulation toggle is a developer preview, not on the screen");
}

static void testClamp() {
    AVER_INFO("=== clamp and snap ===");
    const AccessKey& scale = *accessFind("access.ui.textScale");
    check(near(accessClamp(scale, 5.0f), 2.0f) && near(accessClamp(scale, 0.0f), 0.75f), "a slider clamps to its range");
    check(near(accessClamp(scale, 1.07f), 1.05f) && near(accessClamp(scale, 1.09f), 1.10f), "and snaps to its step");
    const AccessKey& tog = *accessFind("access.ui.highContrast");
    check(accessClamp(tog, 0.7f) == 1.0f && accessClamp(tog, 0.2f) == 0.0f && accessClamp(tog, -3.0f) == 0.0f, "a toggle is 0 or 1");
    const AccessKey& choice = *accessFind("access.colour.mode");
    check(accessClamp(choice, 2.4f) == 2.0f && accessClamp(choice, 9.0f) == 3.0f && accessClamp(choice, -1.0f) == 0.0f, "a choice is an integer in range");
    check(accessClamp(scale, std::nanf("")) == scale.def, "NaN becomes the default");
    const AccessKey& rate = *accessFind("access.speech.rate");
    check(accessClamp(rate, -20.0f) == -10.0f && accessClamp(rate, 3.4f) == 3.0f, "speech rate snaps to whole SAPI steps");
    check(captionSizeScale(0) == 0.8f && captionSizeScale(1) == 1.0f && captionSizeScale(2) == 1.4f && captionSizeScale(3) == 1.9f && captionSizeScale(9) == 1.9f,
          "caption size presets");
}

static void testIni() {
    AVER_INFO("=== the INI reader ===");
    AccessState st;
    const usize n = accessParseIni(
        "# a comment\n"
        "graphics.uiScale=1.250000\n"
        "access.ui.highContrast=true\n"
        "access.motion.blur = off\r\n"
        "access.captions.size=3\n"
        "unknown.key=7\n"
        "this line is junk\n"
        "access.colour.strength=abc\n"
        "audio.master=0.5\n", st);
    check(n == 4, "four known keys read");
    check(near(st.get("graphics.uiScale"), 1.25f), "'1.250000' (what Settings writes) reads back");
    check(st.get("access.ui.highContrast") == 1.0f, "'true' reads as 1");
    check(st.get("access.motion.blur") == 0.0f, "'off' with spaces and CRLF reads as 0");
    check(st.get("access.captions.size") == 3.0f, "a hand-edited choice is honoured");
    check(st.get("access.colour.strength") == 1.0f, "an unparseable value leaves the default");
    check(st.revision == 0 && st.dirty.empty(), "loading marks nothing dirty and moves no revision");

    AccessState t;
    check(accessParseIni("access.input.toggle.Sprint=1\naccess.input.toggle.=1\n", t) == 1, "a dynamic toggle key is accepted, an empty action is not");
    check(t.toggleOn("Sprint") && !t.toggleOn("Jump"), "and reads back per action");
    check(t.get("access.input.toggle.Sprint") == 1.0f, "through get() too");
}

static void testState() {
    AVER_INFO("=== set, dirty, revision ===");
    AccessState st;
    check(!st.set("nope", 1.0f), "an unknown key is refused");
    check(st.set("access.ui.textScale", 1.5f) && st.revision == 1 && st.dirty.size() == 1, "an accepted change bumps the revision and is dirty");
    check(st.set("access.ui.textScale", 1.5f) && st.revision == 1, "setting the same value changes nothing");
    check(st.set("access.ui.textScale", 1.52f) && st.revision == 1, "a value that snaps to the same step changes nothing");
    check(st.set("access.ui.textScale", 1.8f) && st.revision == 2 && st.dirty.size() == 1 && near(st.dirty[0].second, 1.8f),
          "a second change to the same key updates its dirty entry in place");
    check(st.set("access.input.toggle.Sprint", 1.0f) && st.toggleOn("Sprint") && st.dirty.size() == 2, "a dynamic toggle is dirty too");
    st.resetDefaults();
    check(near(st.get("access.ui.textScale"), 1.0f) && !st.toggleOn("Sprint"), "resetDefaults restores every value and drops the toggles");
}

static void testEffective() {
    AVER_INFO("=== effective options ===");
    AccessState st;
    AccessEffective e = accessEffective(st);
    check(e.colourVisionPacked == 0.0f && e.shake == 1.0f && e.bob == 1.0f && e.motionBlurAllowed == 1.0f && e.lensAllowed == 1.0f, "defaults change nothing");
    st.put("access.motion.shake", 0.4f);
    st.put("access.motion.reduce", 1.0f);
    e = accessEffective(st);
    check(e.shake == 0.0f && e.bob == 0.0f && e.motionBlurAllowed == 0.0f && e.lensAllowed == 0.0f, "reduce motion zeroes shake, bob, blur and lens");
    st.put("access.motion.reduce", 0.0f);
    e = accessEffective(st);
    check(near(e.shake, 0.4f) && e.motionBlurAllowed == 1.0f, "un-ticking restores the sliders: reduce is a gate, not a preset");
    st.put("access.colour.mode", 2.0f);
    st.put("access.colour.strength", 0.5f);
    check(accessEffective(st).colourVisionPacked == 2.0f + 8.0f * 50.0f, "the packed filter is mode + 8 * percent");
    st.put("access.colour.simulate", 1.0f);
    check(accessEffective(st).colourVisionPacked == 2.0f + 4.0f + 8.0f * 50.0f, "and 4 for simulation");
    check(accessPackColourVision(0, true, 1.0f) == 0.0f && accessPackColourVision(4, false, 1.0f) == 0.0f, "mode Off packs to 0");
}

int main() {
    AVER_INFO("AccessKeysTest");
    testTable();
    testClamp();
    testIni();
    testState();
    testEffective();
    AVER_INFO(g_failures == 0 ? "AccessKeysTest: PASS" : "AccessKeysTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
