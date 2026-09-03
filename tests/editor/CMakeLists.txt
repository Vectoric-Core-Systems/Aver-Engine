# EditorPrefsTest -- the editor's own preferences store.
#
# It COMPILES the sandbox translation unit directly rather than linking a library, because the store
# lives in the editor and has no business being reachable from the engine. Promoting it to a module
# purely to make it testable would put editor UI state into the engine's dependency graph, which is a
# worse trade than this one unusual line.
add_executable(EditorPrefsTest
  src/EditorPrefsTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/EditorPrefs.cpp)
target_link_libraries(EditorPrefsTest PRIVATE Aver.Platform Aver.Core)
set_target_properties(EditorPrefsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# EditorEulerTest -- the Euler <-> quaternion pair the editor reads and writes every transform
# through. Header-only, so this needs no sandbox .cpp; it exists because those two functions used to
# be static inside SandboxApp.cpp where nothing could reach them, and one of them was wrong in a way
# that wrote corrupted rotations into saved levels.
#
# The pair itself now lives in modules/world (it is the .ocworld rotation contract, not an editor
# convention -- the game runtime had been carrying its own copy to read levels the editor wrote), and
# sandbox/src/EditorEuler.hpp re-exports it. This test is UNCHANGED and still links no module: the
# world header is header-only and Core-only, so an include directory is all it needs. That is
# precisely the property that let it move without making the trade this file argues against above.
add_executable(EditorEulerTest src/EditorEulerTest.cpp)
target_include_directories(EditorEulerTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src
  ${CMAKE_SOURCE_DIR}/modules/world/include)
target_link_libraries(EditorEulerTest PRIVATE Aver.Core Aver.Platform)
set_target_properties(EditorEulerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GraphEditorGeometryTest -- the .ocgraph node editor's headless core: the node descriptor table
# (sandbox/src/GraphNodeDefs.hpp), and layout/hit-testing/link-rules/cycle-detection/canvas-transform
# (sandbox/src/GraphEditorGeometry.hpp/.cpp). No ImGui, no Engine, no scene:: -- an editor that only
# edits a text file needs none of those, and keeping it that way is what lets this run headless.
#
# COMPILES sandbox/src/GraphEditorGeometry.cpp directly into the test binary rather than linking a
# library, for EditorPrefsTest's exact reason above: Sandbox is add_executable-only (no library half),
# and promoting editor geometry into a real module purely to make it testable would put editor UI code
# into the engine's dependency graph, which is a worse trade than this one unusual line.
# GraphNodeDefs.hpp is header-only (see its own header comment) and needs no extra source file.
add_executable(GraphEditorGeometryTest
  src/GraphEditorGeometryTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditorGeometry.cpp)
target_include_directories(GraphEditorGeometryTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
# The repo root, so testNodeCatalogCoversTheCompiler can READ OcGraphParser.cs and compare the two
# vocabularies. GraphNodeDefs.hpp is a deliberate hand-maintained copy of the C# side's node set
# (see its header), which means nothing links the two and an omission is invisible -- Select,
# InputKey and Raycast sat in the compiler and not in the palette for two whole slices because of
# exactly that. Reading the .cs as TEXT is ugly, and it is still the only thing that can notice.
target_compile_definitions(GraphEditorGeometryTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(GraphEditorGeometryTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(GraphEditorGeometryTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GraphEditorLoadSaveTest -- the .ocgraph editor TAB's public surface: construct from a path, save().
# Same EditorPrefsTest-precedent reasoning as GraphEditorGeometryTest above (Sandbox is
# add_executable-only), so this compiles sandbox/src/GraphEditor.cpp and GraphEditorGeometry.cpp
# directly into the test binary rather than linking a library.
#
# AVER_WITH_IMGUI is deliberately left UNDEFINED for this target: GraphEditor.cpp's `#include
# "imgui.h"` and its entire draw() method are behind `#if AVER_WITH_IMGUI` (see that file), so this
# target never needs ImGui headers or an ImGui context, and genuinely exercises load/save/undo/
# selection headlessly -- the part of the editor that is NOT gated behind a window.
#
# AVER_OCGRAPH_FIXTURE reuses the exact fixture path/definition tests/formats/CMakeLists.txt already
# established for OcGraphTest's own cross-impl check, so there is one definition of "where that file
# lives," not two that could drift apart.
add_executable(GraphEditorLoadSaveTest
  src/GraphEditorLoadSaveTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditor.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/GraphEditorGeometry.cpp)
target_include_directories(GraphEditorLoadSaveTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
target_compile_definitions(GraphEditorLoadSaveTest PRIVATE
  AVER_OCGRAPH_FIXTURE="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/cross_impl_test.ocgraph"
  # attribute_edit_test.ocgraph -- the Gap B (node-attribute authoring) round-trip fixture. This test
  # edits a COPY of it (never the checked-in original -- see testAttributeEditRoundTrip's own comment)
  # and writes the result back into the SAME C# test project directory under a different, generated
  # filename, so scripting/csharp/Aver.Graph.Tests's own test suite can load and COMPILE exactly what
  # this C++ editor actually wrote, not a hand-eyeballed copy of it. One definition of "where the
  # source fixture lives," matching AVER_OCGRAPH_FIXTURE's own precedent just above.
  AVER_OCGRAPH_ATTR_FIXTURE="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/attribute_edit_test.ocgraph"
  AVER_OCGRAPH_ATTR_FIXTURE_OUTPUT="${CMAKE_SOURCE_DIR}/scripting/csharp/Aver.Graph.Tests/attribute_edit_test.output.ocgraph")
# Aver.World is here for its HEADERS ONLY: LevelClassSave.hpp (the class-placement save path this
# suite covers) includes aver/world/LevelTransform.hpp for eulerDegFromQuat, so that the level save
# and every other level path agree on what "yaw 90" means rather than each rolling its own. The
# functions there are `inline`, so this adds an include directory and no object code.
target_link_libraries(GraphEditorLoadSaveTest PRIVATE Aver.Formats Aver.World Aver.Core Aver.Platform)
set_target_properties(GraphEditorLoadSaveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# BtEditorTest -- the .ocbt behaviour-tree editor tab's headless core: the four structural edits
# (add/delete/reparent/reorder) and the tab's own load/save/dirty/undo bookkeeping.
#
# Same GraphEditorLoadSaveTest arrangement immediately above, for the same reasons: Sandbox is
# add_executable-only so this compiles sandbox/src/BtEditor.cpp directly, and AVER_WITH_IMGUI is
# deliberately left UNDEFINED because BtEditor.cpp's `#include "imgui.h"` and its whole drawing half
# sit behind it. What is left is exactly the part worth testing -- which is why the structural edits
# are free functions rather than members (see BtEditor.hpp's own header comment).
add_executable(BtEditorTest
  src/BtEditorTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/BtEditor.cpp)
target_include_directories(BtEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(BtEditorTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(BtEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SoundEditorTest -- the .ocsnd sound editor tab's headless core: the structural edits, the
# topological re-sort that keeps "sources before consumers" true by construction, the tab's own
# load / save / dirty / undo bookkeeping, and renderPreview().
#
# Same BtEditorTest arrangement immediately above (Sandbox is add_executable-only, so this compiles
# sandbox/src/SoundEditor.cpp directly), and AVER_WITH_IMGUI is likewise left undefined.
#
# AVER_SOUND_EDITOR_AUDIO IS ALSO LEFT UNDEFINED, and that is a SECOND thing this target proves
# rather than merely a convenience: sandbox/CMakeLists.txt defines it only when Aver.Audio.Abi is
# linked, so a build configured without the audio seam has to keep compiling the whole tab. This is
# the only target in the tree that actually builds that configuration -- without it, "the editor
# still builds without audio" would be a claim nothing checks.
#
# GUARDED ON Aver.Sound because renderPreview() calls into the synthesiser, and -DAVER_MODULE_SOUND
# is a configuration the module matrix is entitled to turn off.
if(TARGET Aver.Sound)
  add_executable(SoundEditorTest
    src/SoundEditorTest.cpp
    ${CMAKE_SOURCE_DIR}/sandbox/src/SoundEditor.cpp)
  target_include_directories(SoundEditorTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
  target_link_libraries(SoundEditorTest PRIVATE Aver.Sound Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(SoundEditorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ProjectCopyTest -- copyProjectTree, the "Upgrade a Copy" branch of the upgrade prompt.
#
# Compiles sandbox/src/ProjectScaffold.cpp directly, for EditorPrefsTest's precedent above (Sandbox
# is add_executable-only). This is why that function was lifted OUT of ProjectBrowser: a private
# member of an ImGui screen cannot be reached from a test, and this is the one path in the editor
# that duplicates somebody's project while sitting as the DEFAULT button on a modal whose other
# option is destructive. It is the branch that most needs proving, so it had to be reachable.
add_executable(ProjectCopyTest
  src/ProjectCopyTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/ProjectScaffold.cpp)
target_include_directories(ProjectCopyTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(ProjectCopyTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(ProjectCopyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# TemplateScaffoldTest -- listTemplatesIn()/listTemplates()/scaffoldProjectFromTemplate(), the New
# Project template picker's machinery. Same ProjectCopyTest precedent immediately above (Sandbox is
# add_executable-only, so this compiles ProjectScaffold.cpp directly rather than linking a library);
# no ImGui, no Engine, no scene:: needed, because template discovery and scaffolding are pure
# filesystem, same as scaffoldProject() itself.
#
# AVER_REPO_ROOT reuses GraphEditorGeometryTest's own precedent above: this test's own executable
# lands in the same build\bin as Sandbox.exe, so listTemplates()'s real executableDir()-walk already
# finds the real templates\ directory the identical way a shipped editor would -- AVER_REPO_ROOT lets
# the test assert that independently, by reading the same directory a second way.
# ProjectMigrateTest -- migrateProject, the VERSION half of the upgrade prompt.
#
# Guarded on the target for UpgradeTest's reason in tests/formats: -DAVER_MODULE_UPGRADE=OFF is a
# configuration the module matrix checks, and it has to keep configuring even though this test
# cannot exist there. Linking Aver.Upgrade is also what defines AVER_MODULE_UPGRADE=1 for
# ProjectScaffold.cpp (the module carries it as a PUBLIC_DEF), so without it migrateProject would
# compile to its no-chain stub and the test would be asserting on the stub.
if(TARGET Aver.Upgrade)
  add_executable(ProjectMigrateTest
    src/ProjectMigrateTest.cpp
    ${CMAKE_SOURCE_DIR}/sandbox/src/ProjectScaffold.cpp)
  target_include_directories(ProjectMigrateTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
  target_link_libraries(ProjectMigrateTest PRIVATE
    Aver.Upgrade Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(ProjectMigrateTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_executable(TemplateScaffoldTest
  src/TemplateScaffoldTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/ProjectScaffold.cpp)
target_include_directories(TemplateScaffoldTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
target_compile_definitions(TemplateScaffoldTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(TemplateScaffoldTest PRIVATE Aver.Formats Aver.Core Aver.Platform)
set_target_properties(TemplateScaffoldTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# EditorEntitySnapshotTest -- EditorEntitySnapshot.hpp/.cpp: capture a live entity's components,
# destroy it, rebuild it from the snapshot, and check the rebuild is byte-exact where it should be
# and deliberately different where it should be (the transform, which is supplied at instantiate
# time; the object id, when restoreObjectId=false). This is the lower layer describeEntity()/
# recreateFrom()/copySelection()/pasteClipboard()/duplicateSelection() in SandboxApp.cpp all build
# on; --undo-test (Sandbox's own CLI flag, see runUndoTest()'s comment) proves those six commands
# themselves work end to end through the real undo stack. Needs Aver.Scene for real (a live
# scene::World, not a fake), unlike EditorPrefsTest/GraphEditorGeometryTest's Sandbox .cpp files,
# which are why this is its own target rather than folded into one of those.
if(TARGET Aver.Scene)
  add_executable(EditorEntitySnapshotTest
    src/EditorEntitySnapshotTest.cpp
    ${CMAKE_SOURCE_DIR}/sandbox/src/EditorEntitySnapshot.cpp)
  target_include_directories(EditorEntitySnapshotTest PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
  # AVER_MODULE_SCENE=1 arrives PUBLIC through Aver.Scene itself (modules/scene/CMakeLists.txt) --
  # no need to define it a second time here.
  target_link_libraries(EditorEntitySnapshotTest PRIVATE Aver.Scene Aver.Core Aver.Platform)
  set_target_properties(EditorEntitySnapshotTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# DumpClusterPs -- prints the composed HLSL (and -D list) for the cluster path's pixel shader, so it
# can be compiled with dxc offline. A shader here is a C++ string built at runtime from two preludes
# plus a feature's own source, and the only thing that ever type-checks it is createShader inside a
# live device -- so without this, "the C++ compiles" is all anyone can say before shipping it.
if(TARGET Aver.Render.PBR)
  add_executable(DumpClusterPs ${CMAKE_SOURCE_DIR}/tools/DumpClusterPs.cpp)
  target_include_directories(DumpClusterPs PRIVATE ${CMAKE_SOURCE_DIR}/sandbox/src)
  # PbrShaders.cpp lives in Aver.Render.PBR.Materials, not Aver.Render.PBR.
  target_link_libraries(DumpClusterPs PRIVATE Aver.RHI Aver.Render.PBR.Materials Aver.Render.PBR Aver.Core)
  # STAGE 3: DumpClusterPs also prints the Voxi-merged composition -- see its own header comment --
  # when this tree has Voxi to link against. Guarded the same way the whole executable already is,
  # for the same reason: a build without the module has nothing for it to demonstrate. No explicit
  # AVER_MODULE_VOXI=1 needed here: Aver.Render.Voxi.Renderer PUBLIC-links Aver.Render.Voxi, whose
  # own PUBLIC compile definition already brings it (same transitive path sandbox/CMakeLists.txt's
  # own comment on Sandbox's link line describes).
  if(TARGET Aver.Render.Voxi.Renderer)
    target_link_libraries(DumpClusterPs PRIVATE Aver.Render.Voxi.Renderer)
  endif()
  set_target_properties(DumpClusterPs PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# InputOwnershipTest -- the editor's input arbitration table.
#
# Same EditorPrefsTest precedent as above (Sandbox is add_executable-only, so this compiles the sandbox
# translation units directly rather than linking a library half that does not exist). It needs
# It links NOTHING from the keybind system: editor::Scope moved into InputOwnership.hpp precisely so
# this target would not have to drag in EditorKeybinds.hpp, which is whole-file `#if AVER_WITH_IMGUI`.
#
# NO ImGui, deliberately and by construction: resolveInputOwnership takes a struct of plain bools and
# returns one, precisely so the decision that used to be spread across ~11 ImGui-reading call sites in
# SandboxApp.cpp could be tested without a window, a device or an ImGui context. If this target ever
# needs AVER_WITH_IMGUI, the function has stopped being pure and the extraction has been undone.
add_executable(InputOwnershipTest
  src/InputOwnershipTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/InputOwnership.cpp)
target_include_directories(InputOwnershipTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(InputOwnershipTest PRIVATE Aver.Core)
set_target_properties(InputOwnershipTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# RecentProjectsTest -- the start screen's list model: recent order, cards, and the selection.
#
# Same EditorPrefsTest precedent as the targets above (Sandbox is add_executable-only, so this
# compiles the sandbox translation unit directly rather than linking a library half that does not
# exist), and the same reason ProjectCopyTest gives for the extraction itself: a private member of
# an ImGui screen cannot be reached from a test, and the browser's two parallel containers were
# where its worst bug lived -- a selection that indexed one of them while the Open button indexed
# the other.
#
# NO ImGui and NO filesystem, by construction. RecentProjects::parse() takes its own keep-predicate
# and rebuild() takes the results of the caller's directory walk, so the ordering, identity and
# selection logic is reachable without a window or a disk. If this target ever needs
# AVER_WITH_IMGUI or Aver.Platform, the model has stopped being pure and the extraction has been
# undone -- the same tripwire InputOwnershipTest sets for itself.
add_executable(RecentProjectsTest
  src/RecentProjectsTest.cpp
  ${CMAKE_SOURCE_DIR}/sandbox/src/RecentProjects.cpp)
target_include_directories(RecentProjectsTest PRIVATE
  ${CMAKE_SOURCE_DIR}/sandbox/src)
target_link_libraries(RecentProjectsTest PRIVATE Aver.Core)
set_target_properties(RecentProjectsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
