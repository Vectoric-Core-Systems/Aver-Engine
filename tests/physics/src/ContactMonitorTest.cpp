// ContactMonitorTest -- per-body contact monitoring (physics_areas_abi.h): the contact cap and the begin / end events.
//
//   1. a world that never monitors anything is unchanged, and a monitored world simulates bit-identically (the monitor only watches),
//   2. BEGIN and END per body pair, with a point and a normal pointing from the monitored body to its partner,
//   3. a body that FALLS ASLEEP does not end its touch; a wake keeps it; a real departure after sleep still ends it,
//   4. two monitored bodies each get their record; the tracked partners read back,
//   5. max_contacts: an extra partner is held back, then promoted with a BEGIN when a slot frees; removing a body ends its pairs,
//   6. turning the monitor on mid-touch begins it; sensors are not contacts; off stops the records,
//   7. snapshot restore re-seeds the monitor silently,
//   8. the refusals.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_areas_abi.h"
#include "aver/physics/physics_body_ext_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static const float kDt = 1.0f / 60.0f;

struct Ev { int32_t body, other, began; float p[3], n[3]; };

// Everything the contact-monitor stream said over the steps taken through it.
struct Log {
    std::vector<Ev> ev;
    int legacyContacts = 0;
    void step(int n) {
        for (int i = 0; i < n; ++i) {
            aver_phys_step(kDt);
            legacyContacts += aver_phys_contact_count();
            const int32_t c = aver_phys_contact_event_count();
            for (int32_t k = 0; k < c; ++k) {
                Ev e{};
                aver_phys_contact_event_get(k, &e.body, &e.other, &e.began, e.p, e.n);
                ev.push_back(e);
            }
        }
    }
    int count(int32_t body, int32_t other, int began) const {
        int n = 0;
        for (const Ev& e : ev) if (e.body == body && e.other == other && e.began == began) ++n;
        return n;
    }
    const Ev* first(int32_t body, int32_t other, int began) const {
        for (const Ev& e : ev) if (e.body == body && e.other == other && e.began == began) return &e;
        return nullptr;
    }
    void clear() { ev.clear(); legacyContacts = 0; }
};

static int32_t floorBox() { return aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f); }   // top at z = 0

// ---------------------------------------------------------------------------------------------------

static std::vector<float> runStack(bool monitor) {
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    const int32_t b = aver_phys_add_dynamic_box(10.0f, 5.0f, 260.0f, 50, 50, 50, 10.0f);
    const int32_t c = aver_phys_add_dynamic_sphere(-20.0f, 0, 600.0f, 40.0f, 5.0f);
    if (monitor) {
        aver_phys_body_set_contact_monitor(a, 1, 4);
        aver_phys_body_set_contact_monitor(b, 1, 4);
        aver_phys_body_set_contact_monitor(fl, 1, 8);
    }
    for (int i = 0; i < 400; ++i) aver_phys_step(kDt);
    std::vector<float> out;
    for (int32_t h : {a, b, c}) { float p[3]; aver_phys_body_position(h, p); out.insert(out.end(), p, p + 3); }
    aver_phys_shutdown();
    return out;
}

