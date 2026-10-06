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
};

static void fSetReverb(void* u, const audio::ReverbParams& p) { auto* f = static_cast<Fake*>(u); f->reverb = p; ++f->reverbPushes; }
static void fSetOcc(void* u, i32 v, f32 o) { auto* f = static_cast<Fake*>(u); f->occVoice = v; f->occValue = o; ++f->occPushes; }
static void fSetPos(void* u, i32, f32 x, f32 y, f32 z) { auto* f = static_cast<Fake*>(u); f->pos[0] = x; f->pos[1] = y; f->pos[2] = z; }
static bool fPlaying(void* u, i32) { return static_cast<Fake*>(u)->playing; }
static f32  fRay(void* u, const f32*, const f32*) { auto* f = static_cast<Fake*>(u); ++f->rays; return f->blocking; }

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

    if (g_failures == 0) AVER_INFO("=== all audio scene tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
