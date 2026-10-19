// The particle editor's edit logic (sandbox/src/ParticleStackModel.hpp), with no ImGui and no window: every edit function,
// the clamping the .ocparticle parser's own rules demand, the cascades when an emitter or a parameter goes away, and the
// round trip of each result through writeOcparticleSystem / parseOcparticleSystem -- the property the header promises is
// "a typed value can never be one the save path would go on to refuse", so each edit here is ended by a write + parse.
#include "ParticleStackModel.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcParticle.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace aver;
namespace px = aver::editor::pxmodel;
using particles::ModuleKind;
using particles::ParticleEffect;
using particles::ParticleSystemDef;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static ParticleSystemDef oneEmitter(const char* name = "Main") {
    ParticleSystemDef d;
    ParticleEffect e;
    e.name = name;
    d.emitters.push_back(e);
    return d;
}

// Writes, parses and returns the parsed system; `ok` false (and a failure recorded) when the parser refused the writer's own text.
static ParticleSystemDef roundTrip(const ParticleSystemDef& d, const std::string& what, bool* ok = nullptr) {
    const std::string text = fmt::writeOcparticleSystem(d);
    ParticleSystemDef back;
    std::string err;
    const bool parsed = fmt::parseOcparticleSystem(text, back, nullptr, &err);
    check(parsed, what + ": the writer's text parses" + (parsed ? "" : " (" + err + ")"));
    if (ok) *ok = parsed;
    return back;
}

