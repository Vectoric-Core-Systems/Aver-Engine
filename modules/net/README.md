# Aver.Net  (`modules/net`)

- **Language:** C++
- **Depends on:** Core
- **Option:** `AVER_MODULE_NET` (default on). Nothing links it except `Aver.Net.Abi` (`modules/net.abi`) and the tests.

OS sockets (UDP, TCP, listeners; IPv4 + IPv6; non-blocking; `waitSocket`), name resolution, and a reliable-UDP host/peer protocol with
ENet-like semantics (channels, reliable / unreliable-ordered / unsequenced packets, fragmentation, RTT estimation, timeouts, bandwidth
limits), plus `SimNetwork`, an in-memory network with seeded loss, delay, duplication and reordering that the protocol is tested on.

Not wire compatible with ENet or with the old OCServer protocol (`docs/recon/networking.md`). Design, the wire format and the contract
for the layers above: [docs/plans/fabric/G6_NETWORK.md](../../docs/plans/fabric/G6_NETWORK.md).
