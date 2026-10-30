#pragma once
// Shared by the text scene parser (GodotScene.cpp) and the binary one (GodotBinary.cpp): where a node goes once its name, parent path and
// instance are known. Not part of the module's public headers.
#include "aver/formats/godot/GodotScene.hpp"

#include <string_view>
#include <unordered_map>

namespace aver::fmt::godot {

// Sets the node's override flag, scene-relative path and parent index, then appends it. `n.isRoot` and `n.parentPath` (the parent's scene path,
// "." for the root) must be filled in; `pathIndex` maps the scene paths already placed to their index. A parent that is not a node of this file
// is the anchor case (an instanced sub-scene's child) or an error.
void godotPlaceNode(GodotScene& scene, std::unordered_map<std::string, int>& pathIndex, GodotNode n, std::string_view file, GodotDiagnostics& diag);

}  // namespace aver::fmt::godot
