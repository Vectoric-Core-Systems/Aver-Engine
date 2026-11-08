// The particle editor's track C logic (docs/plans/EDITOR_UE_PARITY.md): stats, playback and seek, curve and gradient key edits,
// module isolate, the moving-emitter path, templates, live parameters and scalability. Headless: the tab is built without ImGui.
#include "ParticleCurveModel.hpp"
#include "ParticleEditor.hpp"
#include "ParticlePreviewModel.hpp"
#include "ParticleTemplates.hpp"
#include "ParticleStackModel.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcParticle.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace aver;
namespace px = aver::editor::pxmodel;
using particles::ParticleEffect;
using particles::ParticleSystemDef;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// A scratch .ocparticle on disk for the tab to load.
static std::string writeFixture(const ParticleSystemDef& d, const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::string why;
    if (!fmt::saveOcparticleSystem(p.string(), d, &why)) AVER_ERROR("fixture write failed: {}", why);
    return p.string();
}

static ParticleSystemDef smallSystem() {
    ParticleSystemDef d;
    ParticleEffect e;
    e.name = "Main";
    e.emissionRate = 60.0f;
    e.maxParticles = 400;
    e.lifetimeMin = e.lifetimeMax = 1.0f;
    e.spreadDeg = 40.0f;
    e.speedMin = 50.0f; e.speedMax = 150.0f;
    d.emitters.push_back(e);
    return d;
}

