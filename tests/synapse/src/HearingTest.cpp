// Hearing arithmetic: range and falloff, sensitivity, tag masks, occlusion, and the last-known-
// position memory with its decay. Pure: the occlusion query is a fake wall.
#include "SynapseAiTestUtil.hpp"

#include "aver/synapse/Hearing.hpp"

#include <cmath>

using namespace aver;
using namespace aver::synapse;
using aitest::check;

namespace {

bool approx(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// A wall across x = 250 (all y, z): occludes any segment that crosses it.
u32 wallAt250(void* user, const Vec3& from, const Vec3& to) {
    ++*static_cast<int*>(user);
    return ((from.x < 250.0f) != (to.x < 250.0f)) ? 1u : 0u;
}

NoiseEvent noise(f32 x, f32 loud, u32 tag = 0, u32 source = 0) {
    NoiseEvent n;
    n.pos = Vec3{x, 0, 0};
    n.loudnessCm = loud;
    n.tag = tag;
    n.source = source;
    return n;
}

} // namespace

int main() {
    AVER_INFO("HearingTest");

    AVER_INFO("range and falloff");
    {
        HearingProfile p;
        const NoiseEvent ev = noise(0, 1000.0f);
        f32 d = 0.0f;
        check(approx(hearingLevel(ev, Vec3{0, 0, 0}, p, nullptr, nullptr, &d), 1.0f) && approx(d, 0.0f),
              "right on top of it: full level");
        check(approx(hearingLevel(ev, Vec3{500, 0, 0}, p, nullptr, nullptr), 0.5f), "half the radius: half level");
        check(hearingLevel(ev, Vec3{1000, 0, 0}, p, nullptr, nullptr) == 0.0f, "at the radius: silent");
        check(hearingLevel(ev, Vec3{1500, 0, 0}, p, nullptr, nullptr) == 0.0f, "beyond the radius: silent");
        check(approx(hearingLevel(ev, Vec3{0, 300, 400}, p, nullptr, nullptr, &d), 0.5f) && approx(d, 500.0f),
              "distance is three-dimensional");
    }

    AVER_INFO("sensitivity, the ear's own range and tag masks");
    {
        HearingProfile keen;
        keen.sensitivity = 2.0f;
        check(approx(hearingLevel(noise(0, 1000), Vec3{1000, 0, 0}, keen, nullptr, nullptr), 0.5f),
              "sensitivity 2 doubles the audible radius");
        HearingProfile deaf;
        deaf.sensitivity = 0.0f;
        check(hearingLevel(noise(0, 1000), Vec3{1, 0, 0}, deaf, nullptr, nullptr) == 0.0f, "sensitivity 0 hears nothing");
        HearingProfile limited;
        limited.maxRangeCm = 300.0f;
        check(hearingLevel(noise(0, 1000), Vec3{400, 0, 0}, limited, nullptr, nullptr) == 0.0f,
              "a loud noise beyond the ear's hard limit is not heard");
        HearingProfile onlyTag3;
        onlyTag3.tagMask = 1u << 3;
        check(hearingLevel(noise(0, 1000, 3), Vec3{100, 0, 0}, onlyTag3, nullptr, nullptr) > 0.0f, "an allowed tag is heard");
        check(hearingLevel(noise(0, 1000, 4), Vec3{100, 0, 0}, onlyTag3, nullptr, nullptr) == 0.0f, "another tag is not");
    }

    AVER_INFO("occlusion shrinks the audible radius, and is only asked when it could matter");
    {
        HearingProfile p;   // factor 0.5 per occluder
        int asked = 0;
        // A clear line: listener and noise on the same side of the wall.
        check(approx(hearingLevel(noise(0, 1000), Vec3{200, 0, 0}, p, wallAt250, &asked), 0.8f),
              "same side of the wall: unaffected");
        // Through the wall: radius 1000 -> 500, listener at 300 -> level 1 - 300/500.
        check(approx(hearingLevel(noise(0, 1000), Vec3{300, 0, 0}, p, wallAt250, &asked), 0.4f),
              "through one wall: the radius halves (level 0.4 at 300 cm)");
        check(hearingLevel(noise(0, 1000), Vec3{600, 0, 0}, p, wallAt250, &asked) == 0.0f,
              "past the shrunken radius: silent");
        asked = 0;
        check(hearingLevel(noise(0, 1000), Vec3{2000, 0, 0}, p, wallAt250, &asked) == 0.0f && asked == 0,
              "out of range: rejected without asking for occlusion");
        HearingProfile mask;
        mask.tagMask = 0;
        asked = 0;
        hearingLevel(noise(0, 1000), Vec3{100, 0, 0}, mask, wallAt250, &asked);
        check(asked == 0, "a masked tag is rejected without asking either");
    }

    AVER_INFO("memory: remember, refresh, merge");
    {
        HearingMemory mem(3);
        check(mem.best() == nullptr, "empty at first");
        mem.remember(Vec3{100, 0, 0}, 0.5f, 1, 7);
        check(mem.entries().size() == 1 && mem.best() != nullptr, "one noise, one entry");
        mem.remember(Vec3{150, 0, 0}, 0.9f, 1, 7);
        check(mem.entries().size() == 1 && approx(mem.entries()[0].pos.x, 150.0f), "same source and tag: refreshed in place");
        mem.remember(Vec3{150, 0, 0}, 0.9f, 2, 7);
        check(mem.entries().size() == 2, "same source, other tag: a separate entry");
        mem.remember(Vec3{900, 0, 0}, 0.2f, 5, 0);
        mem.remember(Vec3{960, 0, 0}, 0.3f, 5, 0);
        check(mem.entries().size() == 3, "two sourceless noises of one tag close together merge");
    }

    AVER_INFO("memory: decay to nothing, reporting what was forgotten");
    {
        HearingMemory mem(4);
        mem.remember(Vec3{100, 0, 0}, 0.8f, 1, 1);
        std::vector<HeardMemory> gone;
        mem.tick(2.0f, 8.0f, &gone);
        check(approx(mem.entries()[0].confidence, 0.75f) && approx(mem.entries()[0].ageSec, 2.0f),
              "2 s of an 8 s memory leaves 75% confidence");
        mem.tick(4.0f, 8.0f, &gone);
        check(approx(mem.entries()[0].confidence, 0.25f) && gone.empty(), "6 s leaves 25%, nothing forgotten yet");
        mem.tick(2.0f, 8.0f, &gone);
        check(mem.entries().empty() && gone.size() == 1, "at 8 s it is forgotten and reported");
        check(gone.size() == 1 && gone[0].source == 1 && approx(gone[0].pos.x, 100.0f), "with what it remembered");

        mem.remember(Vec3{0, 0, 0}, 1.0f, 0, 2);
        mem.tick(100.0f, 8.0f);
        check(mem.entries().empty(), "a very long stall forgets cleanly, without a negative confidence");

        mem.remember(Vec3{0, 0, 0}, 1.0f, 0, 3);
        mem.tick(1.0f, 0.0f);
        check(mem.entries().empty(), "memorySec 0 means no memory at all");
    }

    AVER_INFO("memory: a refresh resets the clock; best() prefers fresh and loud");
    {
        HearingMemory mem(4);
        mem.remember(Vec3{100, 0, 0}, 0.4f, 1, 1);
        mem.tick(4.0f, 8.0f);
        mem.remember(Vec3{200, 0, 0}, 0.4f, 1, 2);
        const HeardMemory* b = mem.best();
        check(b && b->source == 2, "the fresher entry is the best");
        mem.remember(Vec3{100, 0, 0}, 0.4f, 1, 1);
        check(approx(mem.entries()[0].confidence, 1.0f) && approx(mem.entries()[0].ageSec, 0.0f), "a re-hearing resets age");
        mem.remember(Vec3{100, 0, 0}, 0.9f, 1, 1);
        b = mem.best();
        check(b && b->source == 1 && approx(b->level, 0.9f), "equally fresh: the louder wins");
    }

    AVER_INFO("memory: a full list replaces the weakest entry");
    {
        HearingMemory mem(2);
        mem.remember(Vec3{0, 0, 0}, 0.5f, 1, 1);
        mem.tick(5.0f, 8.0f);
        mem.remember(Vec3{0, 0, 0}, 0.5f, 1, 2);
        mem.remember(Vec3{0, 0, 0}, 0.5f, 1, 3);
        bool has1 = false, has2 = false, has3 = false;
        for (const HeardMemory& m : mem.entries()) { has1 |= m.source == 1; has2 |= m.source == 2; has3 |= m.source == 3; }
        check(mem.entries().size() == 2 && !has1 && has2 && has3, "the oldest entry was the one replaced");
    }

    AVER_INFO("search radius grows while the trail goes cold");
    {
        HeardMemory m;
        m.ageSec = 0.0f;
        const f32 young = HearingMemory::searchRadiusCm(m);
        m.ageSec = 4.0f;
        check(HearingMemory::searchRadiusCm(m) > young, "older memories are searched over a wider area");
    }

    return aitest::g_failures == 0 ? 0 : 1;
}
