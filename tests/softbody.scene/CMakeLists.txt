# SoftBodySystemTest -- cloth and destruction on scene entities, headless. Meshes, .ocsoft assets and skin
# palettes are injected and nothing is drawn, so the whole system is decidable with no GPU and no device: a
# flag hanging and blowing, a cape following a palette, a dent that stays, a pane that shatters (Torn, then
# Detached, dust for the shards), explosions and cuts, debris caps, the rebuild budget, replay equality and the
# legacy CSoftBody path. Physics is the real Jolt world, through Aver.Physics.
#
# Wave 10 adds two more, linked the same way:
#   SoftBodyDepthTest       fields, the reveal handshake, thick breaks, break effects, saved damage, baking, the proxy cage
#                           and replay equality with all of it on.
#   SoftBodyCalibrationTest the preset tuning scenarios (an energy ladder against a door, a pane, a crate, a flag): prints the
#                           measured table and asserts only coarse brackets.
# Wave 11 adds SoftShapeSystemTest (jelly, rope, grab, cut, sensors, fast cloth and inflatables; the real Jolt world, no GPU).
foreach(_t SoftBodySystemTest SoftBodyDepthTest SoftBodyCalibrationTest SoftShapeSystemTest)
  add_executable(${_t} src/${_t}.cpp)
  target_include_directories(${_t} PRIVATE src)
  target_link_libraries(${_t} PRIVATE Aver.SoftBody.Scene Aver.Physics Aver.Scene Aver.SoftBody Aver.Formats Aver.Core)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