int main() {
    // ---- 1. stats ----------------------------------------------------------------------------------------
    AVER_INFO("stats");
    {
        ParticleEffect e;
        e.maxParticles = 512;
        check(px::emitterCapacity(e) == 512, "capacity is maxParticles");
        check(!px::emitterOnGpu(e) && px::gpuMemoryEstimate(e) == 0, "Auto below 2048 is the CPU tier with no GPU memory");
        e.maxParticles = 4096;
        check(px::emitterOnGpu(e), "Auto at 2048 or more is the GPU tier");
        e.simTier = particles::SimTier::Cpu;
        check(!px::emitterOnGpu(e), "a CPU request wins over size");
        e.simTier = particles::SimTier::Gpu;
        e.maxParticles = 100;
        check(px::emitterOnGpu(e), "a GPU request wins over size");
        e.simTier = particles::SimTier::Auto;
        e.maxParticles = 8192;
        e.blend = rhi::BlendMode::Additive;
        const u64 additive = px::gpuMemoryEstimate(e);
        e.blend = rhi::BlendMode::PremultipliedAlpha;
        const u64 sorted = px::gpuMemoryEstimate(e);
        check(additive == 8192ull * 80ull, "additive: 80 bytes per particle (64 + dead 4 + alive 12)");
        check(sorted == additive + 8192ull * 8ull, "a sorted emitter adds 8 bytes of sort keys per particle");
        e.render = particles::RenderKind::Ribbon;
        e.linked = true;
        e.linkedCapacity = 4096;
        check(px::emitterCapacity(e) == 4096, "a linked ribbon's capacity is its ring");
        check(px::formatBytes(1536) == "1.5 KB" && px::formatBytes(3u << 20) == "3.0 MB", "byte formatting");
    }

    // ---- 2. playback ---------------------------------------------------------------------------------------
    AVER_INFO("playback");
    {
        const std::string path = writeFixture(smallSystem(), "pxTrackC_playback.ocparticle");
        editor::ParticleEditor a(path), b(path);
        check(a.loaded() && b.loaded(), "the fixture loads");
        a.seekPreview(2.0f);
        b.seekPreview(0.7f);
        b.seekPreview(2.0f);   // from a different time: still resimulates from 0
        check(std::fabs(a.previewTime() - 2.0f) < 1e-3f && std::fabs(b.previewTime() - 2.0f) < 1e-3f, "seek lands on the asked time");
        const auto& pa = a.previewParticles();
        const auto& pb = b.previewParticles();
        bool same = pa.size() == pb.size() && !pa.empty();
        for (usize i = 0; same && i < pa.size(); ++i)
            same = pa[i].position.x == pb[i].position.x && pa[i].position.y == pb[i].position.y && pa[i].position.z == pb[i].position.z &&
                   pa[i].age == pb[i].age && pa[i].id == pb[i].id;
        check(same, "seeking the same time gives bit-identical particles, whatever came before");
        check(a.previewTotal() > 30, "a 60/s emitter has particles at 2 s: " + std::to_string(a.previewTotal()));

        editor::ParticleEditor c(path);
        for (int i = 0; i < 120; ++i) c.stepPreviewFrame();
        const auto& pc = c.previewParticles();
        bool stepSame = pc.size() == pa.size();
        for (usize i = 0; stepSame && i < pc.size(); ++i) stepSame = pc[i].position.x == pa[i].position.x && pc[i].id == pa[i].id;
        check(stepSame, "120 frame steps equal a seek to 2 s");
        check(!c.previewPlaying(), "frame step pauses");
        check(c.previewFrame() == 120, "the frame readout counts steps");

        editor::ParticleEditor d(path);
        d.setPlaybackSpeed(2.0f);
        d.advancePreview(1.0f);
        check(std::fabs(d.previewTime() - 2.0f) < 0.05f, "speed 2 advances two simulated seconds per second: " + std::to_string(d.previewTime()));
        d.setPlaybackSpeed(100.0f);
        check(d.playbackSpeed() == 4.0f, "speed is clamped");
        d.setPlaybackSpeed(1.0f);
        d.restartPreview();
        d.setLoop(true);
        d.setLoopLength(1.0f);
        d.advancePreview(0.9f);
        d.advancePreview(0.5f);
        check(d.previewTime() < 1.0f && d.previewTime() > 0.2f, "a looping preview wraps at its length: " + std::to_string(d.previewTime()));
        d.seekPreview(50.0f);
        check(d.previewTime() <= 1.0f + 1e-3f, "a looping seek stops at the loop length");
        d.setLoop(false);
        d.seekPreview(-5.0f);
        check(d.previewTime() == 0.0f, "a negative seek is time 0");
    }

    // ---- 3. curve graph and gradient ----------------------------------------------------------------------
    AVER_INFO("curves");
    {
        using namespace px;
        CurveView view;
        const ScreenRect rc{100, 50, 500, 250};
        check(std::fabs(xToView(view, rc, viewToX(view, rc, 0.3f)) - 0.3f) < 1e-5f, "graph x round-trips through the screen");
        check(std::fabs(yToView(view, rc, viewToY(view, rc, 0.7f)) - 0.7f) < 1e-5f, "graph y round-trips through the screen");
        check(viewToY(view, rc, view.v1) == rc.y0 && viewToY(view, rc, view.v0) == rc.y1, "value grows upward on screen");

        particles::Curve4 c;
        const f32 a[4] = {1.0f, 0, 0, 0}, b[4] = {0.0f, 0, 0, 0};
        c.n = 2; c.t[0] = 0.0f; c.t[1] = 1.0f;
        for (int k = 0; k < 4; ++k) { c.v[0][k] = a[k]; c.v[1][k] = b[k]; }
        check(insertKey(c, 0, 0.5f, 3.0f, false) && c.n == 3, "double-click adds a key");
        check(std::fabs(c.t[1] - 0.5f) < 1e-6f && c.v[1][0] == 3.0f, "at the clicked time and value, in order");
        check(!insertKey(c, 0, 0.5f, 1.0f, false), "but not on top of an existing key");
        check(insertKey(c, 0, 0.25f, 0.5f, false) && c.n == 4, "a fourth key is allowed");
        check(!insertKey(c, 0, 0.9f, 0.5f, false), "a fifth is refused (the format holds four)");
        const int hit = pickKey(c, 0, view, rc, viewToX(view, rc, 0.5f) + 3.0f, viewToY(view, rc, 3.0f) - 2.0f, 8.0f);
        check(hit >= 0 && std::fabs(c.t[hit] - 0.5f) < 1e-6f, "picking finds the key under the mouse");
        check(pickKey(c, 0, view, rc, rc.x0, rc.y0, 8.0f) == -1, "and nothing in empty space");
        check(moveKey(c, hit, 0, 0.9f, -4.0f, false), "dragging a key moves it");
        check(c.t[hit] < 1.0f && c.t[hit] > c.t[hit - 1] && c.t[hit] < c.t[hit + 1], "its time is held between the neighbours");
        check(c.v[hit][0] == 0.0f, "a size cannot go negative");
        particles::ParticleSystemDef sys = smallSystem();
        sys.emitters[0].sizeCurve = c;
        bool ok = false;
        const std::string text = fmt::writeOcparticleSystem(sys);
        particles::ParticleSystemDef back;
        std::string err;
        ok = fmt::parseOcparticleSystem(text, back, nullptr, &err);
        check(ok && back.emitters[0].sizeCurve.n == 4, "the edited curve survives a write and parse: " + err);

        // colour channels and the gradient bar
        particles::Curve4 g;
        g.n = 2; g.t[0] = 0.0f; g.t[1] = 1.0f;
        const f32 red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 0};
        for (int k = 0; k < 4; ++k) { g.v[0][k] = red[k]; g.v[1][k] = blue[k]; }
        check(gradientInsertStop(g, 0.5f) && g.n == 3, "a click on the bar adds a stop");
        check(std::fabs(g.v[1][0] - 0.5f) < 1e-5f && std::fabs(g.v[1][2] - 0.5f) < 1e-5f && std::fabs(g.v[1][3] - 0.5f) < 1e-5f,
              "carrying the colour the gradient had there");
        check(gradientPick(g, 100.0f, 500.0f, 300.0f + 4.0f, 8.0f) == 1, "the stop under the mouse is found");
        check(gradientPick(g, 100.0f, 500.0f, 250.0f, 8.0f) == -1, "and none in between");
        check(gradientMoveStop(g, 1, gradientTimeAt(100.0f, 500.0f, 380.0f)), "dragging a stop slides it");
        check(std::fabs(g.t[1] - 0.7f) < 1e-5f && std::fabs(g.v[1][0] - 0.5f) < 1e-5f, "keeping its colour");
        check(gradientMoveStop(g, 1, 5.0f) && g.t[1] < 1.0f, "and it stays inside the bar, before the next stop");
        const f32 green[4] = {0, 2.0f, 0, 3.0f};
        check(gradientSetStopColor(g, 1, green) && g.v[1][1] == 2.0f && g.v[1][3] == 1.0f, "recolouring keeps HDR rgb and clamps alpha");
        check(removeCurvePoint(g, 1) && g.n == 2, "removing a stop leaves two");
        check(removeCurvePoint(g, 0) && g.n == 0, "removing below two clears the curve back to the start/end pair");
        CurveView z;
        const f32 span0 = z.t1 - z.t0;
        zoomView(z, 0.5f, 0.5f, 0.5f, 1.0f);
        check(std::fabs((z.t1 - z.t0) - span0 * 0.5f) < 1e-5f && std::fabs((z.v1 - z.v0) - 1.2f) < 1e-5f, "zoom scales only the asked axes");
        const f32 anchorBefore = 0.5f;
        zoomView(z, anchorBefore, 0.5f, 0.5f, 1.0f);
        check(z.t0 < anchorBefore && z.t1 > anchorBefore, "about a point inside the view");
        for (int i = 0; i < 100; ++i) zoomView(z, 0.5f, 0.5f, 0.05f, 0.05f);
        check(z.t1 - z.t0 >= 1e-3f, "zooming in has a floor");
        CurveView pv;
        panView(pv, rc, 40.0f, 0.0f);
        check(pv.t0 < -0.05f, "panning right shows earlier times");
        particles::Curve4 tall;
        tall.n = 2; tall.t[1] = 1.0f; tall.v[0][0] = 0.0f; tall.v[1][0] = 50.0f;
        check(frameCurve(tall, 1u).v1 > 50.0f, "framing fits the tallest key");
    }

    // ---- 4. per-module isolate ------------------------------------------------------------------------------
    AVER_INFO("isolate");
    {
        ParticleSystemDef d = smallSystem();
        ParticleEffect& e = d.emitters[0];
        e.spreadDeg = 0.0f;
        e.speedMin = e.speedMax = 100.0f;
        const int fx = px::addModule(e, particles::ModuleKind::UpdateForce);
        const int fy = px::addModule(e, particles::ModuleKind::UpdateForce);
        px::setModuleParam(e, fx, 0, 1000.0f);   // x
        px::setModuleParam(e, fy, 1, 1000.0f);   // y
        const std::string path = writeFixture(d, "pxTrackC_isolate.ocparticle");
        editor::ParticleEditor ed(path);
        ed.seekPreview(1.0f);
        f32 maxX = 0, maxY = 0;
        for (const particles::Particle& p : ed.previewParticles()) { maxX = std::max(maxX, std::fabs(p.position.x)); maxY = std::max(maxY, std::fabs(p.position.y)); }
        check(maxX > 1.0f && maxY > 1.0f, "with both modules on, the particles drift in x and y");

        ed.isolateModule(0, fx);
        check(ed.isolatedModule() == fx && ed.isolatedEmitter() == 0, "isolate is recorded");
        ed.seekPreview(1.0f);
        maxX = maxY = 0;
        for (const particles::Particle& p : ed.previewParticles()) { maxX = std::max(maxX, std::fabs(p.position.x)); maxY = std::max(maxY, std::fabs(p.position.y)); }
        check(maxX > 1.0f && maxY < 1e-3f, "isolating the x force removes the y drift");
        ed.isolateModule(0, fy);
        ed.seekPreview(1.0f);
        maxX = maxY = 0;
        for (const particles::Particle& p : ed.previewParticles()) { maxX = std::max(maxX, std::fabs(p.position.x)); maxY = std::max(maxY, std::fabs(p.position.y)); }
        check(maxY > 1.0f && maxX < 1e-3f, "and isolating the y force the reverse");
        check(ed.effect().modules[0].enabled && ed.effect().modules[1].enabled, "the saved enable flags are untouched");
        check(!ed.dirty(), "isolate is not an edit");
        const ParticleEffect iso = px::isolatedModule(ed.effect(), 1);
        check(!iso.modules[0].enabled && iso.modules[1].enabled, "isolatedModule keeps only the chosen one");
        check(px::isolatedModule(ed.effect(), 9).modules[0].enabled, "an index out of range changes nothing");
        ed.isolateModule(-1, -1);
        check(ed.isolatedModule() < 0, "clearing works");
        ed.isolateModule(0, fy);
        ed.editEffect([&](ParticleEffect& x) { return px::removeModule(x, fy); });
        ed.seekPreview(0.2f);
        check(ed.isolatedModule() < 0, "removing the isolated module clears the isolate by itself");
    }

    // ---- 5. preview: moving emitter and overlay projection ---------------------------------------------------
    AVER_INFO("preview motion");
    {
        using px::PreviewMotion;
        Vec3 p, v;
        px::emitterMotion(PreviewMotion::None, 100.0f, 50.0f, 3.0f, p, v);
        check(p.size() == 0.0f && v.size() == 0.0f, "Still stays at the origin");
        px::emitterMotion(PreviewMotion::Circle, 100.0f, 200.0f, 0.0f, p, v);
        check(std::fabs(p.x - 100.0f) < 1e-3f && std::fabs(v.y - 200.0f) < 1e-3f, "a circle starts on +X heading +Y at the asked speed");
        px::emitterMotion(PreviewMotion::Circle, 100.0f, 200.0f, 1.2345f, p, v);
        check(std::fabs(std::sqrt(p.x * p.x + p.y * p.y) - 100.0f) < 1e-2f && std::fabs(v.size() - 200.0f) < 1e-2f, "and stays on the circle at constant speed");
        px::emitterMotion(PreviewMotion::Line, 100.0f, 100.0f, 1.0f, p, v);
        check(std::fabs(p.x - 100.0f) < 1e-3f, "a line reaches its end after radius / speed");
        px::emitterMotion(PreviewMotion::Line, 100.0f, 100.0f, 3.0f, p, v);
        check(std::fabs(p.x + 100.0f) < 1e-3f && v.x > 0.0f, "then the far end, turning round");
        px::emitterMotion(PreviewMotion::Line, 100.0f, 100.0f, 4.0f, p, v);
        check(std::fabs(p.x) < 1e-2f, "and is back at the start after one period");
        px::emitterMotion(PreviewMotion::Circle, 0.0f, 100.0f, 1.0f, p, v);
        check(p.size() == 0.0f, "a zero radius does not divide by zero");

        // identity-ish view-projection: x_clip = x, y_clip = y, w = 1 -> the rectangle's centre is the origin
        const f32 vp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        f32 sx = 0, sy = 0;
        check(px::projectToRect(vp, Vec3{0, 0, 0}, 10, 20, 200, 100, sx, sy) && std::fabs(sx - 110) < 1e-3f && std::fabs(sy - 70) < 1e-3f, "the origin lands mid-rectangle");
        check(px::projectToRect(vp, Vec3{1, 1, 0}, 10, 20, 200, 100, sx, sy) && std::fabs(sx - 210) < 1e-3f && std::fabs(sy - 20) < 1e-3f, "+x right and +y UP the screen");
        const f32 behind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1};
        check(!px::projectToRect(behind, Vec3{0, 0, 0}, 0, 0, 10, 10, sx, sy), "a point behind the camera is not drawn");

        // The moving emitter: spawn positions follow it and spawn.inheritvelocity carries its velocity.
        ParticleSystemDef d = smallSystem();
        ParticleEffect& e = d.emitters[0];
        e.spreadDeg = 0.0f;
        e.speedMin = e.speedMax = 10.0f;
        e.lifetimeMin = e.lifetimeMax = 5.0f;
        const int inh = px::addModule(e, particles::ModuleKind::SpawnInheritVelocity);
        px::setModuleParam(e, inh, 0, 1.0f);
        const std::string path = writeFixture(d, "pxTrackC_motion.ocparticle");
        editor::ParticleEditor ed(path);
        ed.setMotion(PreviewMotion::Line, 200.0f, 100.0f);
        ed.seekPreview(1.0f);
        check(std::fabs(ed.emitterPosition().x - 100.0f) < 1.0f, "the editor tracks the emitter position: " + std::to_string(ed.emitterPosition().x));
        f32 newestX = -1e9f, newestVx = 0;
        u32 newestId = 0;
        for (const particles::Particle& q : ed.previewParticles())
            if (q.id >= newestId && particles::particleAlive(q)) { newestId = q.id; newestX = q.position.x; newestVx = q.velocity.x; }
        check(std::fabs(newestX - 100.0f) < 8.0f, "the newest particle was born at the emitter, not the origin: " + std::to_string(newestX));
        check(std::fabs(newestVx - 100.0f) < 5.0f, "carrying its velocity: " + std::to_string(newestVx));
        editor::ParticleEditor still(path);
        still.seekPreview(1.0f);
        bool allOnAxis = true;
        for (const particles::Particle& q : still.previewParticles()) allOnAxis = allOnAxis && std::fabs(q.position.x) < 1e-3f;
        check(allOnAxis, "a still emitter keeps every particle on the axis");
        ed.setMotion(PreviewMotion::Circle, 1.0f, 99999.0f);
        check(ed.motionRadius() == 10.0f && ed.motionSpeed() == 5000.0f, "motion settings are clamped");
    }

    // ---- 6. templates and module presets -------------------------------------------------------------------
    AVER_INFO("templates");
    {
        namespace tp = editor::pxtemplates;
        const auto& all = tp::templates();
        check(all.size() >= 10, "at least ten templates ship");
        for (const char* want : {"Smoke", "Sparks", "Rain", "Snow", "Embers", "Dust", "Mist", "Trail", "Debris", "Flipbook burst"}) {
            bool found = false;
            for (const auto& t : all) found = found || std::string(t.name) == want;
            check(found, std::string("the ") + want + " starter exists");
        }
        for (usize i = 0; i < all.size(); ++i) {
            const std::string name = all[i].name;
            particles::ParticleSystemDef d;
            std::string err;
            const bool parsed = tp::instantiate(i, d, &err);
            check(parsed, name + " parses: " + err);
            if (!parsed) continue;
            check(*all[i].description != '\0', name + " has a description");
            bool builtinOnly = true;
            for (const auto& e : d.emitters) {
                builtinOnly = builtinOnly && e.textureId == 0 && e.customShader.empty() &&
                              (e.meshPath.empty() || e.meshPath == "Meshes/cube.ocmesh" || e.meshPath == "Meshes/sphere.ocmesh");
            }
            check(builtinOnly, name + " uses only built-in assets (the soft dot, the cube and sphere meshes)");
            std::string again = fmt::writeOcparticleSystem(d);
            particles::ParticleSystemDef back;
            const bool rt = fmt::parseOcparticleSystem(again, back, nullptr, &err);
            check(rt && back.emitters.size() == d.emitters.size() && back.emitters[0].modules.size() == d.emitters[0].modules.size(),
                  name + " survives a write and parse: " + err);

            // it runs: write the authored text to disk, load it into a tab and let it play half a second
            const std::filesystem::path file = std::filesystem::temp_directory_path() / ("pxTrackC_tpl_" + std::to_string(i) + ".ocparticle");
            { std::FILE* f = std::fopen(file.string().c_str(), "wb"); if (f) { std::fputs(all[i].text, f); std::fclose(f); } }
            editor::ParticleEditor ed(file.string());
            check(ed.loaded(), name + " loads in the editor: " + ed.loadError());
            ed.seekPreview(0.5f);
            bool finite = true;
            for (usize s = 0; s < ed.previewSims().size(); ++s)
                for (const particles::Particle& q : ed.previewSims()[s].particles())
                    finite = finite && std::isfinite(q.position.x) && std::isfinite(q.position.y) && std::isfinite(q.position.z);
            check(ed.previewTotal() > 0 && finite, name + " has live, finite particles after half a second: " + std::to_string(ed.previewTotal()));
        }
        for (usize i = 0; i < all.size(); ++i)
            for (usize j = i + 1; j < all.size(); ++j) check(std::string(all[i].name) != all[j].name, std::string("unique name ") + all[i].name);

        const auto& presets = tp::presets();
        check(presets.size() >= 8, "module presets ship");
        for (usize i = 0; i < presets.size(); ++i) {
            ParticleEffect e;
            const bool added = tp::applyPreset(e, i);
            check(added && e.modules.size() == presets[i].modules.size(), std::string(presets[i].name) + " adds its modules");
            bool allNamed = true;
            for (const auto& pm : presets[i].modules) {
                std::string_view rest = pm.params;
                const auto& info = particles::moduleInfo(pm.kind);
                while (!rest.empty()) {
                    const usize sp = rest.find(' ');
                    const std::string_view tok = rest.substr(0, sp);
                    rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
                    allNamed = allNamed && tok.find('=') != std::string_view::npos && particles::findModuleParam(info, tok.substr(0, tok.find('='))) >= 0;
                }
            }
            check(allNamed, std::string(presets[i].name) + " names only real parameters");
            ParticleSystemDef d = smallSystem();
            d.emitters[0].modules = e.modules;
            ParticleSystemDef back;
            std::string err;
            check(fmt::parseOcparticleSystem(fmt::writeOcparticleSystem(d), back, nullptr, &err) && back.emitters[0].modules.size() == e.modules.size(),
                  std::string(presets[i].name) + " round-trips: " + err);
        }
        {
            ParticleEffect e;
            tp::applyPreset(e, 1);   // Wind: a force then a drag
            check(e.modules.size() == 2 && e.modules[0].p[0].value == 80.0f && e.modules[1].p[0].value == 0.3f, "preset parameters are applied");
            while (e.modules.size() + 2 <= particles::kMaxModulesPerEmitter) tp::applyPreset(e, 1);
            const usize before = e.modules.size();
            check(!tp::applyPreset(e, 1) || e.modules.size() == before, "a preset that would pass 16 modules is refused whole");
            check(e.modules.size() <= particles::kMaxModulesPerEmitter, "the stack never passes 16");
        }
    }

    // ---- 7. live user parameters -----------------------------------------------------------------------------
    AVER_INFO("live parameters");
    {
        ParticleSystemDef d = smallSystem();
        ParticleEffect& e = d.emitters[0];
        e.spreadDeg = 0.0f;
        e.speedMin = e.speedMax = 10.0f;
        const int wind = px::addUserParam(d, "Wind", 0);
        { const f32 zero[4] = {0, 0, 0, 0}; px::setUserParamValue(d, wind, zero); }
        const int mod = px::addModule(d.emitters[0], particles::ModuleKind::UpdateForce);
        px::bindModuleParam(d, 0, mod, 0, wind, 0);
        const std::string path = writeFixture(d, "pxTrackC_live.ocparticle");
        editor::ParticleEditor ed(path);
        const auto maxX = [&]() {
            f32 m = 0;
            for (const particles::Particle& q : ed.previewParticles()) m = std::max(m, std::fabs(q.position.x));
            return m;
        };
        ed.seekPreview(1.0f);
        check(maxX() < 1e-3f, "at the asset's value of 0 there is no wind");
        check(ed.liveValue(wind) == nullptr && ed.liveCount() == 0, "and no live override");
        const f32 live[4] = {2000.0f, 0, 0, 0};
        ed.setLiveParam(wind, live);
        check(ed.liveValue(wind) != nullptr && ed.liveValue(wind)[0] == 2000.0f && ed.liveCount() == 1, "a live value is held");
        ed.seekPreview(1.0f);
        check(maxX() > 100.0f, "the preview follows the live value: " + std::to_string(maxX()));
        check(!ed.dirty() && !ed.canUndo(), "a live value is neither a dirty mark nor an undo entry");
        check(ed.system().params[0].v[0] == 0.0f, "and the asset still holds 0");
        ed.clearLiveParam(wind);
        ed.seekPreview(1.0f);
        check(maxX() < 1e-3f, "clearing it returns to the asset's value");
        ed.setLiveParam(wind, live);
        check(ed.commitLiveParam(wind), "Keep commits the live value");
        check(ed.system().params[0].v[0] == 2000.0f && ed.dirty() && ed.canUndo(), "into the asset, dirty, with an undo entry");
        check(ed.liveValue(wind) == nullptr, "and the override is gone");
        ed.undo();
        check(ed.system().params[0].v[0] == 0.0f, "undo takes it back");
        ed.setLiveParam(wind, live);
        ed.editSystem([&](ParticleSystemDef& s) { return px::renameUserParam(s, wind, "Gust"); });
        check(ed.liveValue(wind) == nullptr, "renaming the parameter drops the stale override");
        ed.setLiveParam(wind, live);
        ed.resetLiveParams();
        check(ed.liveCount() == 0, "Reset all clears every override");
        ed.setLiveParam(7, live);
        check(ed.liveCount() == 0, "an unknown parameter index is ignored");
    }

    // ---- 8. scalability tiers --------------------------------------------------------------------------------
    AVER_INFO("scalability");
    {
        using particles::ScalabilityTier;
        using particles::TierScale;
        ParticleSystemDef d = smallSystem();
        d.emitters[0].name = "Main";
        d.emitters[0].burstCount = 40;
        ParticleEffect second = d.emitters[0];
        second.name = "Extra";
        d.emitters.push_back(second);
        px::setTierScale(d.scalability, ScalabilityTier::Low, TierScale{true, true, 0.25f, 0.5f});
        px::setTierScale(d.scalability, ScalabilityTier::Medium, TierScale{true, true, 0.75f, 1.0f});
        px::setTierScale(d.emitters[1].scalability, ScalabilityTier::Low, TierScale{true, false, 1.0f, 1.0f});   // Extra is off at Low
        px::setTierScale(d.emitters[0].scalability, ScalabilityTier::Medium, TierScale{true, true, 1.0f, 1.0f});   // Main opts out at Medium

        // -- the file
        const std::string text = fmt::writeOcparticleSystem(d);
        check(text.find("OCPARTICLE 2") != std::string::npos, "scalability is a version 2 feature");
        check(text.find("SCALEALL low rate=0.25 max=0.5") != std::string::npos, "the effect default is one SCALEALL line per set tier");
        check(text.find("SCALE low off") != std::string::npos, "an emitter override is a SCALE line inside its block");
        ParticleSystemDef back;
        std::string err;
        check(fmt::parseOcparticleSystem(text, back, nullptr, &err), "and it parses: " + err);
        bool same = back.emitters.size() == 2 && back.scalability.tier[0].set && back.scalability.tier[0].rateScale == 0.25f &&
                    back.scalability.tier[0].maxScale == 0.5f && !back.scalability.tier[2].set && back.scalability.tier[1].rateScale == 0.75f;
        same = same && back.emitters[1].scalability.tier[0].set && !back.emitters[1].scalability.tier[0].enabled && !back.emitters[0].scalability.tier[0].set &&
               back.emitters[0].scalability.tier[1].set && back.emitters[0].scalability.tier[1].rateScale == 1.0f;
        check(same, "every tier comes back as written, set or not");
        check(fmt::writeOcparticleSystem(back) == text, "write, parse, write is stable");

        // -- older readers: the extension is additive, nothing else moves
        std::string stripped;
        {
            size_t at = 0;
            while (at < text.size()) {
                size_t nl = text.find('\n', at);
                if (nl == std::string::npos) nl = text.size();
                const std::string line = text.substr(at, nl - at);
                if (line.rfind("SCALE", 0) != 0) stripped += line + "\n";
                at = nl + 1;
            }
        }
        ParticleSystemDef plain;
        check(fmt::parseOcparticleSystem(stripped, plain, nullptr, &err) && plain.emitters.size() == 2 && !particles::hasScalability(plain),
              "without its SCALE lines the same file is a plain effect: " + err);
        check(plain.emitters[0].emissionRate == back.emitters[0].emissionRate && plain.emitters[1].name == "Extra", "with every other field untouched");

        // v1 stays v1 until scalability is authored
        ParticleSystemDef one;
        one.emitters.emplace_back();
        check(fmt::writeOcparticleSystem(one).rfind("OCPARTICLE 1", 0) == 0, "an effect with no scalability still writes as version 1");
        px::setTierScale(one.emitters[0].scalability, ScalabilityTier::High, TierScale{true, true, 2.0f, 2.0f});
        check(fmt::writeOcparticleSystem(one).rfind("OCPARTICLE 2", 0) == 0, "and becomes version 2 the moment a tier is authored");
        px::clearTierScale(one.emitters[0].scalability, ScalabilityTier::High);
        check(fmt::writeOcparticleSystem(one).rfind("OCPARTICLE 1", 0) == 0, "clearing it goes back to version 1");

        // merging into an existing file keeps comments and records it does not know
        std::string existing = text;
        existing.insert(existing.find("EMITTER Main"), "# kept comment\nFUTURERECORD 1 2 3\n");
        px::setTierScale(d.scalability, ScalabilityTier::High, TierScale{true, true, 1.5f, 1.5f});
        const std::string merged = fmt::writeOcparticleSystem(d, existing);
        check(merged.find("# kept comment") != std::string::npos && merged.find("FUTURERECORD 1 2 3") != std::string::npos, "a save keeps comments and unknown records");
        check(merged.find("SCALEALL high rate=1.5 max=1.5") != std::string::npos, "and writes the new tier");
        ParticleSystemDef reparsed;
        check(fmt::parseOcparticleSystem(merged, reparsed, nullptr, &err) && reparsed.scalability.tier[2].set, "the merged file parses: " + err);

        // errors name the line
        ParticleSystemDef junk;
        const char* bad[] = {"SCALE nonsense\n", "SCALE low rate=-1\n", "SCALE low speed=2\n", "SCALE low rate=abc\n", "SCALEALL\n"};
        for (const char* b : bad) {
            std::string e2;
            const bool ok = fmt::parseOcparticleSystem(std::string("OCPARTICLE 2\nEMITTER A\n") + b, junk, nullptr, &e2);
            check(!ok && e2.find("line 3") != std::string::npos, std::string("a bad record is refused with its line: ") + std::string(b, strlen(b) - 1));
        }

        // -- selection
        const TierScale lowMain = particles::effectiveTierScale(d, 0, ScalabilityTier::Low);
        check(lowMain.rateScale == 0.25f && lowMain.maxScale == 0.5f, "an emitter with no override takes the effect default");
        check(!particles::effectiveTierScale(d, 1, ScalabilityTier::Low).enabled, "an emitter override wins over the default");
        check(particles::effectiveTierScale(d, 0, ScalabilityTier::Medium).rateScale == 1.0f, "an override back to 1 beats a default of 0.75");
        check(particles::effectiveTierScale(d, 1, ScalabilityTier::Medium).rateScale == 0.75f, "the other emitter still gets 0.75");
        check(particles::tierIsIdentity(particles::effectiveTierScale(d, 0, ScalabilityTier::Epic)), "a tier nobody authored plays as written");

        ParticleSystemDef low = particles::scaledForTier(d, ScalabilityTier::Low);
        check(std::fabs(low.emitters[0].emissionRate - 15.0f) < 1e-4f && low.emitters[0].burstCount == 10 && low.emitters[0].maxParticles == 200,
              "Low: rate 60 -> 15, burst 40 -> 10, max 400 -> 200");
        check(low.emitters[1].emissionRate == 0.0f && low.emitters[1].burstCount == 0 && !low.emitters[1].spawnAtTarget, "an emitter that is off spawns nothing");
        check(d.emitters[0].emissionRate == 60.0f, "the authored effect is not touched");
        const ParticleSystemDef epic = particles::scaledForTier(d, ScalabilityTier::Epic);
        check(epic.emitters[0].emissionRate == 60.0f && epic.emitters[0].maxParticles == 400, "Epic is the effect as written");
        ParticleSystemDef ev = d;
        px::addEvent(ev, 0, 0, 1, 3, 0.0f);
        check(particles::scaledForTier(ev, ScalabilityTier::Low).emitters[0].events.empty(), "an event into an emitter that is off at the tier is dropped");
        check(particles::scaledForTier(ev, ScalabilityTier::Epic).emitters[0].events.size() == 1, "but kept where it plays");
        ParticleSystemDef tiny = d;
        px::setTierScale(tiny.scalability, ScalabilityTier::High, TierScale{true, true, 0.0f, 0.0f});
        check(particles::scaledForTier(tiny, ScalabilityTier::High).emitters[0].maxParticles == 1, "a particle cap never goes below one");
        TierScale clampMe{true, true, 1000.0f, -5.0f};
        particles::Scalability sc;
        px::setTierScale(sc, ScalabilityTier::Low, clampMe);
        check(sc.tier[0].rateScale == 100.0f && sc.tier[0].maxScale == 0.0f, "authored scales are clamped to what the parser accepts");
        check(!px::setTierScale(sc, ScalabilityTier::Low, clampMe), "setting the same value is not an edit");

        // -- the runtime library applies the tier and keeps the authored effect
        particles::ParticleEffectLibrary lib;
        lib.setSystem(7, d);
        check(lib.find(7)->emissionRate == 60.0f, "the library plays at Epic by default");
        lib.setScalabilityTier(ScalabilityTier::Low);
        check(std::fabs(lib.find(7)->emissionRate - 15.0f) < 1e-4f && lib.findSystem(7)->emitters[1].emissionRate == 0.0f, "setting the tier re-scales what is registered");
        check(lib.findAuthored(7)->emitters[0].emissionRate == 60.0f, "the authored effect is kept");
        ParticleSystemDef later = d;
        later.emitters[0].emissionRate = 100.0f;
        lib.setSystem(8, later);
        check(std::fabs(lib.find(8)->emissionRate - 25.0f) < 1e-4f, "an effect registered after the tier change is scaled too");
        lib.setScalabilityTier(ScalabilityTier::Epic);
        check(lib.find(7)->emissionRate == 60.0f && lib.find(8)->emissionRate == 100.0f, "and back to Epic restores them");
        ParticleSystemDef plainDef = smallSystem();
        lib.setSystem(9, plainDef);
        lib.setScalabilityTier(ScalabilityTier::Low);
        check(lib.find(9) == &lib.findAuthored(9)->emitters[0], "an effect with no scalability is not copied");

        // -- the editor previews a tier
        const std::string path = writeFixture(d, "pxTrackC_scale.ocparticle");
        editor::ParticleEditor ed(path);
        check(ed.loaded() && ed.previewTier() == ScalabilityTier::Epic, "the tab opens on Epic");
        ed.seekPreview(1.5f);
        const u32 epicCount = ed.previewTotal();
        ed.setPreviewTier(ScalabilityTier::Low);
        ed.seekPreview(1.5f);
        const u32 lowCount = ed.previewTotal();
        check(lowCount > 0 && lowCount < epicCount, "Low plays fewer particles than Epic: " + std::to_string(lowCount) + " < " + std::to_string(epicCount));
        check(ed.previewSims()[1].aliveCount() == 0, "and the emitter that is off at Low plays none");
        check(!ed.dirty() && ed.system().emitters[0].emissionRate == 60.0f, "previewing a tier is not an edit");
        check(ed.editSystem([&](ParticleSystemDef& s) { return px::setTierScale(s.scalability, ScalabilityTier::Epic, TierScale{true, true, 0.5f, 0.5f}); }) && ed.dirty() && ed.canUndo(),
              "authoring a tier is one undoable edit");
        ed.undo();
        check(!ed.system().scalability.tier[3].set, "undo takes it back");
    }

    {
        using px::PreviewBackdrop;
        f32 c[3] = {};
        check(!px::previewBackdropColor(PreviewBackdrop::Studio, c), "Studio keeps the gradient");
        check(px::previewBackdropColor(PreviewBackdrop::Dark, c) && c[0] < 0.1f && px::previewBackdropColor(PreviewBackdrop::Light, c) && c[1] > 0.5f, "Dark and Light are flat colours");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