int main() {
    AVER_INFO("ParticleStackModelTest");

    // ---- names ---------------------------------------------------------------------------------------
    AVER_INFO("names");
    {
        check(px::validEmitterName("Sparks") && px::validEmitterName("Fire_01") && px::validEmitterName("a.b-c"), "ordinary emitter names are valid");
        check(!px::validEmitterName("") && !px::validEmitterName("has space") && !px::validEmitterName(std::string(49, 'x')), "empty, spaced and over-long names are not");
        check(px::validParamName("Intensity") && !px::validParamName("a.b") && !px::validParamName("9lives") && !px::validParamName("a-b"),
              "a parameter name has no '.', no '-' and does not start with a digit");
        check(px::sanitizeName("Big Fire") == "Big_Fire", "spaces become underscores");
        ParticleSystemDef d = oneEmitter("Main");
        check(px::uniqueEmitterName(d, "Main") == "Main2", "a taken name gets a numeric suffix");
        check(px::uniqueEmitterName(d, "Main", 0) == "Main", "the emitter being renamed does not collide with itself");
    }

    // ---- emitters ------------------------------------------------------------------------------------
    AVER_INFO("emitters");
    {
        ParticleSystemDef d = oneEmitter("Main");
        const int b = px::addEmitter(d, "Smoke");
        check(b == 1 && d.emitters[1].name == "Smoke", "addEmitter appends and names it");
        const int c = px::addEmitter(d);
        check(c == 2 && d.emitters[2].name == "Emitter", "a blank name becomes 'Emitter'");
        check(px::addEmitter(d, "Smoke") == 3 && d.emitters[3].name == "Smoke2", "a duplicate name is made unique");
        while (d.emitters.size() < particles::kMaxEmittersPerSystem) px::addEmitter(d, "Fill" + std::to_string(d.emitters.size()));
        check(px::addEmitter(d, "TooMany") == -1, "the emitter cap is respected");
        bool ok = false;
        roundTrip(d, "a full system", &ok);
        check(ok, "a full system round-trips");

        ParticleSystemDef e = oneEmitter("A");
        px::addEmitter(e, "B");
        px::addEmitter(e, "C");
        check(px::addEvent(e, 0, 0, 1, 3, 0.5f), "A -> B on death");
        check(px::addEvent(e, 1, 1, 2, 2, 0.0f), "B -> C on collide");
        check(!px::addEvent(e, 2, 0, 0, 1, 0.0f), "C -> A would close a loop and is refused");
        check(!px::addEvent(e, 1, 0, 1, 1, 0.0f), "B -> B is refused");
        check(!px::addEvent(e, 0, 0, 1, 0, 0.0f), "a count of 0 is refused");
        check(!px::addEvent(e, 0, 2, 1, 1, 0.0f), "an unknown event kind is refused");
        check(!px::addEvent(e, 0, 0, 7, 1, 0.0f), "a missing child is refused");

        const int dup = px::duplicateEmitter(e, 0);
        check(dup == 1 && e.emitters[1].name == "A_copy", "duplicate lands right after the original, named _copy");
        check(e.emitters[1].events.empty(), "and carries no event rules");
        check(e.emitters[0].events.size() == 1 && e.emitters[0].events[0].emitter == 2, "the original's rule follows B to its new index 2");
        check(e.emitters[2].events.size() == 1 && e.emitters[2].events[0].emitter == 3, "B's rule follows C to index 3");
        roundTrip(e, "after duplicate");

        check(px::moveEmitter(e, 3, 0), "move C to the front");
        check(e.emitters[0].name == "C" && e.emitters[1].name == "A", "the order changed");
        check(e.emitters[3].name == "B" && e.emitters[3].events.size() == 1 && e.emitters[3].events[0].emitter == 0, "B (now at 3) still points at C (now at 0)");
        check(e.emitters[1].events.size() == 1 && e.emitters[1].events[0].emitter == 3, "A (now at 1) still points at B (now at 3)");
        check(!px::moveEmitter(e, 1, 1) && !px::moveEmitter(e, -1, 0) && !px::moveEmitter(e, 0, 9), "a no-op or out-of-range move is refused");
        roundTrip(e, "after move");

        check(px::removeEmitter(e, 3), "remove B");
        check(e.emitters.size() == 3, "three left");
        bool anyDangling = false;
        for (const ParticleEffect& o : e.emitters)
            for (const auto& ev : o.events) anyDangling = anyDangling || ev.emitter >= e.emitters.size();
        check(!anyDangling, "no event rule points past the end");
        check(e.emitters[1].events.empty(), "A's rule onto the removed emitter went with it");
        roundTrip(e, "after remove");

        ParticleSystemDef one = oneEmitter();
        check(!px::removeEmitter(one, 0), "the last emitter cannot be removed");

        ParticleSystemDef r = oneEmitter("A");
        px::addEmitter(r, "B");
        check(px::renameEmitter(r, 0, "Alpha Beta") && r.emitters[0].name == "Alpha_Beta", "rename sanitises spaces");
        check(!px::renameEmitter(r, 0, "B"), "rename onto a taken name is refused");
        check(!px::renameEmitter(r, 0, ""), "an empty name is refused");
        check(!px::renameEmitter(r, 0, "Alpha_Beta"), "renaming to the same name changes nothing");
    }

    // ---- modules -------------------------------------------------------------------------------------
    AVER_INFO("modules");
    {
        ParticleSystemDef d = oneEmitter();
        ParticleEffect& e = d.emitters[0];
        int added = 0;
        for (u32 k = 0; k < static_cast<u32>(ModuleKind::Count); ++k)
            if (px::addModule(e, static_cast<ModuleKind>(k)) >= 0) ++added;
        check(added == static_cast<int>(std::min<u32>(static_cast<u32>(ModuleKind::Count), particles::kMaxModulesPerEmitter)), "every module kind can be added up to the cap");
        check(px::addModule(e, static_cast<ModuleKind>(200)) == -1, "an unknown kind is refused");
        roundTrip(d, "with every module");

        ParticleSystemDef m = oneEmitter();
        ParticleEffect& me = m.emitters[0];
        px::addModule(me, ModuleKind::UpdateForce);
        px::addModule(me, ModuleKind::UpdateDrag);
        px::addModule(me, ModuleKind::UpdateCurlNoise);
        check(px::moveModule(me, 2, 0) && me.modules[0].kind == ModuleKind::UpdateCurlNoise && me.modules[1].kind == ModuleKind::UpdateForce, "move reorders the stack");
        check(!px::moveModule(me, 0, 0) && !px::moveModule(me, 0, 5) && !px::moveModule(me, -1, 0), "bad moves are refused");
        check(px::setModuleEnabled(me, 1, false) && !me.modules[1].enabled, "disable a module");
        check(!px::setModuleEnabled(me, 1, false), "disabling it again changes nothing");
        bool ok = false;
        ParticleSystemDef back = roundTrip(m, "a reordered, partly disabled stack", &ok);
        if (ok) {
            check(back.emitters[0].modules.size() == 3 && back.emitters[0].modules[0].kind == ModuleKind::UpdateCurlNoise, "order survives the file");
            check(!back.emitters[0].modules[1].enabled, "the disabled flag survives the file");
        }
        check(px::removeModule(me, 0) && me.modules.size() == 2, "remove a module");
        check(!px::removeModule(me, 9), "an out-of-range remove is refused");

        // literal edits
        check(px::setModuleParam(me, 0, 0, 12.5f) && me.modules[0].p[0].value == 12.5f, "set a literal");
        check(!px::setModuleParam(me, 0, 0, 12.5f), "setting the same value changes nothing");
        check(!px::setModuleParam(me, 0, 0, std::nanf("")), "NaN is refused");
        check(!px::setModuleParam(me, 0, 7, 1.0f), "a parameter past the module's count is refused");
        ParticleSystemDef c = oneEmitter();
        px::addModule(c.emitters[0], ModuleKind::UpdateCurlNoise);
        const particles::ModuleInfo& ci = particles::moduleInfo(ModuleKind::UpdateCurlNoise);
        check(ci.paramCount > 3, "curl noise has an octaves parameter at index 3");
        px::setModuleParam(c.emitters[0], 0, 3, 99.0f);
        check(c.emitters[0].modules[0].p[3].value == 4.0f, "octaves clamp to 4");
        px::setModuleParam(c.emitters[0], 0, 3, -5.0f);
        check(c.emitters[0].modules[0].p[3].value == 1.0f, "and to 1");
        px::setModuleParam(c.emitters[0], 0, 3, 2.7f);
        check(c.emitters[0].modules[0].p[3].value == 2.0f, "and are whole numbers");
        roundTrip(c, "curl noise octaves");
    }

    // ---- user parameters and binding -----------------------------------------------------------------
    AVER_INFO("user parameters");
    {
        ParticleSystemDef d = oneEmitter();
        ParticleEffect& e = d.emitters[0];
        px::addModule(e, ModuleKind::UpdateForce);
        const int s0 = px::addUserParam(d, "Wind", 1);
        const int s1 = px::addUserParam(d, "Intensity", 0);
        const int s2 = px::addUserParam(d, "Tint", 2);
        check(s0 == 0 && s1 == 1 && s2 == 2, "three parameters, in order");
        check(d.params[1].v[0] == 1.0f, "a new float parameter starts at 1");
        check(d.params[2].v[0] == 1.0f && d.params[2].v[3] == 1.0f, "a new colour parameter starts white");
        check(px::addUserParam(d, "Wind", 0) == 3 && d.params[3].name == "Wind2", "a taken name is made unique");
        check(px::addUserParam(d, "bad.name", 0) >= 0 && px::validParamName(d.params.back().name), "an invalid name falls back to a valid one");
        check(px::addUserParam(d, "X", 9) == -1, "an unknown kind is refused");
        px::removeUserParam(d, 4);
        px::removeUserParam(d, 3);

        check(px::bindModuleParam(d, 0, 0, 0, 0, 2), "bind force x to Wind.z");
        check(e.modules[0].p[0].userSlot == 0 && e.modules[0].p[0].comp == 2, "recorded");
        check(!px::bindModuleParam(d, 0, 0, 0, 0, 2), "the same binding again changes nothing");
        check(!px::bindModuleParam(d, 0, 0, 1, 1, 1), "a float parameter has only component 0");
        check(!px::bindModuleParam(d, 0, 0, 1, 9, 0), "an unknown slot is refused");
        check(!px::bindModuleParam(d, 0, 9, 1, 0, 0), "an unknown module is refused");
        check(px::bindModuleParam(d, 0, 0, 1, 1, 0), "bind force y to Intensity");
        bool ok = false;
        ParticleSystemDef back = roundTrip(d, "bound parameters", &ok);
        if (ok) {
            check(back.params.size() == 3 && back.params[0].name == "Wind", "the parameters survive the file");
            check(back.emitters[0].modules[0].p[0].userSlot == 0 && back.emitters[0].modules[0].p[0].comp == 2, "so does the binding");
        }

        check(px::unbindModuleParam(e, 0, 1) && e.modules[0].p[1].userSlot < 0, "unbind");
        check(!px::unbindModuleParam(e, 0, 1), "unbinding twice changes nothing");
        px::bindModuleParam(d, 0, 0, 1, 1, 0);

        const f32 v3[4] = {1.0f, 2.0f, 3.0f, 9.0f};
        check(px::setUserParamValue(d, 0, v3) && d.params[0].v[2] == 3.0f, "set a vec3 value");
        check(d.params[0].v[3] == 0.0f, "the unused fourth component is left alone");
        check(!px::setUserParamValue(d, 0, v3), "the same value changes nothing");
        const f32 bad[4] = {std::nanf(""), 0, 0, 0};
        check(!px::setUserParamValue(d, 0, bad), "NaN is refused");

        check(px::setUserParamKind(d, 0, 0), "vec3 -> float");
        check(e.modules[0].p[0].userSlot < 0, "a binding to the removed Wind.z went back to its literal");
        check(e.modules[0].p[1].userSlot == 1, "a binding to another parameter is untouched");
        check(!px::setUserParamKind(d, 0, 0) && !px::setUserParamKind(d, 0, 7), "same or unknown kind is refused");

        check(px::renameUserParam(d, 0, "Gust") && d.params[0].name == "Gust", "rename a parameter");
        check(!px::renameUserParam(d, 0, "Intensity"), "onto a taken name is refused");
        check(!px::renameUserParam(d, 0, "9lives"), "a name starting with a digit is refused");

        check(px::removeUserParam(d, 0), "remove Gust");
        check(e.modules[0].p[1].userSlot == 0, "Intensity's slot shifted down and the binding followed");
        check(!px::removeUserParam(d, 9), "an out-of-range remove is refused");
        roundTrip(d, "after parameter removal");
    }

    // ---- curves --------------------------------------------------------------------------------------
    AVER_INFO("curves");
    {
        particles::Curve4 c;
        const f32 one[4] = {1, 1, 1, 1};
        const f32 two[4] = {2, 2, 2, 2};
        const f32 three[4] = {3, 3, 3, 3};
        check(px::addCurvePoint(c, 0.5f, one) && c.n == 2, "the first point makes a valid two-stop curve");
        check(c.t[0] == 0.0f && c.t[1] == 1.0f, "at 0 and 1");
        check(px::addCurvePoint(c, 0.5f, two) && c.n == 3 && c.t[1] == 0.5f && c.v[1][0] == 2.0f, "a point lands in time order");
        check(!px::addCurvePoint(c, 0.5f, three), "a point on an existing stop is refused");
        check(px::addCurvePoint(c, 0.25f, three) && c.n == 4 && c.t[1] == 0.25f && c.t[2] == 0.5f, "a fourth point");
        check(!px::addCurvePoint(c, 0.75f, one), "a fifth is refused");
        check(px::setCurvePoint(c, 1, 0.9f, two), "move a stop");
        check(c.t[1] < c.t[2], "a stop cannot pass its neighbour");
        check(px::setCurvePoint(c, 0, -3.0f, one) || c.t[0] == 0.0f, "the first stop stays at 0 or above");
        check(!px::setCurvePoint(c, 9, 0.5f, one), "an unknown stop is refused");
        for (u32 i = 1; i < c.n; ++i) check(c.t[i] > c.t[i - 1], "times strictly increase");
        check(px::removeCurvePoint(c, 1) && c.n == 3, "remove a stop");
        px::removeCurvePoint(c, 1);
        check(c.n == 2, "down to two");
        check(px::removeCurvePoint(c, 0) && c.n == 0, "removing from two clears the curve (no one-stop curve exists)");
        check(!px::removeCurvePoint(c, 0) , "removing from an empty curve changes nothing");

        // a curve written by the editor is a curve the parser accepts
        ParticleSystemDef d = oneEmitter();
        px::addCurvePoint(d.emitters[0].sizeCurve, 0.5f, one);
        px::addCurvePoint(d.emitters[0].sizeCurve, 0.25f, two);
        bool ok = false;
        ParticleSystemDef back = roundTrip(d, "a size curve", &ok);
        if (ok) check(back.emitters[0].sizeCurve.n == 3, "and it is still a three-stop curve");
    }

    // ---- compute bounds ------------------------------------------------------------------------------
    AVER_INFO("compute bounds");
    {
        ParticleSystemDef d = oneEmitter();
        ParticleEffect& e = d.emitters[0];
        e.burstCount = 50;
        e.emissionRate = 0.0f;
        e.maxParticles = 200;
        e.lifetimeMin = e.lifetimeMax = 2.0f;
        e.speedMin = e.speedMax = 100.0f;
        e.spreadDeg = 180.0f;
        e.gravity = Vec3{0, 0, 0};
        const f32 r1 = px::computeBoundsRadius(d, 0);
        check(r1 > 150.0f && r1 < 400.0f, "a 100 cm/s, 2 s burst reaches about 200 cm (x1.2 and the half size): got " + std::to_string(r1));
        e.speedMin = e.speedMax = 200.0f;
        const f32 r2 = px::computeBoundsRadius(d, 0);
        check(r2 > r1 * 1.5f, "double the speed gives a clearly larger radius");
        check(px::computeBoundsRadius(d, 5) == 0.0f, "an unknown emitter answers 0");
        check(px::computeBoundsRadius(d, 0) == r2, "and the answer is deterministic");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
