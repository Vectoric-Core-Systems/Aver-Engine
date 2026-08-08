#pragma once
// The editor's Euler <-> quaternion pair. NOW A RE-EXPORT: the functions themselves live in
// modules/world/include/aver/world/LevelTransform.hpp.
//
// They lived here because they had lived as two statics inside SandboxApp.cpp, unreachable from any
// test, and one of them was wrong for eight months in a way that corrupted saved levels. Extracting
// them fixed that. This header then declined to promote them further, on the grounds that it "would
// put editor UI conventions into the engine's dependency graph".
//
// WHAT CHANGED. That reasoning held while these were an editor concern. They are not: this is the
// .ocworld ROTATION CONTRACT, and modules/runtime.game/src/GameLevel.cpp had already been forced to
// carry its own byte-identical copy of quatFromEulerDeg in order to read a level the editor wrote.
// A second copy inside the engine is the evidence that the engine owns it.
//
// THE ORIGINAL OBJECTION IS ANSWERED, NOT OVERRIDDEN. LevelTransform.hpp is header-only and depends
// on Aver.Core alone -- no Formats, no Scene, no Physics -- so tests/editor still compiles it
// through an include directory with no link, and the dependency graph does not move. Every call site
// in SandboxApp.cpp and the existing EditorEulerTest are unchanged, because the names still resolve
// in aver::editor.
#include "aver/world/LevelTransform.hpp"

namespace aver::editor {

using aver::world::quatFromEulerDeg;
using aver::world::eulerDegFromQuat;

} // namespace aver::editor
