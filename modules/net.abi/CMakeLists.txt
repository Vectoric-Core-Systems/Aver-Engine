# Aver.Net.Abi -- the plain-C seam onto OS sockets and the reliable-UDP host/peer protocol, so scripts and managed
# extensions can open UDP and TCP sockets and ENet-style hosts and peers.
#
# SHARED because the .NET host P/Invokes it. It links Aver.Net (sockets and protocol: Core only, no scene, no device) statically and nothing
# else, so a managed test host loads it from a plain folder. The file is Aver.Net.Abi and no managed assembly shares the name, so the shared
# NativeResolver that finds Aver.* beside the executable is all it needs.
add_library(Aver.Net.Abi SHARED src/NetAbi.cpp)

target_include_directories(Aver.Net.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Net.Abi PUBLIC cxx_std_20)
target_link_libraries(Aver.Net.Abi PRIVATE Aver.Net Aver.Core)
target_compile_definitions(Aver.Net.Abi PRIVATE AVER_NET_ABI_BUILD)

set_target_properties(Aver.Net.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # beside the exe, where DllImport looks
