#pragma once
// The animation editor tab: a .ocanim or .ocskel opened as an asset, scrubbed against a skeleton
// drawn as one box per bone.
#include "AssetEditor.hpp"

#include <memory>
#include <string>

namespace aver::editor {

// Creates an animation editor for a .ocanim or .ocskel, else nullptr.
std::unique_ptr<AssetEditor> makeAnimEditor(const std::string& path);

// Unregisters the animation editor's GPU skinning feature and frees its meshes. Repeatable: the
// feature is recreated lazily by the next anim tab. Run when no anim tab is open.
void releaseAnimEditorGpu();

// releaseAnimEditorGpu() for app exit. Must run before the device is torn down -- see the
// definition for what happens when it does not.
void shutdownAnimEditors();

// Sets the project's content root, which a clip's skeletonRef is resolved against.
void setAnimEditorContentRoot(std::string root);

} // namespace aver::editor
