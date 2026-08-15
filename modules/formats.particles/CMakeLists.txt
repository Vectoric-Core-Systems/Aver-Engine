# Aver.Formats.Particles -- the .ocparticle reader/writer (particles DECIDED 3, slice 3).
#
# A SEPARATE TARGET, for Aver.Formats.Material's exact reason (see modules/formats/CMakeLists.txt's
# own comment on that target): it is the only format here besides .ocmat that needs a type from
# outside Aver.Formats -- it parses straight into particles::ParticleEffect rather than a duplicate
# struct -- so it must link Aver.Particles, and Aver.Formats itself has to stay dependency-free.
#
# A SEPARATE DIRECTORY, rather than a fourth target defined inside modules/formats/CMakeLists.txt
# the way Aver.Formats.Material is: that file is add_subdirectory'd from the root CMakeLists.txt at
# line ~190, BEFORE modules/particles exists (~line 307). Guarding a target defined THERE on
# AVER_MODULE_PARTICLES would read that option's value before the AVER_MODULE_SCENE force-off
# cascade (root CMakeLists.txt, right above the modules/particles add_subdirectory) has run, so an
# AVER_MODULE_SCENE=OFF configuration -- which the module matrix already exercises as "scene-off" --
# would still see AVER_MODULE_PARTICLES=ON at that point and add a target with a forward link to
# Aver.Particles, which the cascade goes on to make sure never gets defined. That is exactly the
# ordering trap modules/formats.roslyn's own CMakeLists.txt already documents for the one dependency
# hop over (Aver.Formats.Roslyn on Aver.Formats.Material) -- same shape, same fix.
#
# GUARDED ON THE TARGET, not on the option: this directory is add_subdirectory'd from the root
# CMakeLists.txt right after modules/particles, so by the time this file runs, the cascade above has
# already settled whether Aver.Particles exists. `if(TARGET Aver.Particles)` asks that question
# correctly regardless of *how* AVER_MODULE_PARTICLES ended up OFF (its own default, or the scene
# cascade), where re-deriving the boolean condition here would have to stay in sync with that cascade
# by hand and silently drift the next time the cascade grows another dependency.
if(TARGET Aver.Particles)
  aver_add_module(Aver.Formats.Particles
    SOURCES
      src/OcParticle.cpp
    DEPS
      Aver.Formats
      Aver.Particles
  )
endif()
