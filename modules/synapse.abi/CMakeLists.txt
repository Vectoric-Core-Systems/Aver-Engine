# Aver.Synapse.Abi -- the plain-C seam onto steering/crowds, hearing, cover and squads, so the
# managed Aver.Synapse assembly can P/Invoke it.
#
# SHARED because the .NET host loads it, and it is where the ONE SynapseAi instance lives: it links
# Aver.Synapse.Scene statically but exposes the systems only through synapse_ai_abi.h. A host that
# also links Aver.Synapse.Scene must call aver_syn_ai_tick rather than build its own SynapseAi.
#
# Named apart from the managed assembly (Aver.Synapse), like Aver.UI.Abi / Aver.UI, so the default
# DllImport probe finds the right file and no resolver is needed.
add_library(Aver.Synapse.Abi SHARED
  src/SynapseAiAbi.cpp)

target_include_directories(Aver.Synapse.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Synapse.Abi PUBLIC cxx_std_20)
# Aver.Scene is SHARED, so scene::World::instance() is the host's. Aver.Synapse.Scene brings
# Aver.Synapse (and Aver.Physics when built) with it.
target_link_libraries(Aver.Synapse.Abi PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene Aver.Core)

target_compile_definitions(Aver.Synapse.Abi PRIVATE AVER_SYN_ABI_BUILD)

set_target_properties(Aver.Synapse.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # next to the exe, where DllImport looks
