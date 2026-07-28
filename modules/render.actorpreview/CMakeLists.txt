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
  DEPS
    Aver.RHI
    Aver.Core
)
