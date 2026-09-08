# WIN32 -- /SUBSYSTEM:WINDOWS, so the editor never opens a console window of its own. It was a
# console-subsystem program until 0.5.0, which meant every launch from Explorer, a shortcut or the
# Aver Launcher put a black rectangle on screen beside the editor for the whole session.
#
# THE OUTPUT IS NOT LOST: EntryPoint.hpp calls attachParentConsole(), which adopts the launching
# terminal's console when there is one and leaves redirection untouched -- see Console.hpp. gates.ps1,
# test.ps1 and the MCP runner all capture stdout through pipes and are unaffected either way.
add_executable(Sandbox WIN32
  src/SandboxApp.cpp
  src/AssetEditor.cpp
  src/ActorEditor.cpp
  src/AnimEditor.cpp
  src/SkinDrawTest.cpp
  src/SkinSceneTest.cpp
  src/ReflTest.cpp
  src/ProjectBrowser.cpp
  src/RecentProjects.cpp
  src/ProjectScaffold.cpp
  src/EditorPrefs.cpp
  src/EditorNotifications.cpp
  src/InputOwnership.cpp
  src/ViewportIconRenderer.cpp
  src/EngineScaffold.cpp
  src/McpConf.cpp
  src/IdeIntegration.cpp
  src/ShellIntegration.cpp
  src/NavBakeCommand.cpp
  src/ToolsMenu.cpp
  src/GraphEditorGeometry.cpp
  src/GraphEditor.cpp
  src/BtEditor.cpp
  src/SoundEditor.cpp
  src/EditorKeybinds.cpp
  src/EditorEntitySnapshot.cpp
  src/PhysicsSceneSync.cpp
  src/ThumbnailCache.cpp
  Sandbox.rc)
target_link_libraries(Sandbox PRIVATE Aver.Runtime Aver.Formats)

# Dear ImGui: the editor's own UI. Aver.RHI.D3D12 no longer knows this exists -- see its own
# CMakeLists.txt and include/aver/rhi/d3d12/UiBackend.hpp for the seam it exposes instead -- so the
# editor, which is the thing that actually wants ImGui, links it itself: Aver.RHI.D3D12.ImGui for the
# DX12/Win32 render backend, and `imgui` a second time, directly, for the ~90 call sites across
# sandbox/src/*.cpp that build panels and docking with raw ImGui:: calls (that surface was never
# inside D3D12Device.cpp and has nothing to do with the split). Both PRIVATE: Sandbox is a leaf
# executable with nothing further to propagate to.
#
# AVER_WITH_IMGUI is defined HERE now, not inherited through the RHI -- and gated on
# Aver.RHI.D3D12.ImGui existing, NOT on AVER_ENABLE_UI alone. That distinction matters for exactly one
# configuration: AVER_RHI_D3D12=OFF with AVER_ENABLE_UI still ON (module-matrix.ps1's d3d12-off row).
# `imgui` itself builds there (third_party/imgui is gated on AVER_ENABLE_UI alone), but
# Aver.RHI.D3D12.ImGui does not (it DEPS on Aver.RHI.D3D12), and this file's own AVER_WITH_IMGUI code
# below needs that module's types. Gating on AVER_ENABLE_UI alone would define the macro in a tree
# that cannot compile SandboxApp.cpp's ImGui backend wiring; gating on the module being present
# reproduces exactly the pre-split behaviour, where AVER_WITH_IMGUI only ever reached Sandbox by
# riding along on Aver.RHI.D3D12 actually being built.
if(TARGET Aver.RHI.D3D12.ImGui)
  target_link_libraries(Sandbox PRIVATE Aver.RHI.D3D12.ImGui imgui)
  target_compile_definitions(Sandbox PRIVATE AVER_WITH_IMGUI=1)
endif()
# The Vulkan half, on the same terms and INDEPENDENTLY: a tree can have either backend, both, or
# neither, and SandboxApp.cpp picks between them at runtime off IDevice::backend(). Its own macro
# rather than AVER_WITH_IMGUI, because that one means "the D3D12 backend's types are available" --
# guarding the Vulkan wiring on it would break a Vulkan-only tree.
if(TARGET Aver.RHI.Vulkan.ImGui)
  target_link_libraries(Sandbox PRIVATE Aver.RHI.Vulkan.ImGui imgui)
  target_compile_definitions(Sandbox PRIVATE AVER_WITH_IMGUI_VULKAN=1)
