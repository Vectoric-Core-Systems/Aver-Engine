# AbiEnumTest -- the C ABI's integer constants against the C# enums that restate them.
#
# Same shape as tests/repo, and for the same reason it states: this reads SOURCE FILES rather than
# exercising a runtime, so it links Aver.Core alone for the log macros and needs no device, no window
# and no project. AVER_REPO_ROOT is what lets it find the headers and the .cs files; without it the
# suite SKIPS rather than failing, which is right for a binary copied away from its tree.
#
# Named *Test and emitted into <build>/bin because that is the ENTIRE registration mechanism in this
# repo -- tools/mcp/aver_mcp.py sweeps bin/ for *Test.exe and there is no CTest anywhere. Nothing
# else has to be told this suite exists.
add_executable(AbiEnumTest src/AbiEnumTest.cpp)
target_link_libraries(AbiEnumTest PRIVATE Aver.Core)
target_compile_definitions(AbiEnumTest PRIVATE AVER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(AbiEnumTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
