// CReverbZone and CAudioOcclusion against a real scene::World, with the mixer and the raycast faked:
// the system's own arithmetic (zone weights from entity transforms, probe-driven smoothing) is what
// is under test.
#include "aver/audio/AudioScene.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1.0e-3f) { return std::fabs(a - b) <= eps; }

static void clearWorld(scene::World& w) {
    std::vector<scene::Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const scene::Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

// A recording stand-in for the mixer.
struct Fake {
    audio::ReverbParams reverb;
    int reverbPushes = 0;
    i32 occVoice = 0;
    f32 occValue = -1.0f;
    int occPushes = 0;
    f32 pos[3] = {0, 0, 0};
    bool playing = true;
    f32 blocking = 1.0f;
    int rays = 0;
    // Zone sends: the settings of each send reverb slot, each voice's amount into each slot, and its listener-side send.
    audio::ZoneReverb zone[audio::kMaxZoneReverbs];
    int zonePushes = 0;
    f32 zsend[16][audio::kMaxZoneReverbs] = {};
    f32 legacy[16] = {};
    int sendPushes = 0, legacyPushes = 0;
};

static void fSetReverb(void* u, const audio::ReverbParams& p) { auto* f = static_cast<Fake*>(u); f->reverb = p; ++f->reverbPushes; }
static void fSetOcc(void* u, i32 v, f32 o) { auto* f = static_cast<Fake*>(u); f->occVoice = v; f->occValue = o; ++f->occPushes; }
static void fSetPos(void* u, i32, f32 x, f32 y, f32 z) { auto* f = static_cast<Fake*>(u); f->pos[0] = x; f->pos[1] = y; f->pos[2] = z; }
static bool fPlaying(void* u, i32) { return static_cast<Fake*>(u)->playing; }
static f32  fRay(void* u, const f32*, const f32*) { auto* f = static_cast<Fake*>(u); ++f->rays; return f->blocking; }
static void fSetZone(void* u, u32 slot, const audio::ZoneReverb& z) { auto* f = static_cast<Fake*>(u); f->zone[slot] = z; ++f->zonePushes; }
static void fSetZoneSend(void* u, i32 v, u32 slot, f32 s) { auto* f = static_cast<Fake*>(u); f->zsend[v][slot] = s; ++f->sendPushes; }
static void fSetLegacy(void* u, i32 v, f32 s) { auto* f = static_cast<Fake*>(u); f->legacy[v] = s; ++f->legacyPushes; }

// A wall in the plane x = 500 covering |y| <= half (and every z): blocks a segment that crosses or touches the plane inside it.
struct Wall { f32 half = 400.0f; };
static f32 wallRay(void* u, const f32* from, const f32* to) {
    const Wall& w = *static_cast<Wall*>(u);
    const f32 a = from[0] - 500.0f, b = to[0] - 500.0f;
    if (a * b > 0.0f || a == b) return 0.0f;
    const f32 y = from[1] + (to[1] - from[1]) * (a / (a - b));
    return std::fabs(y) <= w.half ? 1.0f : 0.0f;
}

int main() {
    AVER_INFO("AudioSceneTest");
    scene::World& w = scene::World::instance();
    clearWorld(w);

    audio::AudioSceneSystem sys;
    check(sys.registerComponents(w) != 0 && sys.reverbZoneType() != 0 && sys.occlusionType() != 0,
          "both components register");
    check(sys.registerComponents(w) != 0, "registering twice is harmless");

    Fake fake;
    audio::AudioControl ctl;
    ctl.setReverb = &fSetReverb;
    ctl.setVoiceOcclusion = &fSetOcc;
    ctl.setVoicePosition = &fSetPos;
    ctl.voicePlaying = &fPlaying;
    ctl.setZoneReverb = &fSetZone;
    ctl.setVoiceZoneSend = &fSetZoneSend;
    ctl.setVoiceReverbSend = &fSetLegacy;
    ctl.user = &fake;
    sys.setControl(ctl);
    sys.setRayFn(&fRay, &fake);

    AVER_INFO("=== a reverb zone follows the listener ===");
    {
        Transform xf;                                   // at the origin
        const scene::Entity z = w.create("Hall", scene::kInvalidEntity, xf);
        auto* c = sys.attachReverbZone(w, z);
        check(c != nullptr, "the zone component attaches");
        if (c) {
            c->halfExtentsCm[0] = 500.0f; c->halfExtentsCm[1] = 500.0f; c->halfExtentsCm[2] = 300.0f;
            c->blendDistanceCm = 200.0f; c->wet = 0.4f; c->decaySec = 2.5f; c->damping = 0.3f;
        }

        sys.setListenerPosition(0, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f) && near(fake.reverb.decaySec, 2.5f), "inside the box the zone's reverb is applied in full");
        const int pushes = fake.reverbPushes;
        sys.tick(w, 0.016f);
        check(fake.reverbPushes == pushes, "and an unchanged reverb is not pushed again");

        sys.setListenerPosition(600, 0, 0);            // 100 cm outside the +X face, halfway through the blend
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f * 0.5f), "100 cm outside a 200 cm blend: half the wet level");
        check(w.component<audio::CReverbZone>(z, sys.reverbZoneType())->weight > 0.49f, "the zone records its weight");

        sys.setListenerPosition(2000, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.0f), "far away there is no reverb");

        // The entity's transform moves and scales the volume.
        w.setLocalPosition(z, Vec3{2000.0f, 0.0f, 0.0f});
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f), "moving the entity moves the zone with it");

        w.setLocalScale(z, Vec3{2.0f, 2.0f, 2.0f});
        sys.setListenerPosition(2900, 0, 0);            // 900 cm out: outside the unscaled 500, inside the doubled 1000
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f), "and its scale stretches it");

        c = w.component<audio::CReverbZone>(z, sys.reverbZoneType());
        c->enabled = 0;
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.0f), "a disabled zone contributes nothing");
        w.destroy(z); w.flush();
    }

    AVER_INFO("=== a sphere zone, and priority between two zones ===");
    {
        clearWorld(w);
        Transform xf;
        const scene::Entity big = w.create("Big", scene::kInvalidEntity, xf);
        const scene::Entity small = w.create("Small", scene::kInvalidEntity, xf);
        sys.attachReverbZone(w, big);
        sys.attachReverbZone(w, small);
        // Fetched after BOTH attach: adding the second grows the pool and can move the first's storage.
        auto* b = w.component<audio::CReverbZone>(big, sys.reverbZoneType());
        auto* s = w.component<audio::CReverbZone>(small, sys.reverbZoneType());
        if (b && s) {
            b->shape = 1; b->radiusCm = 1000.0f; b->blendDistanceCm = 0.0f; b->wet = 0.2f; b->decaySec = 1.0f; b->priority = 0;
            s->shape = 1; s->radiusCm = 200.0f;  s->blendDistanceCm = 0.0f; s->wet = 0.9f; s->decaySec = 4.0f; s->priority = 5;
        }
        sys.setListenerPosition(100, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.9f) && near(fake.reverb.decaySec, 4.0f), "where they overlap the higher priority zone wins");
        sys.setListenerPosition(600, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.2f) && near(fake.reverb.decaySec, 1.0f), "outside the small one the large one applies");
        sys.setListenerPosition(1500, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.0f), "outside both there is nothing");
        clearWorld(w);
    }

    AVER_INFO("=== occlusion follows the probe, smoothly ===");
    {
        clearWorld(w);
        Transform xf; xf.position = Vec3{1000.0f, 0.0f, 0.0f};
        const scene::Entity e = w.create("Emitter", scene::kInvalidEntity, xf);
        auto* c = sys.attachOcclusion(w, e, 7);
        check(c != nullptr && c->voice == 7, "the occlusion component attaches with its voice");
        sys.setListenerPosition(0, 0, 0);
        fake.blocking = 1.0f; fake.playing = true; fake.rays = 0;

        sys.tick(w, 0.1f);
        check(fake.pos[0] == 1000.0f, "the voice is moved to the entity");
        check(fake.rays == 5, "the first think casts five rays");
        check(fake.occVoice == 7 && fake.occValue > 0.0f && fake.occValue < 1.0f, "occlusion starts rising, not jumping");
        f32 prev = fake.occValue;
        bool rising = true;
        for (int i = 0; i < 40; ++i) {
            sys.tick(w, 0.1f);
            if (fake.occValue < prev - 1.0e-6f) rising = false;
            prev = fake.occValue;
        }
        check(rising && prev > 0.99f, "a blocked line of sight climbs to full occlusion");

        fake.blocking = 0.0f;
        sys.tick(w, 0.1f);
        check(fake.occValue < prev && fake.occValue > 0.5f, "clearing it falls more slowly than it rose");
        for (int i = 0; i < 80; ++i) sys.tick(w, 0.1f);
        check(fake.occValue < 0.01f, "and eventually reads clear");

        auto* cc = w.component<audio::CAudioOcclusion>(e, sys.occlusionType());
        cc->thinkIntervalSec = 1.0f; cc->thinkAccumulatorSec = 0.0f;
        fake.rays = 0;
        for (int i = 0; i < 5; ++i) sys.tick(w, 0.1f);
        check(fake.rays == 0, "a long think interval holds the rays back between thinks");
        for (int i = 0; i < 6; ++i) sys.tick(w, 0.1f);
        check(fake.rays == 5, "and casts exactly one set when it elapses");

        fake.playing = false;
        sys.tick(w, 0.1f);
        check(w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->voice == 0, "when the voice ends the emitter lets go of it");
        clearWorld(w);
    }

    check(fake.zonePushes == 0 && fake.sendPushes == 0 && fake.legacyPushes == 0,
          "defaults unchanged: nothing above asked for zone sends, and no zone or send call was made");

    AVER_INFO("=== the listener-side blend can be smoothed ===");
    {
        clearWorld(w);
        Transform xf;
        const scene::Entity z = w.create("Room", scene::kInvalidEntity, xf);
        auto* c = sys.attachReverbZone(w, z);
        c->halfExtentsCm[0] = 500.0f; c->halfExtentsCm[1] = 500.0f; c->halfExtentsCm[2] = 500.0f;
        c->blendDistanceCm = 0.0f; c->wet = 0.4f; c->decaySec = 3.0f;
        sys.setListenerPosition(0, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f), "without smoothing the listener inside the zone gets its reverb at once");
        sys.setReverbSmoothing(0.25f);
        sys.setListenerPosition(5000, 0, 0);
        sys.tick(w, 0.016f);
        check(fake.reverb.wet < 0.4f && fake.reverb.wet > 0.35f, "stepping out of a hard-edged zone eases: one 16 ms tick moves the wet level only a little (" + std::to_string(fake.reverb.wet) + ")");
        bool falling = true;
        f32 prev = fake.reverb.wet;
        for (int i = 0; i < 200; ++i) {
            sys.tick(w, 0.016f);
            if (fake.reverb.wet > prev + 1.0e-6f) falling = false;
            prev = fake.reverb.wet;
        }
        check(falling && near(fake.reverb.wet, 0.0f, 2.0e-3f) && near(fake.reverb.decaySec, 1.5f, 5.0e-3f), "it falls monotonically to the ambient reverb, decay included");
        const int pushes = fake.reverbPushes;
        for (int i = 0; i < 20; ++i) sys.tick(w, 0.016f);
        check(fake.reverbPushes == pushes, "and stops pushing once it has settled");
        sys.setReverbSmoothing(0.0f);
        sys.setListenerPosition(0, 0, 0);
        sys.tick(w, 0.016f);
        check(near(fake.reverb.wet, 0.4f), "smoothing 0 (the default) applies the blend at once again");
        clearWorld(w);
    }

    AVER_INFO("=== zone sends: a voice sends to the zone it is in ===");
    {
        clearWorld(w);
        Transform xa, xb;
        xb.position = Vec3{1200.0f, 0.0f, 0.0f};
        const scene::Entity za = w.create("A", scene::kInvalidEntity, xa);
        const scene::Entity zb = w.create("B", scene::kInvalidEntity, xb);
        sys.attachReverbZone(w, za);
        sys.attachReverbZone(w, zb);
        auto* a = w.component<audio::CReverbZone>(za, sys.reverbZoneType());
        auto* b = w.component<audio::CReverbZone>(zb, sys.reverbZoneType());
        a->halfExtentsCm[0] = a->halfExtentsCm[1] = a->halfExtentsCm[2] = 500.0f; a->blendDistanceCm = 200.0f; a->priority = 1; a->wet = 0.6f; a->decaySec = 1.0f;
        b->halfExtentsCm[0] = b->halfExtentsCm[1] = b->halfExtentsCm[2] = 500.0f; b->blendDistanceCm = 200.0f; b->priority = 3; b->wet = 0.3f; b->decaySec = 3.0f;
        b->impulseSound = 7; b->predelayMs = 20.0f;
        Transform xe;
        const scene::Entity e = w.create("Emitter", scene::kInvalidEntity, xe);
        auto* oc = sys.attachOcclusion(w, e, 3);
        check(oc && oc->zoneSends == 0 && oc->diffraction == 0 && oc->reverbSendBase == 1.0f, "the new emitter settings default to off (zone sends 0, diffraction 0)");
        w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->zoneSends = 1;
        sys.setRayFn(&fRay, &fake);
        fake.blocking = 0.0f; fake.playing = true;
        sys.setListenerPosition(-4000, 0, 0);

        const u64 keyA = static_cast<u64>(za), keyB = static_cast<u64>(zb);
        auto slotOf = [&](u64 key) { for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) if (sys.slotZone(s) == key) return static_cast<int>(s); return -1; };

        w.setLocalPosition(e, Vec3{0.0f, 0.0f, 0.0f});
        sys.tick(w, 0.016f);
        int sa = slotOf(keyA), sb = slotOf(keyB);
        check(sa >= 0 && sb < 0, "a voice inside zone A only: A takes a send reverb slot, B none");
        check(sa >= 0 && near(fake.zsend[3][sa], 1.0f) && near(fake.legacy[3], 0.0f), "it sends everything to A's slot and nothing to the listener-side reverb");
        check(sa >= 0 && near(fake.zone[sa].reverb.wet, 0.6f) && near(fake.zone[sa].reverb.decaySec, 1.0f), "the slot carries A's settings");

        f32 prevA = 1.0f, prevB = 0.0f, prevL = 0.0f, worstStep = 0.0f, worstSum = 0.0f;
        bool monoA = true, monoB = true;
        const int sends0 = fake.sendPushes;
        for (int x = 10; x <= 1200; x += 10) {
            w.setLocalPosition(e, Vec3{static_cast<f32>(x), 0.0f, 0.0f});
            sys.tick(w, 0.016f);
            sa = slotOf(keyA); sb = slotOf(keyB);
            const f32 va = sa >= 0 ? fake.zsend[3][sa] : 0.0f, vb = sb >= 0 ? fake.zsend[3][sb] : 0.0f, vl = fake.legacy[3];
            if (va > prevA + 0.003f) monoA = false;
            if (vb < prevB - 0.003f) monoB = false;
            worstStep = std::max(worstStep, std::max(std::fabs(va - prevA), std::max(std::fabs(vb - prevB), std::fabs(vl - prevL))));
            worstSum = std::max(worstSum, std::fabs(va + vb + vl - 1.0f));
            prevA = va; prevB = vb; prevL = vl;
        }
        check(sa >= 0 && sb >= 0, "walking into B gives it a slot too (two zones, two slots)");
        check(monoA && monoB, "A's send only falls and B's only rises on the way over");
        check(worstStep < 0.12f, "no send moves by more than 0.12 per 10 cm through the overlapping 200 cm blends (" + std::to_string(worstStep) + ")");
        check(worstSum < 0.012f, "the sends and the listener-side remainder always add up to the voice's level (" + std::to_string(worstSum) + ")");
        check(near(prevB, 1.0f) && near(prevA, 0.0f) && near(prevL, 0.0f), "inside B only: everything goes to B");
        check(sb >= 0 && fake.zone[sb].impulse == 7 && near(fake.zone[sb].predelayMs, 20.0f) && near(fake.zone[sb].reverb.wet, 0.3f), "B's slot carries its impulse sound, predelay and wet level");
        const int sends1 = fake.sendPushes;
        sys.tick(w, 0.016f); sys.tick(w, 0.016f);
        check(fake.sendPushes == sends1 && sends1 > sends0, "an unchanged send is not pushed again");

        // Overlap: the higher priority zone takes its share first.
        w.setLocalPosition(e, Vec3{600.0f, 0.0f, 0.0f});
        sys.tick(w, 0.016f);
        sa = slotOf(keyA); sb = slotOf(keyB);
        check(sa >= 0 && sb >= 0 && near(fake.zsend[3][sb], 0.5f, 0.01f) && near(fake.zsend[3][sa], 0.25f, 0.01f) && near(fake.legacy[3], 0.25f, 0.01f),
              "at 600 cm (half way through both blends) the priority-3 zone gets 0.5, the priority-1 zone 0.25 and the listener-side reverb 0.25");

        // The voice stops asking: it gets its own reverb send back and is let go.
        w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->zoneSends = 0;
        sys.tick(w, 0.016f);
        check(near(fake.legacy[3], 1.0f) && near(fake.zsend[3][sa], 0.0f) && near(fake.zsend[3][sb], 0.0f), "turning zoneSends off restores the voice's own reverb send and clears its zone sends");
        clearWorld(w);
    }

    AVER_INFO("=== zone sends: four slots, priority and release ===");
    {
        clearWorld(w);
        scene::Entity zs[6];
        for (int i = 0; i < 6; ++i) {
            Transform xf;
            zs[i] = w.create("Z", scene::kInvalidEntity, xf);
            sys.attachReverbZone(w, zs[i]);
        }
        for (int i = 0; i < 6; ++i) {
            auto* c = w.component<audio::CReverbZone>(zs[i], sys.reverbZoneType());
            c->shape = 1; c->radiusCm = 1000.0f; c->blendDistanceCm = 0.0f; c->priority = i < 5 ? i : 9; c->wet = 0.1f * static_cast<f32>(i + 1); c->decaySec = 1.0f;
            c->enabled = i < 5 ? 1 : 0;                      // the sixth appears later
        }
        Transform xe;
        const scene::Entity e = w.create("Emitter", scene::kInvalidEntity, xe);
        sys.attachOcclusion(w, e, 4);
        w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->zoneSends = 1;
        fake.blocking = 0.0f; fake.playing = true;
        fake.zonePushes = 0;
        sys.setListenerPosition(-9000, 0, 0);
        sys.tick(w, 0.016f);
        int held = 0, lowest = -1;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) {
            for (int i = 0; i < 5; ++i) if (sys.slotZone(s) == static_cast<u64>(zs[i])) { ++held; if (i == 0) lowest = static_cast<int>(s); }
        }
        check(held == 4 && lowest < 0, "five overlapping zones, four slots: the lowest priority zone goes without");
        int top = -1;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) if (sys.slotZone(s) == static_cast<u64>(zs[4])) top = static_cast<int>(s);
        check(top >= 0 && near(fake.zsend[4][top], 1.0f) && near(fake.legacy[4], 0.0f), "the voice sends all of itself to the top priority zone");

        w.component<audio::CReverbZone>(zs[5], sys.reverbZoneType())->enabled = 1;      // priority 9 arrives
        sys.tick(w, 0.016f);
        int has9 = -1, has1 = -1;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) {
            if (sys.slotZone(s) == static_cast<u64>(zs[5])) has9 = static_cast<int>(s);
            if (sys.slotZone(s) == static_cast<u64>(zs[1])) has1 = static_cast<int>(s);
        }
        check(has9 >= 0 && has1 < 0, "a higher priority zone evicts the lowest priority slot holder");

        // Leave: slots are held while their tails ring (the zone's decay), then released with wet 0.
        w.setLocalPosition(e, Vec3{50000.0f, 0.0f, 0.0f});
        for (int i = 0; i < 5; ++i) sys.tick(w, 0.1f);
        int busy = 0;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) busy += sys.slotZone(s) != 0;
        check(busy == 4, "half a second after the voice leaves the slots are still held (their tails are ringing)");
        check(near(fake.legacy[4], 1.0f), "and the voice's listener-side send is back at its level");
        for (int i = 0; i < 20; ++i) sys.tick(w, 0.1f);
        busy = 0;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) busy += sys.slotZone(s) != 0;
        bool allOff = true;
        for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) allOff = allOff && fake.zone[s].reverb.wet == 0.0f;
        check(busy == 0 && allOff, "after the longest decay every slot is released and told wet 0");
        clearWorld(w);
    }

    AVER_INFO("=== diffraction in the scene ===");
    {
        clearWorld(w);
        Transform xf; xf.position = Vec3{1000.0f, 0.0f, 0.0f};
        const scene::Entity e = w.create("Emitter", scene::kInvalidEntity, xf);
        sys.attachOcclusion(w, e, 9);
        Wall wall;
        sys.setRayFn(&wallRay, &wall);
        sys.setListenerPosition(0, 0, 0);
        fake.playing = true;

        auto measured = [&](f32 half, bool diffraction) {
            wall.half = half;
            auto* c = w.component<audio::CAudioOcclusion>(e, sys.occlusionType());
            c->diffraction = diffraction ? 1 : 0;
            c->thinkAccumulatorSec = 0.0f;
            sys.tick(w, 0.1f);
            return std::make_pair(w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->measured,
                                  w.component<audio::CAudioOcclusion>(e, sys.occlusionType())->detourCm);
        };
        const auto off = measured(300.0f, false);
        check(near(off.first, 1.0f) && off.second == 0.0f, "with diffraction off (the default) a wall reads fully occluded, as before");
        const auto on = measured(300.0f, true);
        check(on.first > 0.1f && on.first < 0.7f && on.second > 50.0f, "with it on the same wall reads a bent path: occlusion " + std::to_string(on.first) + ", detour " + std::to_string(on.second) + " cm");
        const auto wide = measured(1200.0f, true), narrow = measured(100.0f, true);
        check(narrow.first < on.first && on.first < wide.first && narrow.second < on.second && on.second < wide.second,
              "a wider wall reads more occluded and a longer detour");
        auto* c = w.component<audio::CAudioOcclusion>(e, sys.occlusionType());
        c->thinkAccumulatorSec = 0.0f;
        for (int i = 0; i < 60; ++i) { c->thinkAccumulatorSec = 0.0f; sys.tick(w, 0.1f); c = w.component<audio::CAudioOcclusion>(e, sys.occlusionType()); }
        check(near(fake.occValue, c->measured, 0.02f) && fake.occValue < 0.7f, "the value sent to the voice settles on the bent path, not on full occlusion");
        sys.setRayFn(&fRay, &fake);
        clearWorld(w);
    }

    if (g_failures == 0) AVER_INFO("=== all audio scene tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