endif()
# The level/world runtime. The editor and the game runtime BOTH link this, which is the whole reason
# it is not inside Aver.Runtime.Game: turning a .ocworld placement into an entity used to be written
# out twice, and every future change to it -- chunk ownership first -- would have had to be made
# twice and stay agreeing.
target_link_libraries(Sandbox PRIVATE Aver.World)
# The Roslyn escalation for an actor script the built-in scanner declines. Linked here rather
# than pulled in by Aver.Formats, which must stay dependency-free -- see its own CMakeLists.
if(TARGET Aver.Formats.Roslyn)
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Roslyn)
  # The tab compiles either way; this is what decides whether it can ESCALATE. A build without the
  # backend still opens every conforming actor, which is the point of the scanner being sufficient
  # on its own.
  target_compile_definitions(Sandbox PRIVATE AVER_HAVE_ROSLYN=1)
endif()
# The audio importer, behind the Content Browser's Import button: .wav/.mp3/.m4a/.flac -> .ocaudio.
# A separate target like the mesh and material readers, and for the same reason: it pulls in Media
# Foundation, which nothing else in the editor needs.
if(TARGET Aver.Formats.Audio)
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Audio)
  target_compile_definitions(Sandbox PRIVATE AVER_HAVE_AUDIO_IMPORT=1)
endif()
# The retained game UI and the render feature that draws it. Both unconditional: neither depends on
# an optional module, and the renderer declines at create() on a backend with no GPU support.
target_link_libraries(Sandbox PRIVATE Aver.UI Aver.Render.UI)
# ...and the C seam onto it. The editor draws its own demo through this rather than against a
# UiDrawList directly, so the seam a game's HUD uses is exercised by the editor too. Aver.UI is
# still named above because the host casts aver_ui_draw_list() back to that type.
target_link_libraries(Sandbox PRIVATE Aver.UI.Abi)
# Water. Guarded because AVER_MODULE_FLUIDS is a real option and the module matrix builds trees with
# it off; linking it is also what brings AVER_MODULE_FLUIDS=1 through to SandboxApp.cpp, which is
# what the #if guards there key on -- the same transitive-define arrangement Scene/PBR/Physics use.
if(TARGET Aver.Fluids)
  target_link_libraries(Sandbox PRIVATE Aver.Fluids)
endif()
# Soft-body drawing. Guarded for Water's reason above, and it brings AVER_MODULE_RENDER_SOFTBODY=1
# through to SandboxApp.cpp the same transitive way.
if(TARGET Aver.Render.SoftBody)
  target_link_libraries(Sandbox PRIVATE Aver.Render.SoftBody)
endif()
if(TARGET Aver.Fluids)
  target_link_libraries(Sandbox PRIVATE Aver.Fluids)
endif()

# Aver Sound and the audio C seam, for the .ocsnd editor tab (sandbox/src/SoundEditor.cpp).
#
# TWO SEPARATE `if(TARGET)` GUARDS BECAUSE THEY ANSWER DIFFERENT QUESTIONS. Aver.Sound is the
# synthesiser -- without it the tab cannot render a graph at all, so it is what SoundEditor.cpp's
# own `#include "aver/sound/Synth.hpp"` needs. Aver.Audio.Abi is the mixer seam, and only the
# PREVIEW BUTTON needs that: a tree configured without the audio seam still gets the whole editor,
# a real render and a real waveform, and only loses the ability to make a noise. That distinction is
# what AVER_WITH_AUDIO_ABI carries into the source, and it is why the tab does not simply reuse
# AVER_WITH_IMGUI to mean both things.
#
# THE MACRO IS A CAPABILITY, NOT A TAB, and it was named after the tab until 0.5.1 -- which is how
# the game runtime came to be excluded from the audio device without anyone noticing. The same
# block now exists in modules/runtime.game/CMakeLists.txt; the two must stay in step, and
# verify-game.ps1 is where that belongs to be asserted rather than remembered.
if(TARGET Aver.Sound)
  target_link_libraries(Sandbox PRIVATE Aver.Sound)