static void testUnchanged() {
    AVER_INFO("=== nothing monitored: nothing reported; a monitored world simulates the same ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 150.0f, 50, 50, 50, 10.0f);
    Log lg;
    lg.step(120);
    check(aver_phys_contact_event_count() == 0 && lg.ev.empty(), "no monitor, no records");
    check(lg.legacyContacts > 0, "the legacy contact queue still fills");
    check(aver_phys_body_contact_monitor(box, nullptr) == 0 && aver_phys_body_contact_count(box) == 0, "the box reads as unmonitored");
    check(aver_phys_areas_abi_version() == AVER_PHYS_AREAS_ABI_VERSION, "the ABI version is the header's");
    (void)fl;
    aver_phys_shutdown();

    const std::vector<float> plain = runStack(false), watched = runStack(true);
    bool same = plain.size() == watched.size();
    for (size_t i = 0; same && i < plain.size(); ++i) same = std::memcmp(&plain[i], &watched[i], sizeof(float)) == 0;
    check(same, "monitoring only watches: every position is bit-identical to the unmonitored run");
}

static void testBeginEnd() {
    AVER_INFO("=== BEGIN and END per pair, with a point and a normal ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 150.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_allow_sleeping(box, 0);
    check(aver_phys_body_set_contact_monitor(box, 1, 4) == 1, "monitor on");
    int32_t mx = -1;
    check(aver_phys_body_contact_monitor(box, &mx) == 1 && mx == 4, "it reads back with its max");
    Log lg;
    lg.step(120);
    check(lg.count(box, fl, 1) == 1, "one BEGIN against the floor");
    check(lg.count(box, fl, 0) == 0, "and no END while it rests");
    if (const Ev* e = lg.first(box, fl, 1)) {
        check(e->n[2] < -0.9f, "the normal points from the box down to the floor, nz = " + f2s(e->n[2]));
        check(std::fabs(e->p[2]) < 10.0f, "the point is at the floor, z = " + f2s(e->p[2]));
    }
    check(aver_phys_body_contact_count(box) == 1, "one tracked partner");
    int32_t other = 0; float pt[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
    check(aver_phys_body_contact_get(box, 0, &other, pt, nrm) == 1 && other == fl && nrm[2] < -0.9f, "read back: the floor, normal down");
    check(aver_phys_body_contact_get(box, 1, &other, pt, nrm) == 0, "index 1 is out of range");

    lg.clear();
    aver_phys_body_set_position(box, 0, 0, 800.0f);
    lg.step(10);
    check(lg.count(box, fl, 0) == 1, "lifted away: one END");
    check(aver_phys_body_contact_count(box) == 0, "no tracked partner left");
    check(lg.count(box, fl, 1) == 0, "no spurious BEGIN");

    lg.clear();
    lg.step(120);
    check(lg.count(box, fl, 1) == 1, "dropped again: BEGIN again");
    aver_phys_shutdown();
}

static void testSleep() {
    AVER_INFO("=== falling asleep is not a separation ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 80.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_contact_monitor(box, 1, 4);
    Log lg;
    lg.step(300);
    check(aver_phys_body_is_active(box) == 0, "the box fell asleep");
    check(lg.count(box, fl, 1) == 1 && lg.count(box, fl, 0) == 0, "one BEGIN, no END across the sleep");
    check(aver_phys_body_contact_count(box) == 1, "still touching the floor");

    lg.clear();
    lg.step(300);
    check(lg.ev.empty(), "300 asleep steps: silence");

    aver_phys_body_activate(box);
    lg.step(60);
    check(lg.ev.empty(), "woken in place: the touch continues, no records");
    check(aver_phys_body_contact_count(box) == 1, "and it is tracked");

    lg.step(300);
    check(aver_phys_body_is_active(box) == 0, "asleep again");
    lg.clear();
    aver_phys_body_set_position(box, 0, 0, 900.0f);   // wakes it and takes it away
    lg.step(12);
    check(lg.count(box, fl, 0) == 1, "moved away after the sleep: END, once the wake grace has run out");
    check(aver_phys_body_contact_count(box) == 0, "no tracked partner");
    aver_phys_shutdown();
}

static void testTwoMonitored() {
    AVER_INFO("=== two monitored bodies: a record for each side ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 50.0f, 50, 50, 50, 10.0f);
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 260.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_contact_monitor(a, 1, 8);
    aver_phys_body_set_contact_monitor(b, 1, 8);
    Log lg;
    lg.step(180);
    check(lg.count(a, fl, 1) == 1 && lg.count(a, b, 1) == 1 && lg.count(b, a, 1) == 1, "A begins with the floor and B, B begins with A");
    check(aver_phys_body_contact_count(a) == 2 && aver_phys_body_contact_count(b) == 1, "A tracks two partners, B one");
    int32_t o0 = 0, o1 = 0;
    aver_phys_body_contact_get(a, 0, &o0, nullptr, nullptr);
    aver_phys_body_contact_get(a, 1, &o1, nullptr, nullptr);
    check(o0 == fl && o1 == b, "partners read back in ascending handle order");
    const Ev* e = lg.first(b, a, 1);
    check(e && e->n[2] < -0.9f, "B sees A below it (normal down)");
    const Ev* f = lg.first(a, b, 1);
    check(f && f->n[2] > 0.9f, "A sees B above it (normal up)");
    aver_phys_shutdown();
}

static void testMaxContacts() {
    AVER_INFO("=== max_contacts, promotion, removal ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 50.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_allow_sleeping(a, 0);
    aver_phys_body_set_contact_monitor(a, 1, 1);
    Log lg;
    lg.step(60);
    const int32_t c = aver_phys_add_dynamic_box(0, 0, 200.0f, 20, 20, 20, 1.0f);
    aver_phys_body_set_allow_sleeping(c, 0);
    lg.step(120);
    check(lg.count(a, fl, 1) == 1, "the floor was tracked first");
    check(lg.count(a, c, 1) == 0 && aver_phys_body_contact_count(a) == 1, "the second partner is held back by max_contacts = 1");

    lg.clear();
    aver_phys_remove_body(fl);
    lg.step(2);
    check(lg.count(a, fl, 0) == 1, "removing the floor ends its pair");
    check(lg.count(a, c, 1) == 1, "and the held-back partner is promoted with a BEGIN");
    int32_t o = 0;
    check(aver_phys_body_contact_count(a) == 1 && aver_phys_body_contact_get(a, 0, &o, nullptr, nullptr) == 1 && o == c, "A now tracks C");

    lg.clear();
    aver_phys_remove_body(c);
    lg.step(2);
    check(lg.count(a, c, 0) == 1 && aver_phys_body_contact_count(a) == 0, "removing C ends that pair too");
    aver_phys_shutdown();
}

static void testSwitching() {
    AVER_INFO("=== switching the monitor on and off ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    Log lg;
    lg.step(300);   // settles and sleeps, unmonitored
    check(aver_phys_body_is_active(box) == 0, "the box sleeps on the floor");
    aver_phys_body_set_contact_monitor(box, 1, 4);
    lg.step(3);
    check(lg.count(box, fl, 1) == 1, "monitor on while touching (and asleep): BEGIN for the existing touch");
    check(aver_phys_body_contact_count(box) == 1, "tracked");
    lg.clear();
    lg.step(120);
    check(lg.ev.empty(), "and it stays quiet");

    check(aver_phys_body_set_contact_monitor(box, 0, 0) == 1, "monitor off");
    check(aver_phys_body_contact_monitor(box, nullptr) == 0 && aver_phys_body_contact_count(box) == 0, "unmonitored again");
    aver_phys_body_set_position(box, 0, 0, 700.0f);
    lg.step(240);
    check(lg.ev.empty(), "no records at all while off, through a fall and a landing");
    aver_phys_shutdown();

    AVER_INFO("--- a sensor is not a contact ---");
    aver_phys_init();
    const int32_t floor2 = floorBox();
    const int32_t sensor = aver_phys_add_sensor_box(0, 0, 200.0f, 200.0f, 200.0f, 60.0f);
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 500.0f, 30.0f, 5.0f);
    aver_phys_body_set_contact_monitor(ball, 1, 8);
    Log l2;
    l2.step(240);
    check(l2.count(ball, floor2, 1) == 1, "the ball reaches the floor");
    check(l2.count(ball, sensor, 1) == 0 && l2.count(ball, sensor, 0) == 0, "and says nothing about the sensor it fell through");
    aver_phys_shutdown();
}

static void testRestore() {
    AVER_INFO("=== snapshot restore re-seeds the monitor silently ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_allow_sleeping(box, 0);
    aver_phys_body_set_contact_monitor(box, 1, 4);
    Log lg;
    lg.step(90);
    check(aver_phys_body_contact_count(box) == 1, "touching before the capture");
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    check(snap > 0, "snapshot taken");
    aver_phys_body_set_position(box, 0, 0, 900.0f);
    lg.step(20);
    check(aver_phys_body_contact_count(box) == 0, "away after the capture");
    lg.clear();
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restored");
    check(aver_phys_body_contact_count(box) == 1, "the restored touch is tracked again");
    lg.step(60);
    check(lg.ev.empty(), "and the restore itself, and the steps after it, were silent");
    aver_phys_body_set_position(box, 0, 0, 900.0f);
    lg.step(10);
    check(lg.count(box, fl, 0) == 1, "a departure after the restore still ends it");
    aver_phys_snapshot_release_all();
    aver_phys_shutdown();
}

static void testRefusals() {
    AVER_INFO("=== refusals ===");
    aver_phys_init();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    check(aver_phys_body_set_contact_monitor(999999, 1, 4) == 0, "a dead handle is refused");
    check(aver_phys_body_set_contact_monitor(box, 1, -1) == 0, "a negative max is refused");
    check(aver_phys_body_set_contact_monitor(box, 1, 100000) == 1, "a huge max clamps");
    int32_t mx = 0;
    aver_phys_body_contact_monitor(box, &mx);
    check(mx == 1024, "to 1024, read back " + std::to_string(mx));
    int32_t o = 0;
    check(aver_phys_body_contact_get(box, 0, &o, nullptr, nullptr) == 0, "no partner at index 0");
    check(aver_phys_contact_event_get(0, &o, &o, &o, nullptr, nullptr) == 0, "no event at index 0");
    check(aver_phys_body_set_contact_monitor(box, 0, 0) == 1 && aver_phys_body_set_contact_monitor(box, 0, 0) == 1, "switching off twice is fine");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Contact monitor (G2)");

    testUnchanged();
    testBeginEnd();
    testSleep();
    testTwoMonitored();
    testMaxContacts();
    testSwitching();
    testRestore();
    testRefusals();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
