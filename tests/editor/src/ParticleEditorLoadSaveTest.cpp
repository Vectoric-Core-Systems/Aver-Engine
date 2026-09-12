// The .ocparticle editor tab's headless core: load / save / dirty, every field setter's own range
// clamping, and the preview simulation's tick (spawn, the once-only burst, the maxParticles cap, and
// decay) -- ALL of it reachable with no ImGui and no window, because tickPreview() touches no ImGui
// symbol at all (see ParticleEditor.hpp's own comment on why).
//
// AVER_WITH_IMGUI IS DELIBERATELY UNDEFINED for this target, matching Bt/SoundEditorTest's own
// precedent: ParticleEditor.cpp's `#include "imgui.h"` and its whole drawing half (drawParams(),
// drawPreviewPane()) sit behind that macro. What is left -- load, save, every setter, and the
// preview tick -- is exactly the part worth testing headlessly.
//
// THE BYTE-IDENTICAL ROUND TRIP IS PROVEN HERE, NOT ASSUMED: the task brief for this editor is
// explicit that a save() which rewrites a whole asset from a parsed struct deletes anything the
// parser missed, and that a no-edit load/save round trip must be shown byte-identical BEFORE the
// save path ships. OcParticleTest.cpp already proves this at the FORMAT layer (parseOcparticle /
// writeOcparticle / saveOcparticle); this test proves it again through the ACTUAL call
// ParticleEditor::save() makes, so a bug introduced in the tab's own save() wrapper -- not the
// format underneath it -- would be caught here too.
#include "ParticleEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcParticle.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main() {
    AVER_INFO("ParticleEditorLoadSaveTest");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-particle-editor").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/fixture.ocparticle";

    // ---- the starter effect ------------------------------------------------------------------------
    AVER_INFO("the starter effect is real and saveable");
    {
        fmt::OcParticleExtras ex;
        const particles::ParticleEffect fx = editor::pxStarterEffect(&ex);
        check(!ex.name.empty(), "the starter names itself");
        check(fx.burstCount > 0, "it is a burst, not silence");
        check(fx.maxParticles > 0, "with room to actually spawn into");
        check(fx.lifetimeMin > 0.0f && fx.lifetimeMax >= fx.lifetimeMin, "a sane lifetime range");
        check(fx.damping >= 0.0f && fx.damping < 1.0f, "damping is in the format's own valid range");
        for (int i = 0; i < 4; ++i) {
            check(fx.colorStart[i] >= 0.0f && fx.colorStart[i] <= 1.0f, "colorStart channel in [0,1]");
            check(fx.colorEnd[i]   >= 0.0f && fx.colorEnd[i]   <= 1.0f, "colorEnd channel in [0,1]");
        }
        std::string why;
        check(fmt::saveOcparticle(path, fx, &ex, &why), "the fixture writes to disk: " + why);
    }

    // ---- the tab loads it ---------------------------------------------------------------------------
    AVER_INFO("the tab loads, and a freshly loaded tab is clean");
    {
        editor::ParticleEditor ed(path);
        check(ed.loaded(), "the tab loaded the fixture: " + ed.loadError());
        check(!ed.dirty(), "a freshly loaded tab is clean");
        check(ed.title().find("fixture.ocparticle") != std::string::npos, "the title names the file");
        check(ed.title().find("###particle:") != std::string::npos, "and carries a stable ImGui id");
        check(ed.extras().name == "New Ember Burst", "the authored NAME round-tripped through load");
        check(ed.effect().burstCount == 40, "and so did a numeric field");
    }

    // ---- BYTE-IDENTICAL NO-EDIT ROUND TRIP, through the tab's own save() ---------------------------
    AVER_INFO("a no-edit load -> save round trip is byte-identical (through ParticleEditor::save)");
    {
        const std::string before = readAll(path);
        check(!before.empty(), "the fixture is readable before the round trip");

        editor::ParticleEditor ed(path);
        check(ed.loaded(), "loaded for the round trip");
        std::string why;
        check(ed.save(&why), "an unedited tab still saves cleanly: " + why);

        const std::string after = readAll(path);
        check(before == after, "the file is byte-identical after a no-edit save");

        // AND AGAIN, a second time -- the merge must be idempotent, not merely stable once.
        editor::ParticleEditor ed2(path);
        check(ed2.save(&why), "a second no-edit save also succeeds: " + why);
        const std::string after2 = readAll(path);
        check(after == after2, "and a second no-edit save changes nothing further");
    }

    // ---- field setters clamp to the format's own valid ranges ---------------------------------------
    AVER_INFO("field setters keep every value inside the range save() would otherwise refuse");
    {
        editor::ParticleEditor ed(path);
        check(ed.loaded(), "loaded for the setter checks");

        ed.setDamping(5.0f);
        check(ed.effect().damping < 1.0f, "an absurd damping is clamped below 1, not saved as-is");
        ed.setDamping(-3.0f);
        check(ed.effect().damping >= 0.0f, "a negative damping is clamped to 0");

        f32 badColor[4] = {5.0f, -2.0f, 0.5f, 1.5f};
        ed.setColorStart(badColor);
        for (int i = 0; i < 4; ++i)
            check(ed.effect().colorStart[i] >= 0.0f && ed.effect().colorStart[i] <= 1.0f,
                  "colorStart channel " + std::to_string(i) + " clamped into [0,1]");

        ed.setDirection(Vec3{0, 0, 1}, 500.0f);
        check(ed.effect().spreadDeg <= 180.0f, "an absurd spread is clamped to the format's 180 max");
        ed.setDirection(Vec3{0, 0, 1}, -20.0f);
        check(ed.effect().spreadDeg >= 0.0f, "a negative spread is clamped to 0");

        ed.setLifetime(9.0f, 2.0f);   // deliberately backwards
        check(ed.effect().lifetimeMin <= ed.effect().lifetimeMax,
              "a backwards lifetime range is reordered rather than saved invalid");

        ed.setEmission(-5.0f, 10, 0);
        check(ed.effect().emissionRate >= 0.0f, "a negative rate is clamped to 0");
        check(ed.effect().maxParticles >= 1,
              "maxParticles cannot be clamped to 0 (nothing could ever spawn)");

        check(ed.dirty(), "all of that marked the tab dirty");

        // Every edit above must still leave a file the format's own parser accepts.
        std::string why;
        check(ed.save(&why), "the edited effect still saves: " + why);
        particles::ParticleEffect reread;
        std::string w2;
        check(fmt::loadOcparticle(path, reread, nullptr, &w2), "and reads back cleanly: " + w2);
    }

    // ---- a no-op edit does not mark the tab dirty ----------------------------------------------------
    AVER_INFO("setting a field to its own value does not mark the tab dirty");
    {
        editor::ParticleEditor ed(path);
        check(ed.loaded(), "loaded");
        check(!ed.dirty(), "clean to start");
        ed.setReceivesGI(ed.effect().receivesGI);
        check(!ed.dirty(), "re-setting the same GI value is a no-op");
        ed.setBlend(ed.effect().blend);
        check(!ed.dirty(), "re-setting the same blend is a no-op");
    }

    // ---- the preview simulation, entirely headless ---------------------------------------------------
    AVER_INFO("the preview burst fires exactly once, regardless of dt");
    {
        editor::ParticleEditor ed(path);
        check(ed.loaded(), "loaded for the burst check");
        ed.setEmission(0.0f, 25, 1000);   // burst only, generous cap
        ed.setLifetime(10.0f, 10.0f);     // long enough that nothing decays mid-test
        check(ed.previewParticles().empty(), "nothing has spawned before the first tick");

        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 25, "the burst spawned exactly burstCount particles");
        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 25, "and never fires a second time while still playing");
    }

    AVER_INFO("emissionRate accumulates fractional particles across ticks");
    {
        // Numbers chosen to sit well clear of any floating-point rounding boundary: after tick 1 the
        // accumulator sits near 50.3 (0.3 away from either integer neighbour), and after tick 2 it
        // crosses 1.1 past the next whole particle (0.1 clear on both sides) -- comfortably wider
        // than float32's relative error at these magnitudes (~1e-6).
        editor::ParticleEditor ed(path);
        ed.setEmission(100.0f, 0, 10000);   // 100/sec, no burst
        ed.setLifetime(10.0f, 10.0f);

        ed.tickPreview(0.503f);   // accum ~50.3 -> spawns 50, remainder ~0.3
        check(ed.previewParticles().size() == 50, "~0.503s at 100/s spawns 50, with a fractional carry");

        ed.tickPreview(0.008f);   // accum ~0.3 + 0.8 = 1.1 -> spawns 1 more, remainder ~0.1
        check(ed.previewParticles().size() == 51,
              "the carried fractional remainder plus the next tick crosses into one more particle");
    }

    AVER_INFO("the preview never exceeds maxParticles");
    {
        editor::ParticleEditor ed(path);
        ed.setEmission(0.0f, 500, 20);   // ask for far more than the cap
        ed.setLifetime(10.0f, 10.0f);
        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 20,
              "the burst is capped at maxParticles, not dropped entirely nor left over budget");
    }

    AVER_INFO("particles decay and are compacted out once their lifetime elapses");
    {
        editor::ParticleEditor ed(path);
        ed.setEmission(0.0f, 10, 1000);
        ed.setLifetime(1.0f, 1.0f);      // fixed, deterministic lifetime
        ed.setGravity(Vec3{0, 0, 0});
        ed.setDamping(0.0f);
        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 10, "the burst spawned");
        ed.tickPreview(0.5f);
        check(ed.previewParticles().size() == 10, "still alive at ~0.52s (lifetime is 1.0s)");
        ed.tickPreview(0.6f);   // cumulative age ~1.116s > 1.0s lifetime
        check(ed.previewParticles().empty(), "and gone once age exceeds lifetime");
    }

    AVER_INFO("restartPreview clears state and re-arms the burst");
    {
        editor::ParticleEditor ed(path);
        ed.setEmission(0.0f, 7, 1000);
        ed.setLifetime(10.0f, 10.0f);
        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 7, "burst fired");
        ed.restartPreview();
        check(ed.previewParticles().empty(), "restart clears every live particle");
        ed.tickPreview(0.016f);
        check(ed.previewParticles().size() == 7, "and the burst fires again after a restart");
    }

    AVER_INFO("tickPreview is a no-op before load and for a non-positive dt");
    {
        editor::ParticleEditor missing(dir + "/nope.ocparticle");
        check(!missing.loaded(), "the fixture does not exist");
        missing.tickPreview(1.0f);
        check(missing.previewParticles().empty(), "an unloaded tab's preview never spawns anything");

        editor::ParticleEditor ed(path);
        ed.setEmission(0.0f, 5, 1000);
        ed.tickPreview(0.0f);
        check(ed.previewParticles().empty(), "a zero dt ticks nothing, not even the burst");
        ed.tickPreview(-1.0f);
        check(ed.previewParticles().empty(), "nor does a negative dt");
    }

    // ---- factory & missing file -----------------------------------------------------------------------
    AVER_INFO("the factory claims only .ocparticle, case-insensitively, and a missing file fails cleanly");
    {
        check(editor::makeParticleEditor("thing.ocsnd") == nullptr, "declines a .ocsnd");
        check(editor::makeParticleEditor("thing.txt") == nullptr, "declines a .txt");
        check(editor::makeParticleEditor("thing.OCPARTICLE") != nullptr,
              "claims .OCPARTICLE, case-insensitively");

        editor::ParticleEditor missing(dir + "/does-not-exist.ocparticle");
        check(!missing.loaded(), "a missing file leaves the tab not loaded");
        check(!missing.loadError().empty(), "with a reason to show");
        std::string why;
        check(!missing.save(&why), "and saving is refused rather than writing a default effect over it");
        check(why.find("failed to load") != std::string::npos, "saying why: " + why);
    }

    AVER_INFO("onFileChanged reloads a clean tab but keeps a dirty one's edits");
    {
        editor::ParticleEditor ed(path);
        check(ed.loaded(), "loaded");
        ed.setDamping(0.42f);
        check(ed.dirty(), "edited");
        ed.onFileChanged();
        check(std::fabs(ed.effect().damping - 0.42f) < 1e-6f,
              "a DIRTY tab keeps its edit across onFileChanged rather than discarding it");

        std::string why;
        ed.save(&why);
        check(!ed.dirty(), "saved, now clean");

        // A second, independent handle sees the same file on disk.
        editor::ParticleEditor ed2(path);
        check(std::fabs(ed2.effect().damping - 0.42f) < 1e-6f, "the second handle sees the saved value");
    }

    if (g_failures == 0) AVER_INFO("ParticleEditorLoadSaveTest: all checks passed");
    else AVER_ERROR("ParticleEditorLoadSaveTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
