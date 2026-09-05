aver_add_module(Aver.Formats
  SOURCES
    src/Avr1.cpp
    # The GI derived-data cache (.cache). Base target: it is the AVR1 container and arithmetic --
    # no device, no renderer -- which is what lets its test run with neither.
    src/GiCache.cpp
    src/GltfImport.cpp
    src/Json.cpp
    src/OcAnim.cpp
    src/OcBeam.cpp
    src/OcGraph.cpp
    # .ocinput -- named input actions and default key/mouse bindings (see OcInput.hpp for the format
    # and why it needs no engine dependency). Beside the other AVR1-adjacent text readers for the same
    # reason .ocparticle states for itself minus the module split: this one has no type from outside
    # Aver.Formats to parse into at all (Aver.Framework is C#-only), so it never needed a third target.
    src/OcInput.cpp
    src/OcMap.cpp
    src/OcMesh.cpp
    src/OcWorld.cpp
    # The mesh importers. All three are dependency-free by construction: OBJ and USDA are text
    # formats this tree parses itself, and neither pulls a licence question into the build.
    src/ObjImport.cpp
    src/UsdImport.cpp
    # .ocland, the landscape heightfield. In the BASE target rather than a fifth one because it needs
    # nothing beyond Core/Platform/Assets -- it is the AVR1 container and arithmetic, no process, no
    # device, no .NET. That is what lets the landscape tests run on a machine with none of those.
    src/OcLand.cpp
    # .oclightmap and its unwrapper. In the BASE target for GiCache.cpp's reason: both are the AVR1
    # container plus arithmetic -- no device, no renderer, no .NET -- which is what lets their tests
    # run on a machine with none of those. The unwrapper is here rather than in a tool because the
    # runtime will need to VALIDATE an unwrap (vertex counts, UVs in range) even if only an offline
    # baker ever produces one.
    src/OcLightmap.cpp
    src/LightmapUnwrap.cpp
    # The C# actor-script scanner. MOVED HERE FROM Aver.Formats.Material, where it never belonged:
    # it parses a generated region out of a C# file and contains not one reference to pbr:: or
    # MaterialDesc, but sat behind AVER_MODULE_PBR anyway. Four things outside the material system
    # include its header -- formats.roslyn, render.actorpreview, the editor's ActorEditor and two
    # tests -- so with PBR off they all lost their implementation and the failure surfaced as
    # LNK1104 on a library nobody could see a reference to.
    src/ActorScript.cpp
    src/OcProject.cpp
    # .ocsave -- a whole world captured generically. In the BASE target because it needs only the
    # AVR1 container: a field KIND crosses it as a plain u32, so this never reaches Aver.Scene.
    src/OcSave.cpp
    # .ocnav -- baked navigation. Beside the other AVR1 readers; it is a grid and a header.
    src/OcNav.cpp
    # .ocbt -- a baked Synapse behaviour tree. Beside the other AVR1 readers for the same reason:
    # a node array and a string table, nothing that needs Aver.Scene.
    src/OcBt.cpp
    # .ocsnd -- a procedural sound as a node graph. Beside the other AVR1 readers for the same
    # reason: a node array, a link array and a header, nothing that needs a mixer.
    src/OcSound.cpp
    # .ocrig -- a control rig. TEXT rather than an AVR1 container, unlike its neighbours here:
    # a rig is four lines a person writes and reads, not a baked array. See OcRig.hpp.
    src/OcRig.cpp
    src/Texture.cpp
  DEPS
    Aver.Core
    Aver.Platform
    Aver.Assets
)

# Aver.Formats.Material — the .ocmat reader/writer, deliberately a THIRD target.
#
# It is the only format that needs a type from outside Aver.Formats: it parses straight into
# pbr::MaterialDesc rather than into a duplicate struct. Aver.Render.PBR is a Core-only shared DLL
# and cannot link this, and making Aver.Formats link the PBR DLL would put a renderer-family binary
# behind every headless tool that only wanted to read a map. See OcMat.hpp.
#
# Guarded on the module switch rather than defined unconditionally: with PBR off there is no
# MaterialDesc to parse into, and a target linking a library that was never added is a configure
# error a long way from the option that caused it.
if(AVER_MODULE_PBR)
  aver_add_module(Aver.Formats.Material
    SOURCES
      src/OcMat.cpp
      src/MaterialScript.cpp
      # Turning imported materials into .ocmat. Here rather than beside the importers for
      # exactly the reason this target exists: it is the step that needs pbr::MaterialDesc, and the
      # importers must stay in the PBR-free base target. Format-neutral: glTF, OBJ and USD feed it.
      src/MaterialCook.cpp
    DEPS
      Aver.Formats
      Aver.Render.PBR
  )
endif()

# Aver.Formats.Audio — the WAV reader, deliberately a FOURTH target, for the reason Material is a
# third: it is the only format that parses into a type from outside Aver.Formats. It fills an
# audio::SoundData directly rather than a duplicate struct that something would then have to copy
# across, and making Aver.Formats itself depend on Aver.Audio would put an audio dependency on every
# consumer of a mesh loader.
if(TARGET Aver.Audio)
  aver_add_module(Aver.Formats.Audio
    SOURCES
      src/Wav.cpp
      src/OcAudio.cpp
    DEPS
      Aver.Formats
      Aver.Audio
  )
  if(WIN32)
    # Media Foundation is how .mp3/.m4a/.wma/.flac get imported without vendoring a decoder. An MP3
    # decoder is two thousand lines of Huffman, IMDCT and polyphase synthesis, and the operating
    # system already ships a correct one -- the same reasoning that chose WASAPI over a vendored
    # audio backend. .wav still goes through this module's own reader, which needs none of it.
    target_link_libraries(Aver.Formats.Audio PRIVATE mfplat mfreadwrite mfuuid ole32)
  endif()
endif()
