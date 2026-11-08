#pragma once
// Starter effects and module presets for the particle editor, authored as .ocparticle text with the engine's own modules and no
// texture or mesh other than the built-ins (the procedural soft dot, Meshes/cube.ocmesh). ImGui-free: the Content Browser's
// "New Particle Effect from template" menu and the editor's preset popup read this table, and tests/editor's
// ParticleEditorTrackCTest parses and simulates every entry.
#include "ParticleStackModel.hpp"

#include "aver/formats/OcParticle.hpp"
#include "aver/particles/ParticleModules.hpp"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor::pxtemplates {

struct Template {
    const char* name;
    const char* description;
    const char* text;   // a complete .ocparticle file
};

inline const std::vector<Template>& templates() {
    static const std::vector<Template> t = {
        {"Smoke", "A slow grey column that widens, turns and fades.",
         "OCPARTICLE 2\nNAME Smoke\n"
         "EMITTER Smoke\nSHAPE disc 12\nBLEND premultiplied\nEMISSION 22 0 400\nLIFETIME 3 5\nDIRECTION 0 0 1 14\nSPEED 40 80\n"
         "GRAVITY 0 0 12\nDAMPING 0.2\nSIZE 30 110\nSIZECURVE 0 30 0.3 70 1 120\n"
         "COLORCURVE 0 0.5 0.5 0.52 0 0.15 0.5 0.5 0.52 0.45 0.6 0.32 0.32 0.34 0.3 1 0.2 0.2 0.22 0\n"
         "LIGHTING ambient\nSORT auto\nBOUNDS 500\n"
         "MODULE spawn.rotation minDeg=0 maxDeg=360 spinMin=-25 spinMax=25\n"
         "MODULE spawn.sizescale min=0.7 max=1.3\n"
         "MODULE update.curlnoise freq=0.008 strength=45 scroll=0.15 octaves=2 seed=3\n"},
        {"Sparks", "A burst of hot streaks that fall and bounce on the ground plane.",
         "OCPARTICLE 2\nNAME Sparks\n"
         "EMITTER Sparks\nSHAPE sphere 4\nBLEND additive\nEMISSION 0 90 512\nLIFETIME 0.6 1.4\nDIRECTION 0 0 1 60\nSPEED 300 700\n"
         "GRAVITY 0 0 -980\nDAMPING 0.02\nSIZE 2.5 1\n"
         "COLORCURVE 0 1 0.95 0.7 1 0.3 1 0.55 0.15 0.9 1 0.2 0.04 0.01 0\n"
         "SPRITE velocity stretch 0.5\nLIGHTING unlit\nGI off\nSORT none\nBOUNDS 900\n"
         "MODULE update.collideplane nx=0 ny=0 nz=1 d=0 restitution=0.45 friction=0.3 kill=0\n"},
        {"Rain", "Fast thin streaks falling from a wide box above the origin.",
         "OCPARTICLE 2\nNAME Rain\n"
         "EMITTER Rain\nSHAPE box 600 600 2\nBLEND premultiplied\nEMISSION 700 0 1400\nLIFETIME 0.7 0.9\nDIRECTION 0 0 -1 1.5\nSPEED 1200 1500\n"
         "GRAVITY 0 0 0\nSIZE 1.6 1.6\nCOLOR start 0.7 0.8 1 0.4\nCOLOR end 0.7 0.8 1 0.4\n"
         "SPRITE velocity stretch 2\nLIGHTING ambient\nSORT none\nBOUNDS 900\n"
         "MODULE update.force x=40 y=0 z=0\n"},
        {"Snow", "Large soft flakes drifting down on a light wandering wind.",
         "OCPARTICLE 2\nNAME Snow\n"
         "EMITTER Snow\nSHAPE box 500 500 2\nBLEND premultiplied\nEMISSION 90 0 1200\nLIFETIME 8 11\nDIRECTION 0 0 -1 12\nSPEED 25 55\n"
         "GRAVITY 0 0 0\nDAMPING 0.1\nSIZE 3 3\nCOLOR start 1 1 1 0.9\nCOLOR end 1 1 1 0.9\n"
         "LIGHTING ambient\nSORT none\nBOUNDS 800\n"
         "MODULE spawn.sizescale min=0.5 max=1.6\n"
         "MODULE update.curlnoise freq=0.006 strength=30 scroll=0.1 octaves=2 seed=11\n"},
        {"Embers", "Glowing motes that rise on turbulence and cool from yellow to red.",
         "OCPARTICLE 2\nNAME Embers\nPARAM Turbulence float 60\n"
         "EMITTER Embers\nSHAPE disc 60 shell\nBLEND additive\nEMISSION 45 0 600\nLIFETIME 2 4.5\nDIRECTION 0 0 1 25\nSPEED 30 80\n"
         "GRAVITY 0 0 25\nDAMPING 0.2\nSIZE 3 1\nSIZECURVE 0 1.2 0.2 3 0.7 2 1 0\n"
         "COLORCURVE 0 1 0.85 0.4 0 0.15 1 0.55 0.15 1 0.7 1 0.25 0.05 0.9 1 0.3 0.1 0.02 0\n"
         "LIGHTING unlit\nGI off\nSORT none\nBOUNDS 700\n"
         "MODULE spawn.sizescale min=0.6 max=1.5\n"
         "MODULE update.curlnoise freq=0.012 strength=@Turbulence.x scroll=0.3 octaves=2 seed=7\n"},
        {"Dust", "Faint motes hanging in a volume, barely moving.",
         "OCPARTICLE 2\nNAME Dust\n"
         "EMITTER Dust\nSHAPE sphere 250\nBLEND premultiplied\nEMISSION 18 0 300\nLIFETIME 5 9\nDIRECTION 0 0 1 180\nSPEED 4 14\n"
         "GRAVITY 0 0 -2\nDAMPING 0.3\nSIZE 1.8 1.8\nCOLORCURVE 0 0.9 0.85 0.75 0 0.2 0.9 0.85 0.75 0.28 0.8 0.9 0.85 0.75 0.28 1 0.9 0.85 0.75 0\n"
         "LIGHTING ambient\nSORT auto\nBOUNDS 450\n"
         "MODULE spawn.sizescale min=0.5 max=1.8\n"
         "MODULE update.curlnoise freq=0.005 strength=12 scroll=0.08 octaves=1 seed=5\n"},
        {"Mist", "Big low sprites that swell and thin out along the ground.",
         "OCPARTICLE 2\nNAME Mist\n"
         "EMITTER Mist\nSHAPE disc 180\nBLEND premultiplied\nEMISSION 5 0 120\nLIFETIME 6 9\nDIRECTION 0 0 1 8\nSPEED 4 12\n"
         "GRAVITY 0 0 2\nDAMPING 0.4\nSIZE 70 200\nCOLORCURVE 0 0.8 0.85 0.9 0 0.25 0.8 0.85 0.9 0.14 0.75 0.8 0.85 0.9 0.14 1 0.8 0.85 0.9 0\n"
         "LIGHTING ambient\nSORT auto\nBOUNDS 500\n"
         "MODULE spawn.rotation minDeg=0 maxDeg=360 spinMin=-8 spinMax=8\n"
         "MODULE update.curlnoise freq=0.004 strength=18 scroll=0.05 octaves=2 seed=9\n"},
        {"Trail", "Particles that swirl around a point, each dragging a ribbon. Move the emitter in the preview to see it stretch.",
         "OCPARTICLE 2\nNAME Trail\n"
         "EMITTER Trail\nSHAPE sphere 6\nBLEND additive\nEMISSION 14 0 128\nLIFETIME 1.5 2.5\nDIRECTION 0 0 1 180\nSPEED 80 160\n"
         "GRAVITY 0 0 0\nDAMPING 0.1\nSIZE 5 1\nCOLORCURVE 0 0.4 0.8 1 1 0.6 0.4 0.9 1 0.2 1 0.3 0.9 0.8 0\n"
         "RENDER ribbon trail 18 5 fade 1\nLIGHTING unlit\nGI off\nSORT none\nBOUNDS 600\n"
         "MODULE update.vortex ax=0 ay=0 az=1 strength=220 radius=400 lift=10\n"
         "MODULE spawn.inheritvelocity scale=1\n"},
        {"Debris", "Small cubes thrown out of a point, tumbling and bouncing on the ground plane.",
         "OCPARTICLE 2\nNAME Debris\n"
         "EMITTER Debris\nSHAPE sphere 6\nBLEND opaque\nEMISSION 0 28 128\nLIFETIME 2.5 3.5\nDIRECTION 0 0 1 55\nSPEED 250 550\n"
         "GRAVITY 0 0 -980\nDAMPING 0.01\nSIZE 5 5\nCOLOR start 0.55 0.5 0.45 1\nCOLOR end 0.55 0.5 0.45 1\n"
         "RENDER mesh Meshes/cube.ocmesh spin\nLIGHTING lit\nSORT none\nBOUNDS 900\n"
         "MODULE spawn.sizescale min=0.5 max=1.6\n"
         "MODULE spawn.rotation minDeg=0 maxDeg=360 spinMin=-360 spinMax=360\n"
         "MODULE update.collideplane nx=0 ny=0 nz=1 d=0 restitution=0.35 friction=0.5 kill=0\n"},
        {"Flipbook burst", "A burst whose particles each start on a random frame of a 4 x 4 sheet. Assign a sheet texture under Render; without one the dot is drawn.",
         "OCPARTICLE 2\nNAME Flipbook Burst\n"
         "EMITTER Burst\nSHAPE sphere 8\nBLEND additive\nEMISSION 0 24 128\nLIFETIME 0.8 1.2\nDIRECTION 0 0 1 180\nSPEED 40 120\n"
         "GRAVITY 0 0 10\nDAMPING 0.5\nSIZE 40 90\nSUBUV 4 4 16 life 0\n"
         "COLORCURVE 0 1 0.9 0.7 1 0.3 1 0.8 0.5 0.8 1 0.2 0.1 0.05 0\n"
         "LIGHTING unlit\nGI off\nSORT none\nBOUNDS 400\n"
         "MODULE spawn.rotation minDeg=0 maxDeg=360 spinMin=-40 spinMax=40\n"
         "MODULE spawn.animstart min=0 max=1 frames=16\n"},
    };
    return t;
}

