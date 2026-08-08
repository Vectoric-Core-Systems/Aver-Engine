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