endif()
if(TARGET Aver.Audio.Abi)
  target_link_libraries(Sandbox PRIVATE Aver.Audio.Abi)
  target_compile_definitions(Sandbox PRIVATE AVER_WITH_AUDIO_ABI=1)
endif()
# The actor editor's preview. Its own mesh registry, deliberately: an asset editor that reached
# into the level editor's table could not exist or be tested without one.
target_link_libraries(Sandbox PRIVATE Aver.Render.ActorPreview)
# GPU skinning. THIS COMMENT USED TO SAY that the pass had "no other consumer yet", that "the mesh
# draw path cannot take a substituted position stream", and that "this link line is what keeps it
# compiled and checkable". ALL THREE ARE FALSE, and the correction is recorded here rather than
# quietly deleted because the sentence was being cited as a reason not to attempt work that is in
# fact already shipping.
#
# Verified at every link in the chain, twice -- once by hand and once by an adversarial sweep:
#
#   * IDevice::createSkinTargetMesh (RHI.hpp:363) mints a NEW MeshHandle over a BufferKind::Default,
#     allowUnorderedAccess buffer seeded with the source mesh's vertices, sharing the source's index
#     buffer. Implemented on BOTH backends -- D3D12Device.cpp:2648, VulkanDevice.cpp:1592.
#   * SkinningPass::dispatch writes posed vertices into that buffer every frame, then transitions it
#     UnorderedAccess -> GeometryRead.
#   * THREE production consumers, none of them behind --skin-test: SkinnedScene registered
#     unconditionally in the editor (SandboxApp.cpp:1311) and in the game runtime
#     (GameApp.cpp:490-503), and the animation editor's own preview (AnimEditor.cpp:189), which
#     calls createSkinTargetMesh directly.
#   * Both renderers substitute the handle into the ORDINARY drawMesh every other entity uses --
#     SandboxApp.cpp:4168 and GameRender.cpp:142. D3D12RenderContext::drawMesh has no idea the
#     buffer is compute-written; the substitution is a MeshHandle swap and nothing below it changes.
#
# So substitution is not merely possible, it is the shipping path for every skinned character. The
# HANDLE is substituted rather than the stream, deliberately -- see createSkinTargetMesh's own
# comment for why threading a substitute stream through submitDraw instead would have been one
# missed site away from a rest-pose shadow beside a posed character.
#
# WHY THIS LINE IS STILL HERE, since the old reason was wrong: Aver.Runtime.Game already PUBLIC-links
# Aver.Render.Skin (modules/runtime.game/CMakeLists.txt:55), so removing this would not stop the
# module compiling. It is here because the EDITOR ITSELF uses SkinningPass directly, in
# AnimEditor.cpp and SkinDrawTest.cpp -- an ordinary link dependency, not a keep-alive.
#
# WHAT IS GENUINELY UNBUILT, so the correction does not swing too far the other way: this pattern
# serves producers that emit a full interleaved rhi::MeshVertex. The two CPU producers that exist --
# deform::deformVertices and aver_phys_softbody_vertices -- emit bare xyz, and MeshVertex is
# {pos, normal, uv} at a 32-byte stride (RHI.hpp:46). Driving the draw path from either therefore
# still needs real glue: normals recomputed per frame, physics output converted from WORLD space
# back to mesh-local (drawMesh applies a per-object transform), and a frames-in-flight ring, because
# writeBuffer is immediate, unsynchronised, and refuses a non-Upload buffer outright
# (D3D12Device.cpp:5608) -- so the skin target cannot simply be written from the CPU. That work is
# a render feature of its own, mirroring SkinnedScene. It is unbuilt. It is not blocked.
target_link_libraries(Sandbox PRIVATE Aver.Render.Skin)
# The path tracer, for --pt-furnace. Linked here for the same reason skinning is: the editor is the
# only host that drives it, and this link line is what keeps its shader compiled and its energy
# oracle runnable on a real device.
target_link_libraries(Sandbox PRIVATE Aver.Render.PathTracer)
if(TARGET Aver.Render.Voxi)
  # Settings DLL (brings AVER_MODULE_VOXI=1) + the static render feature that drives the RHI.
  target_link_libraries(Sandbox PRIVATE Aver.Render.Voxi Aver.Render.Voxi.Renderer)
