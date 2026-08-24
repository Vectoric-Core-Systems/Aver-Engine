# Aver.Render.ActorPreview — an actor's authored composition, drawn into a texture of its own.
#
# A FEATURE, not a second viewport. It owns a colour target, a depth target, one pipeline and one
# camera published at the register a feature is given; nothing about the scene's frame changes. The
# reasoning, including why the obvious alternative silently draws with the previous frame's camera,
# is docs/ACTOR_EDITOR.md section 3.
#
# It links the generic RHI and never a backend, so a recording device can drive it with no GPU --
# the same property that made Aver.Render.UI testable.
aver_add_module(Aver.Render.ActorPreview
  SOURCES
    src/ActorPreview.cpp
    src/PreviewMeshCache.cpp
  DEPS
    Aver.RHI
    # The .ocmesh reader, for the preview's OWN mesh registry. Aver.Formats knows nothing of the
    # RHI, so naming it here creates no cycle -- and the alternative was reaching into the level
    # editor's private table, which is exactly the seam this module must not cross.
    #
    # Aver.Formats.Material WAS ALSO NAMED HERE, unconditionally, and it only exists when
    # AVER_MODULE_PBR is on. This module is added in every configuration ("it exists in every
    # configuration", root CMakeLists.txt), so every PBR=OFF build named a target that did not
    # exist. CMake does not object to that -- it hands the name to the linker, which reports
    # LNK1104 in some other target entirely. aver_add_module now refuses it at configure time.
    #
    # It was never needed: the only thing this module used from it was ActorScript, which has now
    # moved into the base Aver.Formats where it always belonged.
    Aver.Formats
    Aver.Core
)

# The MATERIAL path: ActorPreview.cpp's second pipeline shades through averEvalMaterial so the
# graph editor can preview a .ocgraph material on the sphere it edits. CONDITIONAL for the exact
# reason the comment above named Aver.Formats.Material's own removal: Aver.Render.PBR.Materials
# exists only when AVER_MODULE_PBR is ON, and this module is added in EVERY configuration -- naming
# it unconditionally would put the same LNK1104 back, one target sooner. `if(TARGET ...)`, never
# `if(AVER_MODULE_PBR)`: this is the form aver_check_module_dag() (cmake/AvModule.cmake) can prove
# resolves in whatever configuration actually runs, rather than trusting the variable and the target
# to agree.
#
# Every `#if AVER_MODULE_PBR` block in ActorPreview.cpp/.hpp is dead code, compiled to nothing, in a
# tree where this does not run -- AVER_MODULE_PBR only ever reaches that file as the PUBLIC compile
# definition Aver.Render.PBR.Materials carries transitively from Aver.Render.PBR (see that module's
# own CMakeLists.txt), and PUBLIC here is what lets ActorPreviewTest.cpp see the same definition
# without tests/render.actorpreview/CMakeLists.txt naming a single new library itself.
if(TARGET Aver.Render.PBR.Materials)
  target_link_libraries(Aver.Render.ActorPreview PUBLIC Aver.Render.PBR.Materials)
endif()
