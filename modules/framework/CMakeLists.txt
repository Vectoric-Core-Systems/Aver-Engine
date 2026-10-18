# Aver.Framework — the gameplay layer (see README.md).
#
# SHARED for the same P/Invoke-identity reason as Aver.Scene and Aver.Render.PBR: the C# layer calls
# into this DLL, and one binary means one set of classes and instances.
#
# It links Aver.Scene because gameplay stands ON storage and the arrow must point down. That makes
# this the tree's first SHARED-links-SHARED edge, so the rule it is tested against is worth stating
# where the link line is: modules/render.pbr/CMakeLists.txt requires that no RHI type sit behind a
# P/Invoke DLL. The transitive closure of this target is {Core, Assets, Scene, Platform, Formats} —
# Formats joined at the INPUT SCHEME section (framework_abi.h), which parses .ocinput through
# Aver.Formats's own aver::fmt::parseOcinput rather than duplicating that reader here; Platform
# arrives transitively behind Formats (modules/formats/CMakeLists.txt's own DEPS). No RHI is behind
# this boundary, so that property holds. Collapsing the two DLLs into one would satisfy a narrower
# reading of "SHARED depends on Core only" while destroying the separation the split exists for.
add_library(Aver.Framework SHARED
  src/FrameworkAbi.cpp
  src/ActionEngine.cpp
  src/TimersEventsAbi.cpp
  src/BlackboardAbi.cpp
  src/AnimAbi.cpp
  # Model import (FBX, glTF) relayed to the editor's importers (wave 9, docs/plans/wave9/FBX_IMPORT.md).
  src/ImportAbi.cpp
  # IK goals, foot-IK weights and control-rig inputs relayed to the rig system (wave 5, docs/ANIM_IK_RETARGET.md).
  src/IkAbi.cpp

  # Blend-shape weights, face poses, face rig and lip-sync relayed to the morph system's provider (docs/ANIM_MORPH_FACE.md).
  src/MorphAbi.cpp
  # Cloth and destruction calls relayed to the soft-body system's provider (docs/SOFTBODY.md).
  src/SoftBodyAbi.cpp
  src/SequenceAbi.cpp
  src/LocalizationAbi.cpp
  # Accessibility: the option table, captions, speech and camera feel (wave 9, docs/ACCESSIBILITY.md).
  src/AccessAbi.cpp
  src/DataTableAbi.cpp
  src/EntityStateAbi.cpp
  # Terrain and water queries relayed from the host (wave 3, landscape layers and rivers).
  src/TerrainAbi.cpp)

target_include_directories(Aver.Framework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Framework PUBLIC cxx_std_20)
target_link_libraries(Aver.Framework PUBLIC Aver.Core Aver.Assets Aver.Scene Aver.Formats)

# AVER_FW_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Framework PRIVATE AVER_FW_BUILD)
# Consumers get AVER_MODULE_FRAMEWORK so they can #if around the whole feature.
target_compile_definitions(Aver.Framework PUBLIC AVER_MODULE_FRAMEWORK=1)

set_target_properties(Aver.Framework PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe
