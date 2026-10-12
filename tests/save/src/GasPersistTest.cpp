// Gameplay ability systems ride in a save, with a real .NET runtime in the process.
//
// WHAT ONLY THIS TEST PROVES. The managed tests (Aver.Framework.Tests) run against a fake host and
// SaveWorldTest runs against a fake provider; neither shows that the bridge actually (a) installed the
// managed-state provider at startup (GameplayAbilities.Startup in SetupManagedActors), (b) loaded the
// project's *.ocgas files at the per-Play-start call (ConfigureInput -> BeginSession, after
// DeclareGraphClasses remembered the content root), and (c) carries a system through
// capture -> file -> restore via the aver_fw_state_* seam. Each of those is a one-line call in
// HostBridge.cs that nothing else would notice missing.
//
// Drives aver_fw_state_set with a hand-written GASSTATE 1 blob, reads it back through
// aver_fw_state_count/at, then runs a real save::capture / file / save::restore with the state
// pointers relaying to aver_fw_state_*.
//
// UNAVAILABLE IS NOT A FAILURE: a machine with no .NET runtime, or a build with no staged bridge,
// reports so and exits 0 -- the same path the engine itself takes.
#include "aver/framework/framework_abi.h"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/framework/framework_state_abi.h"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/save/SaveWorld.hpp"
#include "aver/scene/World.hpp"
#include "aver/scripting/ScriptHost.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static const char* kDefs =
    "OCGAS 1\n"
    "ATTRSET Character\n"
    "  ATTR Health 100 min=0 max=MaxHealth\n"
    "  ATTR MaxHealth 100 min=1\n"
    "EFFECT Fx.Shield\n"
    "  POLICY duration 60\n"
    "  GRANTS Status.Shielded\n"
    "  MOD MaxHealth add 25\n"
    "ABILITY Ab.Strike\n"
    "  TAGS Ability.Strike\n";

static const char* kBlob =
    "GASSTATE 1\n"
    "SET Character\n"
    "ATTR Health 73.5\n"
    "ATTR MaxHealth 100\n"
    "LOOSE State.Alive 1\n"
    "GRANT Ab.Strike 2\n"
    "EFFECT Fx.Shield stacks=1 remaining=42.5 mags=25 source=none\n";

// The relays a composition root installs (Runtime GameApp.cpp / sandbox SandboxApp.hpp): this test IS one.
static i32 relayCount(scene::Entity e, void*) { return aver_fw_state_count(static_cast<i32>(e)); }
static i32 relayAt(scene::Entity e, i32 index, char* nameBuf, i32 nameCap, char* dataBuf, i32 dataCap, void*) {
    return aver_fw_state_at(static_cast<i32>(e), index, nameBuf, nameCap, dataBuf, dataCap);
}
static i32 relaySet(scene::Entity e, const char* name, const char* data, i32 len, void*) {
    return aver_fw_state_set(static_cast<i32>(e), name, data, len);
}

static std::string readPayload(scene::Entity e) {
    char name[64] = {};
    std::vector<char> data(512);
    i32 len = aver_fw_state_at(static_cast<i32>(e), 0, name, sizeof(name), data.data(), static_cast<i32>(data.size()));
    if (len < 0 || std::strcmp(name, "gas") != 0) return {};
    if (len > static_cast<i32>(data.size())) {
        data.assign(static_cast<usize>(len), '\0');
        len = aver_fw_state_at(static_cast<i32>(e), 0, name, sizeof(name), data.data(), len);
    }
    return std::string(data.data(), static_cast<usize>(len));
}

