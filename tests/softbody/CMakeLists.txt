# PlasticSoftBodyTest -- the plastic cage solver, with no GPU, no scene and no Jolt.
#
# Links Aver.SoftBody and Aver.Core only, which is the module's claim made into something the build
# enforces. Every property here -- a bar keeping its bend past yield, a strip tearing into exactly two
# pieces, energy staying bounded at the fixed rate, replays being bitwise identical -- is one a
# screenshot cannot decide.
add_executable(PlasticSoftBodyTest src/SoftBodyTest.cpp)
target_link_libraries(PlasticSoftBodyTest PRIVATE Aver.SoftBody Aver.Core Aver.Platform)
set_target_properties(PlasticSoftBodyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PlasticSoftBodyAsyncTest -- the fixed-rate worker against a synchronous replay. A separate executable so a
# thread problem names itself rather than hiding among the solver arithmetic.
add_executable(PlasticSoftBodyAsyncTest src/SoftBodyAsyncTest.cpp)
target_link_libraries(PlasticSoftBodyAsyncTest PRIVATE Aver.SoftBody Aver.Core Aver.Platform)
set_target_properties(PlasticSoftBodyAsyncTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
