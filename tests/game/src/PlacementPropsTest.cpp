// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// PlacementPropsTest: a placed actor's per-instance [Editable] values -- a level's `COMPONENT Props` record
// into the framework's per-entity store and back out, through the real .ocworld parser and writer. No device.

#include "aver/game/PlacementProps.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/scene/scene_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) AVER_INFO("   ok    {}", what);
    else { ++g_failures; AVER_ERROR("   FAIL  {}", what); }
}

int main() {
    AVER_INFO("PlacementProps test");

    fmt::OcWorldData w;
    std::string why;
    const char* text =
        "OCWORLD 1\n"
        "PLACE Meshes/door.ocmesh 0 0 0 0 0 0 1 class Door\n"
        "  COMPONENT Props Speed=2.5 Label=front%20door Locked=true\n";
    check(fmt::parseOcworld(text, w, &why), "the level parses");
    if (!why.empty()) AVER_INFO("   parse said: {}", why);
    check(w.placements.size() == 1 && w.placements[0].components.size() == 1, "one placement with one Props record");

    const int32_t e = aver_scene_create();
    check(game::applyPlacementProps(e, w.placements[0]) == 3, "three values reach the entity's store");
    check(aver_fw_props_count(e) == 3, "the store holds three");
    check(std::string(aver_fw_props_value(e, 1)) == "front door", "a value is stored decoded");
    check(aver_fw_props_set(e, "SPEED", "4") == 1 && aver_fw_props_count(e) == 3,
          "a name set again in another case replaces, not adds");
    check(std::string(aver_fw_props_value(e, 0)) == "4", "the replaced value is the new one");

    fmt::OcWorldPlacement saved = w.placements[0];
    game::capturePlacementProps(e, saved);
    check(saved.components.size() == 1 && saved.components[0].kv.size() == 3, "capture writes one Props record back");
    w.placements[0] = saved;
    const std::string written = fmt::writeOcworld(w);
    check(written.find("COMPONENT Props") != std::string::npos && written.find("front%20door") != std::string::npos,
          "the writer emits it, encoded");
    fmt::OcWorldData again;
    check(fmt::parseOcworld(written, again, &why) && again.placements.size() == 1 &&
          again.placements[0].components == saved.components, "and it reads back the same");

    check(aver_fw_destroy(e) == 1 && aver_fw_props_count(e) == 0, "destroying the entity drops its values");
    const int32_t bare = aver_scene_create();
    fmt::OcWorldPlacement none;
    game::capturePlacementProps(bare, none);
    check(none.components.empty(), "an entity with no values writes no Props record");

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
