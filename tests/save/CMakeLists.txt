# SaveWorldTest — capturing a live world and putting it back, with no GPU and no framework.
#
# It installs its OWN host function pointers (none, in fact — every block here is plain entities),
# which is the whole point of Aver.Save taking them rather than linking Aver.Framework: the module
# is testable without booting a gameplay layer.
add_executable(SaveWorldTest src/SaveWorldTest.cpp)
target_link_libraries(SaveWorldTest PRIVATE Aver.Save Aver.Scene Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SaveWorldTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SaveActorTest — the ACTOR half, with a real .NET runtime in the process.
#
# Separate from SaveWorldTest deliberately: that one must keep proving the module works with NO
# framework and NO scripting host, which is the architectural claim Aver.Save makes by taking
# function pointers instead of linking them. This one installs real ones.
if(TARGET Aver.Scripting.Host AND TARGET Aver.Framework)
  add_executable(SaveActorTest src/SaveActorTest.cpp)
  target_link_libraries(SaveActorTest PRIVATE Aver.Save Aver.Scene Aver.Formats
                                              Aver.Framework Aver.Scripting.Host
                                              Aver.Core Aver.Platform)
  set_target_properties(SaveActorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  # It loads the staged bridge and the sample actor assembly at run time.
  add_dependencies(SaveActorTest Aver.Scripting.Host)

  # GraphVarPersistTest — graph-VAR persistence is scoped to GameInstance-classed entities, at the
  # HostBridge.cs provider layer (FindPersistedGraphHost), not in Aver.Save itself. Same real-CLR
  # shape as SaveActorTest immediately above, driving aver_fw_graph_var_count/at/set directly rather
  # than going through a full capture()/restore() round trip.
  add_executable(GraphVarPersistTest src/GraphVarPersistTest.cpp)
  target_link_libraries(GraphVarPersistTest PRIVATE Aver.Save Aver.Scene Aver.Formats
                                                     Aver.Framework Aver.Scripting.Host
                                                     Aver.Core Aver.Platform)
  set_target_properties(GraphVarPersistTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_dependencies(GraphVarPersistTest Aver.Scripting.Host)
endif()
