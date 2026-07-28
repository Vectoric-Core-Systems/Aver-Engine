# RoslynTest -- the averdesign backend, checked against the built-in scanner.
#
# The important half is the AGREEMENT: on a file both backends can read they must produce identical
# ids, values and byte spans, because a fallback that quietly disagrees writes a coordinate into the
# wrong byte of somebody's source. The other half is the files the scanner is right to decline.
#
# It SKIPS, loudly, when averdesign is not staged: a machine with no .NET SDK cannot build the tool,
# and reporting that as a failure would train people to ignore a red suite.
# Guarded like the module: with no Aver.Formats.Roslyn there is no backend to test.
if(TARGET Aver.Formats.Roslyn)
add_executable(RoslynTest src/RoslynTest.cpp)
target_link_libraries(RoslynTest PRIVATE Aver.Formats.Roslyn Aver.Formats.Material Aver.Formats Aver.Platform Aver.Core)
set_target_properties(RoslynTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