endif()
if(TARGET Aver.Render.PBR)
  # The material DLL (brings AVER_MODULE_PBR=1). Named explicitly rather than left to arrive
  # transitively through Voxi's renderer: the executable is what the C# layer P/Invokes into, so it
  # is the executable that must depend on the material system whether or not a render feature does.
  # ...and the GPU half, which is what turns an authored material into a binding set and a b2 block.
  # Both are named here for the same reason Voxi's two are: the executable is what the C# layer
  # P/Invokes, so it must depend on the material system whether or not a render feature does.
  target_link_libraries(Sandbox PRIVATE Aver.Render.PBR Aver.Render.PBR.Materials)
  # The .ocmat reader and the decode-to-GPU step. Both are only meaningful with a material system,
  # so they hang off the same guard rather than being linked unconditionally: with PBR off the
  # editor has no material to load a file into and nothing to upload a texture for.
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Material Aver.Assets.Gpu)
endif()
if(TARGET Aver.Scene)
  # The world DLL (brings AVER_MODULE_SCENE=1). Named explicitly for the reason the material system
  # is: the executable is what the C# layer P/Invokes into, so it must depend on the world whether
  # or not anything else in the link already does.
  target_link_libraries(Sandbox PRIVATE Aver.Scene)
endif()
if(TARGET Aver.Anim.Scene)
  # The scene/animation join. Named here because the app owns the frame loop, and the frame loop is
  # what decides when the animation clock advances.
  target_link_libraries(Sandbox PRIVATE Aver.Anim.Scene)
endif()
if(TARGET Aver.Physics)
  # The simulation (brings AVER_MODULE_PHYSICS=1). Named on the executable for the same reason the
  # scene and material systems are: the app owns the frame loop, and the frame loop is what decides
  # when the world is stepped.
  target_link_libraries(Sandbox PRIVATE Aver.Physics)
endif()
if(TARGET Aver.Synapse)
  # Navigation. The editor is where a bake can happen at all: terrain collision is editor-only, so
  # this process is the only one that can see the ground it would sample. Aver.Synapse itself knows
  # nothing about physics or a scene -- src/NavBakeCommand.cpp is the piece that does.
  target_link_libraries(Sandbox PRIVATE Aver.Synapse)
endif()
if(TARGET Aver.Synapse.Scene)
  # CSynapseAgent + AgentSystem (brings AVER_MODULE_SYNAPSE_SCENE=1) -- the join between the grid
  # above and a live scene::World, so the editor can register the component and tick agents in Play.
  target_link_libraries(Sandbox PRIVATE Aver.Synapse.Scene)
endif()
if(TARGET Aver.Framework)
  # Gameplay (brings AVER_MODULE_FRAMEWORK=1). It carries Aver.Scene transitively, but the scene is
  # still named above on its own: the framework is optional and the world is not meant to become
  # unreachable the day someone turns it off.
  target_link_libraries(Sandbox PRIVATE Aver.Framework)
endif()
if(TARGET Aver.Scripting.Host)
  # The in-process CLR host (brings AVER_MODULE_SCRIPTING=1). The executable owns the instance and
  # ticks it, the same way it owns the Voxi feature -- a subsystem the app configures is not
  # something the composition root should be reaching into.
  target_link_libraries(Sandbox PRIVATE Aver.Scripting.Host)
endif()
target_include_directories(Sandbox PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
set_target_properties(Sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ship the splash next to the executable so the engine can show it at startup.
#
# Driven by an OUTPUT rule rather than POST_BUILD: a POST_BUILD command only runs when the target
# actually relinks, so editing the artwork alone left the old splash shipped next to the exe with a
# clean build reporting success.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/splash.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/splash.png ${CMAKE_BINARY_DIR}/bin/splash.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/splash.png
  COMMENT "Staging splash.png"
  VERBATIM)
add_custom_target(Sandbox.Splash DEPENDS ${CMAKE_BINARY_DIR}/bin/splash.png)
add_dependencies(Sandbox Sandbox.Splash)
set_target_properties(Sandbox.Splash PROPERTIES FOLDER "sandbox")

# Ship the project browser's mark next to the executable. Same OUTPUT/DEPENDS rule as the splash
# above, for the same reason.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/logo.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/logo.png ${CMAKE_BINARY_DIR}/bin/logo.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/logo.png
  COMMENT "Staging logo.png"
  VERBATIM)
