# Fabric G6 NET1 (https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G6_NETWORK.md).
#
# NetProtocolTest -- the reliable-UDP host/peer protocol on SimNetwork (an in-memory network with seeded loss, delay, duplication and reordering,
# on a virtual clock): no sockets, no wall time, the same result every run.
add_executable(NetProtocolTest src/NetProtocolTest.cpp)
target_link_libraries(NetProtocolTest PRIVATE Aver.Net Aver.Core)
set_target_properties(NetProtocolTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NetSocketTest -- the OS socket layer and the protocol over real sockets. LOOPBACK ONLY: every bind is 127.0.0.1 or ::1 (a wildcard bind pops the
# Windows Firewall dialog), every wait is short, and no name but "localhost" is resolved.
add_executable(NetSocketTest src/NetSocketTest.cpp)
target_link_libraries(NetSocketTest PRIVATE Aver.Net Aver.Core)
set_target_properties(NetSocketTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NetAbiTest -- the C seam end to end over loopback: sockets, a host pair, events, peer calls.
add_executable(NetAbiTest src/NetAbiTest.cpp)
target_link_libraries(NetAbiTest PRIVATE Aver.Net.Abi Aver.Core)
set_target_properties(NetAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
