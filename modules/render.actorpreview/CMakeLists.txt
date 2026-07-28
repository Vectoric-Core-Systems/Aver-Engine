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
    Aver.Formats.Material
    Aver.Formats
    Aver.Core
)