add_custom_target(Sandbox.Logo DEPENDS ${CMAKE_BINARY_DIR}/bin/logo.png)
add_dependencies(Sandbox Sandbox.Logo)
set_target_properties(Sandbox.Logo PROPERTIES FOLDER "sandbox")

# Ship the Player Start's viewport marker. Same OUTPUT/DEPENDS rule as the splash and logo above.
# Unlike those two this one is drawn into the 3D SCENE rather than blitted into an ImGui panel --
# see EditorIconRenderer -- but it is staged identically because it is loaded the identical way, by
# path, next to the executable.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/player-start-icon.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/player-start-icon.png ${CMAKE_BINARY_DIR}/bin/player-start-icon.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/player-start-icon.png
  COMMENT "Staging player-start-icon.png"
  VERBATIM)
add_custom_target(Sandbox.PlayerStartIcon DEPENDS ${CMAKE_BINARY_DIR}/bin/player-start-icon.png)
add_dependencies(Sandbox Sandbox.PlayerStartIcon)
set_target_properties(Sandbox.PlayerStartIcon PROPERTIES FOLDER "sandbox")

# Ship the Compile C# status sprite sheet (three tiles: built / failed / stale). Same OUTPUT/DEPENDS
# rule as the splash and logo above, so editing the artwork alone re-stages it without a relink.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/compile-status.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/compile-status.png ${CMAKE_BINARY_DIR}/bin/compile-status.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/compile-status.png
  COMMENT "Staging compile-status.png"
  VERBATIM)
add_custom_target(Sandbox.CompileIcon DEPENDS ${CMAKE_BINARY_DIR}/bin/compile-status.png)
add_dependencies(Sandbox Sandbox.CompileIcon)
set_target_properties(Sandbox.CompileIcon PROPERTIES FOLDER "sandbox")

# Ship the Content Browser file-type icon sheet (four tiles: C# Script / C# Class / C++ Class / C++ Module).
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/file-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/file-icons.png ${CMAKE_BINARY_DIR}/bin/file-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/file-icons.png
  COMMENT "Staging file-icons.png"
  VERBATIM)
add_custom_target(Sandbox.FileIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/file-icons.png)
add_dependencies(Sandbox Sandbox.FileIcons)
set_target_properties(Sandbox.FileIcons PROPERTIES FOLDER "sandbox")

# Ship the Content Browser folder icon sheet (two tiles: plain folder / engine-or-module folder).
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/folder-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/folder-icons.png ${CMAKE_BINARY_DIR}/bin/folder-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/folder-icons.png
  COMMENT "Staging folder-icons.png"
  VERBATIM)
add_custom_target(Sandbox.FolderIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/folder-icons.png)
add_dependencies(Sandbox Sandbox.FolderIcons)
set_target_properties(Sandbox.FolderIcons PROPERTIES FOLDER "sandbox")

# The generated asset-type sheet (anim / skeleton / mesh). Staged the same way and for the same
# reason as the others: POST_BUILD would not re-copy a sheet that changed without the exe relinking.
# Drawn by scripts/make-asset-icons.py, NOT hand-authored -- see branding/ASSETS.md.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/asset-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/asset-icons.png ${CMAKE_BINARY_DIR}/bin/asset-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/asset-icons.png
  COMMENT "Staging asset-icons.png"
)
add_custom_target(Sandbox.AssetIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/asset-icons.png)
add_dependencies(Sandbox Sandbox.AssetIcons)
set_target_properties(Sandbox.AssetIcons PROPERTIES FOLDER "sandbox")