int main() {
    AVER_INFO("GasPersistTest");

    scripting::ScriptHost host;
    scripting::HostDesc hd;
    hd.bridgeDir  = executableDir() + AVER_PATH_SEP "Scripting";
    hd.scriptsDir = executableDir() + AVER_PATH_SEP "ActorScripts";
    if (!host.init(hd)) {
        AVER_WARN("GasPersistTest: UNAVAILABLE -- {}", host.declineReason());
        return 0;
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-gas-persist";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "Content", ec);
    {
        std::ofstream f(dir / "Content" / "combat.ocgas", std::ios::binary);
        f << kDefs;
    }

    // The per-Play-start path: DeclareGraphClasses remembers the content root, ConfigureInput begins the session.
    host.declareGraphClasses((dir / "Content").generic_string());
    const i32 cfg = host.configureInput("", "");
    if (cfg == -2) {
        AVER_WARN("GasPersistTest: UNAVAILABLE -- the staged bridge predates ConfigureInput");
        host.shutdown();
        return 0;
    }
    check(cfg == 0, "configureInput with no scheme succeeds (this is the BeginSession call)");
    check(aver_fw_state_abi_version() == AVER_FW_STATE_ABI_VERSION, "the state ABI version matches the header");

    scene::World& w = scene::World::instance();
    const scene::Entity hero = w.create("GasHero");
    check(hero != scene::kInvalidEntity, "an entity exists to carry the system");
    check(aver_fw_state_count(static_cast<i32>(hero)) == 0, "it has no managed state yet");

    AVER_INFO("a hand-written payload is accepted through the provider the bridge installed");
    {
        const i32 ok = aver_fw_state_set(static_cast<i32>(hero), "gas", kBlob, static_cast<i32>(std::strlen(kBlob)));
        check(ok == 1, "aver_fw_state_set(gas) is accepted -- Startup() installed the provider and the .ocgas loaded");
        check(aver_fw_state_set(static_cast<i32>(hero), "nope", kBlob, static_cast<i32>(std::strlen(kBlob))) == 0,
              "an unknown channel is refused");
        check(aver_fw_state_set(static_cast<i32>(hero), "gas", "GASSTATE 9\n", 11) == 0, "a bad header is refused");
        check(aver_fw_state_count(static_cast<i32>(hero)) == 1, "the entity now carries one channel");

        const std::string back = readPayload(hero);
        check(back.find("ATTR Health 73.5") != std::string::npos, "the attribute Base round-trips");
        check(back.find("EFFECT Fx.Shield") != std::string::npos && back.find("remaining=42.5") != std::string::npos,
              "the active effect and its remaining time round-trip");
        check(back.find("GRANT Ab.Strike 2") != std::string::npos, "the granted ability and its level round-trip");
        check(back.find("LOOSE State.Alive 1") != std::string::npos, "the loose tag round-trips");

        char tiny[8] = {};
        const i32 full = aver_fw_state_at(static_cast<i32>(hero), 0, nullptr, 0, tiny, sizeof(tiny));
        check(full == static_cast<i32>(back.size()), "a too-small buffer is told the full length so it can retry");
        check(aver_fw_state_at(static_cast<i32>(hero), 1, nullptr, 0, tiny, sizeof(tiny)) == -1, "a bad channel index is -1");
    }

    AVER_INFO("a full capture -> file -> restore carries the system to the new entity");
    {
        save::CaptureOptions co;
        co.host.stateCount = &relayCount;
        co.host.stateAt    = &relayAt;
        co.host.stateSet   = &relaySet;
        fmt::OcSaveData snap;
        std::string why;
        check(save::capture(w, snap, co, &why), "the world captures: " + why);

        bool sawState = false;
        for (const fmt::OcSaveEntity& en : snap.entities)
            if (en.name == "GasHero")
                for (const fmt::OcSaveComponent& sc : en.components)
                    if (sc.type == "$State" && sc.fields.size() == 1 && sc.fields[0].name == "gas" &&
                        sc.fields[0].s == readPayload(hero))
                        sawState = true;
        check(sawState, "the hero's record carries a $State component holding the gas payload");

        const std::string path = (dir / "slot0.ocsave").string();
        check(fmt::saveOcSave(path, snap, &why), "it writes to disk: " + why);
        fmt::OcSaveData loaded;
        check(fmt::loadOcSave(path, loaded, &why), "and loads back: " + why);

        const std::string before = readPayload(hero);
        save::RestoreOptions ro;
        ro.host = co.host;
        check(save::restore(loaded, w, ro, &why), "it restores: " + why);
        const scene::Entity again = w.find("GasHero");
        check(again != scene::kInvalidEntity, "the hero is back");
        check(again != hero, "under a NEW handle, so the state below came from the restore and not from the old entity");
        check(readPayload(again) == before, "and its ability system is byte for byte what was saved");
    }

    host.shutdown();
    std::filesystem::remove_all(dir, ec);

    AVER_INFO(g_failures ? "GasPersistTest: {} FAILURES" : "GasPersistTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
