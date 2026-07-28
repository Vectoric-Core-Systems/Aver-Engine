# Aver.Formats.Roslyn -- the escalation path for an actor script the built-in scanner declines.
#
# A SEPARATE TARGET, for the same reason Aver.Formats.Material and Aver.Formats.Audio are: the base
# module reads bytes and depends on nothing, which is what lets a test link it alone and what lets
# the built-in scanner run on a machine with no .NET at all. Process spawning belongs above that
# line, not inside it -- putting it in Aver.Formats would hand a dependency to every consumer of
# every format in the tree to serve one optional fallback.
#
# A build without this target simply has no Roslyn backend. That is the structural statement of
# "optional", and it needs no flag to express.
# GUARDED ON THE TARGET THAT OWNS ActorScript.cpp rather than on a switch of its own.
#
# The actor-script reader currently lives in Aver.Formats.Material, which is gated on
# AVER_MODULE_PBR. That is where it is, not where it belongs -- an actor's placements have nothing to
# do with materials -- but a backend for a parser has to be built beside the parser, and inventing a
# second condition here would let the two drift into a configuration where this exists and the thing
# it falls back FROM does not.
if(TARGET Aver.Formats.Material)
  aver_add_module(Aver.Formats.Roslyn
    SOURCES
      src/AverDesign.cpp
    DEPS
      Aver.Core
      Aver.Formats
      Aver.Formats.Material
  )
endif()
