// Turning a level's declared SCATTER records into the runtime palette ChunkGenerator consumes.
//
// THE ONE PLACE BOTH HOSTS CALL. The editor (loading a level to drive chunk streaming) and the game
// runtime both link Aver.World already -- it is what stops "how a placement becomes an entity" from
// existing twice (see this module's CMakeLists.txt), and the identical narrowing from OcWorldData's
// f64 fields to ScatterSpecies's f32 ones belongs here for the same reason.
//
// VALIDATION LIVES HERE, NOT IN AVER.FORMATS. parseOcworld has no filesystem and no notion of a
// project's content root -- it turns text into OcWorldData and nothing else, the same way it does
// for PLACE's asset field. Checking "does this mesh actually exist" needs a content root, which is a
// host concern (the editor's project, the game's package), so it is a parameter here rather than
// something the text parser could ever answer on its own.
#pragma once

#include "aver/world/ChunkGenerator.hpp"

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"

#  include <string>
#  include <vector>

namespace aver::world {

// Converts a level's SCATTER records into the palette GeneratorSettings::palette expects.
//
// `contentDir`, if non-empty, is the project's content root; each species' meshPath is checked to
// exist under it. An empty contentDir skips ONLY that one check -- useful for a caller that has not
// resolved a content root yet, or a test with no real assets on disk -- every other validation still
// runs regardless.
//
// A species that fails validation is left OUT of `out` and reported in `errors`, one entry per
// species so every problem in a level surfaces at once rather than stopping at the first; the rest
// of the palette still builds from whatever validated cleanly. THE ALTERNATIVE -- silently dropping
// a bad species -- is a level that scatters nothing for a reason nobody can see, indistinguishable
// from the feature being broken.
//
// Returns true iff `errors` came back empty.
bool buildScatterPalette(const std::vector<fmt::OcScatterSpecies>& records,
                          const std::string& contentDir,
                          std::vector<ScatterSpecies>& out,
                          std::vector<std::string>& errors);

} // namespace aver::world
#endif // AVER_MODULE_SCENE