# Ship the editor UI typeface next to the executable. Same OUTPUT/DEPENDS rule as the splash, and
# for the same reason -- POST_BUILD would not re-copy a font that changed without the exe relinking.
# MaterialIcons-Regular.ttf rides the same rule: it is MERGED into the body font's atlas at load
# (see applyDpi), so a missing file degrades to text with notdef boxes where icons should be rather
# than failing loudly -- which is why it has to be staged as reliably as Roboto itself.
set(AVER_UI_FONTS Roboto-Regular.ttf Roboto-Medium.ttf MaterialIcons-Regular.ttf)
set(AVER_UI_FONT_OUTPUTS)
foreach(font ${AVER_UI_FONTS})
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/${font}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/fonts/${font} ${CMAKE_BINARY_DIR}/bin/${font}
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/${font}
    COMMENT "Staging ${font}"
    VERBATIM)
  list(APPEND AVER_UI_FONT_OUTPUTS ${CMAKE_BINARY_DIR}/bin/${font})
endforeach()
add_custom_target(Sandbox.Fonts DEPENDS ${AVER_UI_FONT_OUTPUTS})
add_dependencies(Sandbox Sandbox.Fonts)
set_target_properties(Sandbox.Fonts PROPERTIES FOLDER "sandbox")

# The resource script embeds the icon, but nothing tells the generator that it reads the .ico -- so
# replacing the icon would not re-run the resource compiler and the exe would keep the old one.
set_property(SOURCE Sandbox.rc APPEND PROPERTY
  OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/branding/icon.ico)

# The editor control channel. OPTIONAL in the strongest sense: without it the editor has no idea it
# ever existed, which is what `if(TARGET ...)` here and AVER_MODULE_MCP in the module both enforce.
if(TARGET Aver.Mcp)
  target_link_libraries(Sandbox PRIVATE Aver.Mcp)
endif()

# Virtualized-geometry cluster selection (ClusterAdapt/ClusterSelect): the per-frame LOD-level chooser
# that turns a .ocmesh's coarserLods into an actual draw-time decision. Optional and named explicitly
# for the same reason Scene/PBR/Physics are above: it brings its own AVER_MODULE_TRIFACTOR=1 define
# through its link interface (modules/trifactor/CMakeLists.txt PUBLIC_DEFS), which is what lets
# SandboxApp.cpp guard the feature with `#if AVER_MODULE_TRIFACTOR` and have it read as a real zero,
# not an accidental always-off, in a build with the module disabled.
if(TARGET Aver.Upgrade)
  target_link_libraries(Sandbox PRIVATE Aver.Upgrade)
endif()
if(TARGET Aver.Trifactor)
  target_link_libraries(Sandbox PRIVATE Aver.Trifactor)
endif()

# The landscape runtime: quadtree LOD selection, chunk mesh generation and the GPU draw loop
# (modules/landscape). Before this, nothing outside tests/landscape linked either target -- the
# renderer was a complete, tested subsystem the editor could not host. Linking only the .Renderer
# target is sufficient: it PUBLIC-links Aver.Landscape (modules/landscape/CMakeLists.txt), which is
# what brings AVER_MODULE_LANDSCAPE=1 through the link interface and lets SandboxApp.cpp guard the
# feature with `#if AVER_MODULE_LANDSCAPE`, same shape as Aver.Render.Voxi.Renderer above.
if(TARGET Aver.Landscape.Renderer)
  target_link_libraries(Sandbox PRIVATE Aver.Landscape.Renderer)
endif()

# Occlusion (modules/occlusion): brings AVER_MODULE_OCCLUSION=1 through its link interface, which is
# what lets SandboxApp.cpp guard --occlusion-cull and the culler it drives with `#if
# AVER_MODULE_OCCLUSION`, same shape as every other optional module linked on this page.
if(TARGET Aver.Occlusion)
  target_link_libraries(Sandbox PRIVATE Aver.Occlusion)
endif()

# AverSR (docs/AVERSR.md): the built-in upscaler implementations. Optional and named explicitly for
# the same reason every module above is: it brings AVER_MODULE_SR=1 through its link interface
# (modules/render.sr/CMakeLists.txt PUBLIC_DEFS), which is what lets SandboxApp.cpp guard the
# --aversr flag and the render-settings quality combo with `#if AVER_MODULE_SR` and have it read as
# a real zero, not an accidental always-off, in a build with the module disabled. Sandbox is the
# COMPOSITION ROOT here, not the renderer -- it is the concrete `aver::sr::SpatialUpscaler` type
# that only this link line can see, exactly as docs/AVERSR.md says a "host holds an IUpscaler* that
# may be null"; Aver.RHI itself still never links this module and never will.
if(TARGET Aver.Render.Sr)
  target_link_libraries(Sandbox PRIVATE Aver.Render.Sr)
