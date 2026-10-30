# Aver.Net -- OS sockets (UDP, TCP, IPv4 + IPv6) and a reliable-UDP host/peer protocol with ENet-like semantics, plus SimNetwork, an in-memory
# network with seeded loss, delay, duplication and reordering that the protocol is tested on. Core only; no third-party code.
#
# OPT-IN. Nothing links it except Aver.Net.Abi (modules/net.abi, the seam managed extensions drive) and the tests; a game that
# does not ask for it does not carry it. Design and contract: docs/plans/fabric/G6_NETWORK.md.
aver_add_module(Aver.Net
  SOURCES
    src/Address.cpp
    src/Socket.cpp
    src/SimNetwork.cpp
    src/Protocol.cpp
  DEPS
    Aver.Core
)

# The socket layer is the OS's own: ws2_32 (and iphlpapi for the local interface list) on Windows, libc elsewhere.
if(WIN32)
  target_link_libraries(Aver.Net PUBLIC ws2_32 iphlpapi)
endif()