// Parses one template into a system. False (with `err`) only if a shipped template is malformed, which the test forbids.
inline bool instantiate(usize index, particles::ParticleSystemDef& out, std::string* err = nullptr) {
    const auto& all = templates();
    if (index >= all.size()) { if (err) *err = "no such template"; return false; }
    return fmt::parseOcparticleSystem(all[index].text, out, nullptr, err);
}

// ---- module presets: a handful of modules with tuned parameters, added to the selected emitter in one undo step ------------

struct PresetModule {
    particles::ModuleKind kind;
    const char* params;   // "name=value ..." in the module's own parameter names
};
struct Preset {
    const char* name;
    const char* description;
    std::vector<PresetModule> modules;
};

inline const std::vector<Preset>& presets() {
    using K = particles::ModuleKind;
    static const std::vector<Preset> p = {
        {"Turbulence", "Curl-noise swirling.", {{K::UpdateCurlNoise, "freq=0.01 strength=80 scroll=0.2 octaves=2"}}},
        {"Wind", "A steady push with some drag.", {{K::UpdateForce, "x=80 y=0 z=0"}, {K::UpdateDrag, "k=0.3"}}},
        {"Swirl", "Spin around the up axis, drifting upward.", {{K::UpdateVortex, "ax=0 ay=0 az=1 strength=180 radius=400 lift=15"}}},
        {"Ground bounce", "Bounce off the plane z = 0.", {{K::UpdateCollidePlane, "nz=1 d=0 restitution=0.45 friction=0.3"}}},
        {"Random spin", "Random start angle and spin speed.", {{K::SpawnRotation, "minDeg=0 maxDeg=360 spinMin=-90 spinMax=90"}}},
        {"Size variation", "Each particle 0.6 to 1.4 times the authored size.", {{K::SpawnSizeScale, "min=0.6 max=1.4"}}},
        {"Colour jitter", "A small per-particle hue turn.", {{K::SpawnHueVariation, "min=-0.04 max=0.04"}}},
        {"Slow to a stop", "Drag plus a constant deceleration.", {{K::UpdateDrag, "k=0.8"}, {K::UpdateDamping, "min=20 max=60"}}},
        {"Expand outward", "Accelerate away from the origin.", {{K::UpdateRadialAccel, "min=40 max=120"}}},
        {"Orbit", "Turn slowly about the up axis.", {{K::UpdateOrbit, "min=0.1 max=0.3 ax=0 ay=0 az=1"}}},
    };
    return p;
}

// Adds a preset's modules to the emitter (stops at the 16-module cap). Returns whether anything was added. Parameters that name
// something the module does not have are ignored, so a preset can never leave a half-applied module.
inline bool applyPreset(particles::ParticleEffect& e, usize index) {
    const auto& all = presets();
    if (index >= all.size()) return false;
    const Preset& p = all[index];
    if (e.modules.size() + p.modules.size() > particles::kMaxModulesPerEmitter) return false;
    for (const PresetModule& pm : p.modules) {
        const int at = pxmodel::addModule(e, pm.kind);
        if (at < 0) return false;
        const particles::ModuleInfo& info = particles::moduleInfo(pm.kind);
        std::string_view rest = pm.params;
        while (!rest.empty()) {
            const usize sp = rest.find(' ');
            const std::string_view tok = rest.substr(0, sp);
            rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
            const usize eq = tok.find('=');
            if (eq == std::string_view::npos) continue;
            const int pi = particles::findModuleParam(info, tok.substr(0, eq));
            if (pi < 0) continue;
            pxmodel::setModuleParam(e, at, pi, std::strtof(std::string(tok.substr(eq + 1)).c_str(), nullptr));
        }
    }
    return true;
}

} // namespace aver::editor::pxtemplates