endif()

# Particles (modules/particles): brings AVER_MODULE_PARTICLES=1 through its link interface, which is
# what lets SandboxApp.cpp guard the feature and --particle-test with `#if AVER_MODULE_PARTICLES`,
# same shape as every other optional module linked on this page. Aver.Scene is already linked above
# when this target exists at all (Aver.Particles PUBLIC-links it), named again there for the reason
# every other optional module's own dependency is: the executable is what the C# layer P/Invokes.
if(TARGET Aver.Particles)
  target_link_libraries(Sandbox PRIVATE Aver.Particles)
endif()
# .ocparticle reader/writer (DECIDED 3, slice 3): only meaningful once Aver.Particles exists (it
# parses straight into particles::ParticleEffect), so it hangs off the SAME guard rather than its own
# AVER_MODULE_PARTICLES check -- exactly Aver.Formats.Material's relationship to AVER_MODULE_PBR two
# blocks up. Lets SandboxApp.cpp's loadProjectParticleEffects() (slice 5) resolve a real .ocparticle
# on disk instead of only ever seeing the --particle-test harness's hardcoded structs.
if(TARGET Aver.Formats.Particles)
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Particles)
endif()

# Save/load of a whole world. The editor is a composition root too, so it installs the same
# provider the game runtime does -- and today it is the only place a project can test its
# save path at all.
if(TARGET Aver.Save)
  target_link_libraries(Sandbox PRIVATE Aver.Save)
endif()

# /bigobj on SandboxApp.cpp specifically.
#
# NOT a tuning flag: without it MSVC fails outright with C1128, "number of sections exceeded object
# file format limit". The COFF object format caps a translation unit at 65,536 sections, and this one
# file -- the whole editor, every panel, every tool, in one TU -- crossed it. Scoped to the single
# source that needs it rather than the whole target, so the limit being hit again elsewhere stays a
# visible event rather than something this line quietly absorbs.
#
# The real fix is splitting SandboxApp.cpp up, which is a much larger change than whatever edit
# happens to be the one that tips it over.
if(MSVC)
  set_source_files_properties(src/SandboxApp.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
  # WIN32 above sets /SUBSYSTEM:WINDOWS, which makes the linker look for WinMain. The entry point
  # this engine generates is main(), so point the CRT's startup at the one that calls it. Identical
  # to game/CMakeLists.txt, whose comment claimed Sandbox already did this long before it did.
  target_link_options(Sandbox PRIVATE /ENTRY:mainCRTStartup)
endif()

# The editor's own two shaders (the cluster PS and the G-buffer debug view). The same stamp
# mechanism the modules use -- aver_deploy_shaders takes any target, executables included.
aver_deploy_shaders(Sandbox "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# Ship the game UI's baked font next to the executable. Same OUTPUT/DEPENDS rule as the splash and
# logo above, for the same reason: a POST_BUILD would leave a re-baked font unstaged after a build
# that did not relink.
#
# TWO FILES, ONE TARGET: the .ocfont is the glyph table and the .png is the atlas it indexes, and a
# half-staged pair is a font that parses and draws nothing. Baked by scripts/bake-font.py -- see its
# header for why the game UI bakes offline rather than rasterising at runtime.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.ocfont ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/assets/fonts/Roboto-Regular.ocfont ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.ocfont
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/assets/fonts/Roboto-Regular.png ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.png
  DEPENDS ${CMAKE_SOURCE_DIR}/assets/fonts/Roboto-Regular.ocfont
          ${CMAKE_SOURCE_DIR}/assets/fonts/Roboto-Regular.png
  COMMENT "Staging the game-UI font"
  VERBATIM)
add_custom_target(Sandbox.UiFont DEPENDS
  ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.ocfont ${CMAKE_BINARY_DIR}/bin/Roboto-Regular.png)
add_dependencies(Sandbox Sandbox.UiFont)
set_target_properties(Sandbox.UiFont PROPERTIES FOLDER "sandbox")
