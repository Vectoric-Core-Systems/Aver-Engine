# Fabric G6: networking (contracts)

Fabric G6 is the Godot shading-language front-end ([G6_SHADERS.md](G6_SHADERS.md)) and then networking. This file is the networking half. Godot's high-level multiplayer
(`MultiplayerAPI`, `@rpc`, `MultiplayerSpawner`, `MultiplayerSynchronizer`) sits on a `MultiplayerPeer`, and Godot's stock peer is `ENetMultiplayerPeer` over ENet. NET1 is
the transport under all of it: real sockets, a reliable-UDP protocol with ENet's semantics, and the Godot classes that expose them. Everything runs on the game thread
and is polled; nothing here opens a window, a GPU device or an audio device.

| Package | What | Status |
|---|---|---|
| NET1 | the transport: `modules/net` (sockets, reliable-UDP host/peer, simulated network), `modules/net.abi` (`Aver.Net.Abi`), and `PacketPeerUDP`, `UDPServer`, `StreamPeerTCP`, `TCPServer`, `StreamPeer`, `StreamPeerBuffer`, `IP`, `ENetConnection`, `ENetPacketPeer`, `MultiplayerPeer`, `ENetMultiplayerPeer`, `OfflineMultiplayerPeer`, and Godot's Variant binary encoding | DONE headless, nothing run in the editor or the game (`NetProtocolTest`, `NetSocketTest`, `NetAbiTest`, `NetworkTests`, `AbiEnumTest`) |

| NET2 | the high-level multiplayer API on top of NET1: `MultiplayerApi` / `SceneMultiplayer` (peer list, signals, authentication, server relay, `send_bytes`), per-branch APIs (`SceneTree.SetMultiplayer`), node authority, RPC (`[Rpc]` read, `Rpc` / `RpcId` / `RpcConfig` / `Callable.Rpc`, node-path cache), `MultiplayerSpawner`, `MultiplayerSynchronizer`, `SceneReplicationConfig`; the converter side (`@rpc`, `rpc` / `rpc_id` / `func.rpc`, `multiplayer.*`, `[Rpc]` kept, the two nodes and the config sub-resource rated) | DONE headless, nothing run in the editor or the game, everything on 127.0.0.1 (`MultiplayerTests`, `ReplicationTests` in `Aver.Fabric.Tests`, `FabricNetTest`; section "NET2" below) |

Not started: `HTTPRequest` / `HTTPClient`, `WebSocketPeer`, WebRTC. See "What is not here" below.

## What is reused from earlier networking work

Nothing. `docs/recon/networking.md` describes the OpenConstructor27 OCServer protocol (a hand-packed little-endian datagram per message, no reliability layer, "newest wins,
loss self-heals", game server port 7777, coordinator 7788). It was read for context. NET1's protocol is a different design (acknowledged, ordered, fragmenting), it is **not** wire
compatible with OCServer, and it is **not** wire compatible with ENet either: a converted Godot project talks to another Aver process (or to itself), never to a stock Godot server or
client. Only two ideas carried over from OCServer: the 1400-byte MTU budget as the default region (NET1 defaults to 1200) and token-bucket style rate limiting (NET1's bandwidth limit).

## Layers and files

| Layer | Files |
|---|---|
| OS sockets, addresses, name resolution, the datagram seam, `SimNetwork`, the reliable-UDP `Host` / `Peer` | `modules/net/` (`Aver.Net`, static; depends on Core only; `AVER_MODULE_NET`, on by default; nothing links it but `Aver.Net.Abi` and the tests) |
| the plain-C seam | `modules/net.abi/` (`Aver.Net.Abi`, shared; a derived module that exists when `AVER_MODULE_NET` is on; `docs/ABI.md` section 16 row) |
| the Godot classes | `scripting/csharp/Aver.Fabric/Nodes/NetworkNative.cs` (P/Invoke and enums), `NetworkPeers.cs` (IP, PacketPeer, PacketPeerUDP, UDPServer, StreamPeer, StreamPeerBuffer, StreamPeerTCP, TCPServer), `ENet.cs` (ENetConnection, ENetPacketPeer), `Multiplayer.cs` (MultiplayerPeer, ENetMultiplayerPeer, OfflineMultiplayerPeer), `Core/VariantBinary.cs` (the Variant encoding, `GD.VarToBytes`, `GD.BytesToVar`) |
| tests | `tests/net/` (`NetProtocolTest`, `NetSocketTest`, `NetAbiTest`), `scripting/csharp/Aver.Fabric.Tests/NetworkTests.cs`, rows in `tests/abi/src/AbiEnumTest.cpp` |
| shipping | `Aver.Net.Abi.dll ?AVER_MODULE_NET` on `scripts/game.allowlist` and `scripts/payload.allowlist` |

`AVER_MODULE_NET=OFF` removes both modules and the DLL; Aver.Fabric then answers `Error.Unavailable` from every networking call (`NetNative.Available` is false). No existing project, level or
material is affected either way.

## The socket layer (`aver/net/Socket.hpp`, `Address.hpp`)

`UdpSocket`, `TcpSocket`, `TcpListener` over ws2_32 (and iphlpapi for the interface list) on Windows and BSD sockets elsewhere. All sockets are non-blocking; IPv4 and IPv6 are separate
sockets (an IPv6 socket is `IPV6_V6ONLY`, so behaviour is the same on every OS). `waitSocket(fd, forWrite, ms)` is the poll helper. A byte-moving call returns a count (>= 0; a UDP datagram
may be empty) or `kWouldBlock`, `kSockError`, `kSockClosed` (TCP, in-order close). `Address` is a family, 16 bytes, a port and a scope id; `Address::parse` takes numeric text only,
`resolve(name, family)` goes through the system resolver for a name, and `localAddresses()` lists every interface address that is up.

Windows specifics handled: the WSAECONNRESET a UDP socket reports after a datagram hit a closed port is ignored (and turned off with `SIO_UDP_CONNRESET`); a truncated datagram reads as
oversize and is dropped by the host. macOS: `SO_NOSIGPIPE`; Linux: `MSG_NOSIGNAL`.

**No wildcard binds on their own.** A caller that wants "every interface" passes `"*"` (Godot's default bind address, honoured only when a script asks). A client host created with no bind
address opens its UDP socket at its first `connect()`, bound to the loopback address of the target's family when the target is loopback and left to the OS otherwise; a TCP connect to loopback
is made from the loopback address. The tests and the converter-facing defaults never open a wildcard port.

## The protocol (`aver/net/Protocol.hpp`)

`Host` owns one `DatagramSocket` and up to `maxPeers` (1..4095) `Peer` slots. It has **no clock of its own**: every call that moves time takes `nowMs`, so the same code runs on the real
clock (`Aver.Net.Abi` passes a monotonic millisecond clock) and on `SimNetwork`'s virtual one. `service(event, nowMs)` runs one round (receive, timers, resends, flush) when no event is queued
and hands out one event per call: `Connect` (data = the connect-data word), `Disconnect` (data = the disconnect data word, plus a reason: remote, local, timeout, connect-failed) and `Receive`
(channel, bytes, and in `data` the packet's send flags).

### Datagram and commands

Little-endian. Datagram header, 12 bytes: CRC-32 (u32, over the whole datagram with this field zero; written as 0 and not checked when `checksum` is off), the recipient's peer slot (u16, `0xFFFF`
while a connection is being asked for), the session (u16, the low half of the connection id), the sent time (u16, milliseconds), the command count (u8), a reserved byte. Then commands:
type (u8; bit 7 = acknowledge this, bit 6 = unsequenced variant), channel (u8, `0xFF` = control), reliable sequence (u16), payload length (u16), payload. Commands are length-framed, so a parser
never trusts anything else.

| Command | Reliable | Payload |
|---|---|---|
| ACK | no | echoed sent time (u16); the acknowledged channel and sequence are in the command header |
| CONNECT | yes, control | client slot u16, channels u8, mtu u16, connection id u32, connect data u32, in / out bandwidth u32 x2, protocol version u16 |
| VERIFY_CONNECT | yes, control | server slot u16, channels u8, mtu u16, connection id u32, in / out bandwidth u32 x2 |
| DISCONNECT | yes (graceful) or no (`disconnectNow`) | data u32 |
| PING | yes, control | none |
| BANDWIDTH | yes, control | in / out bytes per second u32 x2 |
| SEND_RELIABLE | yes | the packet |
| SEND_UNRELIABLE | no | sequence u16, the packet (unreliable ordered) |
| SEND_UNSEQUENCED | no | group u16, the packet |
| SEND_FRAGMENT | yes | first sequence u16, fragment count / number / total length / offset u32 x4, bytes |
| SEND_UNRELIABLE_FRAGMENT | no (bit 6 = unsequenced) | group u16, count u16, number u16, total length u32, offset u32, bytes |

### Semantics

* **Handshake.** `connect(address, channels, data)` allocates a slot, queues a reliable CONNECT and returns the peer (Connecting). A datagram with peer slot `0xFFFF` whose first command is a
  CONNECT makes a server-side slot (AcknowledgingConnect), acknowledges it and sends a reliable VERIFY_CONNECT; the client becomes Connected on the VERIFY and emits Connect; the server becomes
  Connected, and emits Connect with the data word, when the VERIFY is acknowledged. The channel count is the smaller of the request and the server's `channelLimit`; the mtu the smaller of the two.
  A retransmitted CONNECT is acknowledged again, not duplicated. A CONNECT from an address that already has a peer under another connection id replaces that peer (a restarted client).
  A full host, `refuseConnections`, a wrong protocol version, an inconsistent session: the datagram is dropped without an answer and the client gives up on its timeout (reason connect-failed).
* **Slots and sessions.** The wire carries the recipient's slot and a 16-bit session; a datagram from the wrong address, for a free slot or with the wrong session is ignored. A slot's
  `generation` moves each time it is reused or reset, so a stale reference never matches a new peer.
* **Reliable.** Per channel, a 16-bit sequence; the receiver delivers in order, buffers up to 512 commands ahead, re-acknowledges duplicates and drops anything beyond the window. The sender keeps 256 commands
  in flight per channel; more wait in a queue. Channels are independent: a stalled reliable stream on one never delays another (tested with a 1 MB packet on channel 0 and a small one on channel 1).
* **Resend and timeout (ENet's rules).** The retransmission timeout starts at 500 ms and then follows `rtt + 4 * variance` (never under 30 ms); each resend doubles it. A peer times out when the oldest
  unacknowledged command has waited `timeoutMaximum` (default 30 s), or when its timeout has doubled past `timeoutLimit` (default 32) times its first value and it has waited `timeoutMinimum` (default 5 s).
  `setTimeouts(limit, min, max)` is Godot's `ENetPacketPeer.set_timeout`. A connecting peer also fails if the CONNECT was acknowledged and no VERIFY follows within `timeoutMinimum`.
* **Unreliable ordered** (no flag; Godot's `TRANSFER_MODE_UNRELIABLE_ORDERED`): a per-channel sequence; a packet that is not newer than the last one delivered is dropped. It is not ordered against reliable
  packets (ENet orders them; NET1 does not).
* **Unsequenced** (`kFlagUnsequenced`; Godot's `TRANSFER_MODE_UNRELIABLE`): no order, duplicates dropped by a 1024-wide window.
* **Fragmentation.** A packet over `mtu - 18` bytes (reliable) or `mtu - 20` (unreliable) is cut into fragments of at most `mtu - 38`. Reliable fragments are ordinary reliable commands and are reassembled once the
  stream delivers them. Unreliable fragments carry a group; a group is delivered only when every fragment arrived (with the ordered/unsequenced rule applied to the whole), at most 16 groups are kept per channel.
  The packet cap is 32 MiB. Tested: 1 MB reliable with and without loss, 20 kB unreliable ordered and unsequenced, and a 600 byte MTU over real sockets.
* **RTT and jitter.** Every ACK echoes the sent time of the datagram that carried the command, so a sample is per transmission (a resend does not poison it). Smoothed `rtt` (7/8, 1/8), `rttVariance`
  (3/4, 1/4: the jitter estimate), `lastRtt`, `lowestRtt`; `packetLoss` (resent reliable commands over a one-second epoch, scaled by 65536).
* **Ping.** A Connected peer with nothing queued and nothing sent for `pingInterval` (default 500 ms) queues a reliable PING, so an idle link measures RTT and a dead one times out.
* **Disconnect.** `disconnect(data)` discards everything unsent or unacknowledged and sends a reliable DISCONNECT; both sides emit Disconnect (local reason on the requester, remote with the data word on the other).
  `disconnectLater(data)` waits until every queued packet is acknowledged. `disconnectNow(data)` sends one unreliable notice, resets the slot and emits nothing locally. `reset()` forgets the peer silently.
* **Bandwidth.** `outBandwidth` is a token bucket over the host's whole send rate; `inBandwidth` is advertised in the handshake (and by `setBandwidthLimit` later) and the sender never exceeds the smaller of its own
  `outBandwidth` and the peer's advertised `inBandwidth`. Acknowledgements bypass the bucket. Reliable data waits for tokens; unreliable data queued beyond a quarter second of budget is **dropped at the sender** and counted
  in `packetsDropped`. Tested: 100 KB under a 50 KB/s limit takes about two seconds either way; a 2000-packet unreliable flood loses most of it while a reliable packet still arrives.
* **Checksum.** CRC-32 per datagram; a mismatch is counted and dropped. Both ends must agree on `checksum`. Hostile input: random and checksum-valid random datagrams (40 000 in the test) never crash the parser; every
  length is validated against the bytes present, assembly sizes against the packet cap, fragment counts against their number.

### Differences from ENet (so a script's expectations are met or known)

Not implemented: the RTT-driven **packet throttle** (`throttle_configure` stores and reports its values, the throttle statistic always reads 32 = full), **compression** (`ENetConnection.compress` stores the mode; the
wire is unchanged), **DTLS** (`dtls_*` answer `Error.Unavailable`), ordering of unreliable against reliable packets, ENet's per-IP duplicate-peer limit, multicast, and mesh mode.
Different on purpose: a bad or refused connect is silent (no refusal datagram), the connection id is 16 bits on the wire, and `disconnect` is as ENet's (it discards unsent data; use `disconnectLater` to flush).
ENet's exponential back-off is kept as is, which means a bulk transfer over a very lossy wire wants patient `set_timeout` values (the 1 MB test at 15% loss each way sets `128, 10000, 3600000`; at 30% each way about half of
all round trips fail and the default 32-doublings limit is hit by chance).

## `SimNetwork` (`aver/net/Datagram.hpp`)

An in-memory network on a virtual clock. `bind(address)` gives a `DatagramSocket`; `config` (and `setLink(from, to, ...)` per direction) sets `lossProbability`, `duplicateProbability`, `minDelayMs`..`maxDelayMs` (a wide range
reorders), `maxDatagram`; `blackhole(address, true)` drops everything to and from an address (a vanished peer); `advance(ms)` moves time. A splitmix64 generator seeded in the constructor makes a run reproducible down to the
datagram counts (`NetProtocolTest` checks that two runs agree). The same `Host` code runs on it and on `UdpDatagramSocket`.

## The C ABI (`modules/net.abi`, `net_abi.h`)

50 functions, `aver_net_*`: `resolve`, `local_addresses`, `now_ms`; `udp_*` / `tcp_*` / `close` for sockets and listeners (one int32 handle space); `host_*` for a host (`create`, `connect`, `service`, `event_data`, `flush`,
`broadcast`, `bandwidth_limit`, `channel_limit`, `refuse`, `socket_send`, `peers`, `stat`, `local_port`, `max_peers`, `destroy`); `peer_*` for a peer named by an int64 key (generation << 16 | slot): `send`, `state`, `disconnect`
(request / later / now), `reset`, `ping`, `set_ping_interval`, `set_timeout`, `throttle`, `address`, `connect_data`, `stat`. Addresses cross as numeric text. A stale key or handle is a neutral answer (0, -1, `AVER_NET_SEND_BAD_HANDLE`).
Enums (event, reason, peer state, send flags, disconnect mode, peer and host statistics) are mirrored in `NetworkNative.cs` and pinned by `AbiEnumTest`, which also compares every signature. Version `AVER_NET_ABI_VERSION` 1.

## The Godot classes

Names, enums, constants, signals and error codes are Godot's. Where Aver differs:

* **Polling.** Nothing runs on a thread. `ENetMultiplayerPeer.Poll()`, `ENetConnection.Service()`, `StreamPeerTCP.Poll()` and `UDPServer.Poll()` move the network; until the `MultiplayerAPI` package
  exists a script calls `Poll()` on its multiplayer peer once per frame (Godot's `SceneMultiplayer` does this for a peer assigned to `multiplayer.multiplayer_peer`).
* **`IP`** is a static class (GodotSharp's shape). `resolve_hostname_queue_item` resolves synchronously and keeps the answer under an id. `get_local_interfaces` returns one entry holding all addresses.
* **`PacketPeerUDP`**: `bind(port, address = "*", recv_buf_size)`, `set_dest_address`, `connect_to_host` (a connected peer keeps only that address's packets), `put_packet`, `get_packet`, `get_packet_ip` / `_port`, `wait`
  (up to a minute), `set_broadcast_enabled`. `join_multicast_group` / `leave_multicast_group` answer `Error.Unavailable`. A peer that never bound opens its socket for the first send (loopback target: from loopback).
* **`UDPServer`**: `listen`, `poll`, `is_connection_available`, `take_connection`, `stop`, `max_pending_connections`. The first datagram from a new address makes a `PacketPeerUDP` that shares the server's socket; later
  datagrams from that address go to it; past `max_pending_connections` new sources are dropped; closing the peer forgets the address.
* **`StreamPeerTCP`** / **`TCPServer`**: `connect_to_host`, `poll`, `get_status`, `put_data`, `get_data` (blocking, 30 s ceiling), the partial variants, `get_available_bytes` (a read-ahead buffer), `set_no_delay`,
  `disconnect_from_host`. After an in-order close the buffered bytes stay readable and the status turns to `None` when they are consumed. `StreamPeerTCP.bind` answers `Error.Unavailable`.
* **`StreamPeer`**: every `put_*` / `get_*` in both byte orders; `put_string` / `get_string` are ASCII with a 32-bit length, the `utf8` variants UTF-8; `put_var` writes a 32-bit length then the Variant, `get_var` reads it.
  `StreamPeerBuffer` writes at its cursor and grows; a short `get_data` returns `Error.InvalidParameter` with what was there.
* **`PacketPeer`**: `get_packet`, `put_packet`, `get_var` / `put_var` (no length prefix: the packet is the encoding), `get_packet_error`, `encode_buffer_max_size`.
* **`ENetConnection` / `ENetPacketPeer`**: as listed in the Godot class reference, with `service(timeout)` returning `[EventType, peer, data, channel]`. A received packet is read with `peer.get_packet()`, `get_packet_channel()`,
  `get_packet_flags()`. `FLAG_UNRELIABLE_FRAGMENT` is accepted and ignored. `create_host()` with no address is a client host.
* **`MultiplayerPeer`** (`TransferModeEnum`, `ConnectionStatus`, `TargetPeerBroadcast` = 0, `TargetPeerServer` = 1, `transfer_mode`, `transfer_channel`, `set_target_peer`, `get_packet_peer` = the sender's id, `get_packet_channel`,
  `get_packet_mode`, `refuse_new_connections`, `poll`, `close`, `disconnect_peer(id, force)`, `peer_connected` / `peer_disconnected`).
* **`ENetMultiplayerPeer`**: `create_server(port, max_clients, max_channels, in_bw, out_bw)`, `create_client(address, port, channel_count, in_bw, out_bw, local_port)`, `set_bind_ip` (default `"*"`),
  `host`, `get_peer(id)`. The server is peer 1; a client picks a random id in 2..2^31-1 (`generate_unique_id`) and sends it as the connect-data word; the server announces it with `peer_connected(id)` once the handshake
  completes (an id of 0, 1 or one already taken is turned away); the client announces `peer_connected(1)`. Channels: 0 is reserved (Godot's own handshake channel), 1 carries reliable and unreliable-ordered traffic, 2 unreliable,
  and `transfer_channel` n > 0 is ENet channel n + 2 (received as channel n). `max_channels` is added to those three. Target peers: 0 everyone, > 0 that peer, < 0 everyone but that peer. `disconnect_peer(id)` is graceful
  (the signal comes on a later poll); with `force` the peer is dropped and the signal is immediate. A connect that never completes leaves the client `Disconnected` (the `connection_failed` signal belongs to `SceneMultiplayer`).
  `create_mesh` and `add_mesh_peer` answer `Error.Unavailable`.
* **`OfflineMultiplayerPeer`** as Godot's: always connected, peer 1, no packets.

### The Variant binary encoding (`Core/VariantBinary.cs`)

Godot's `encode_variant` layout (a u32 header with the type in the low 16 bits, little-endian, 4-byte padding for strings and packed bytes, 32-bit reals, the 64-bit flag on an int outside 32 bits and a float that
is not exactly a 32-bit float, the new NodePath format, typed-container headers read and skipped). Covered: all 39 types. Byte-exact against hand-worked layouts for nil, bool, int (32 and 64 bit), float (32 and 64 bit),
String, StringName, Vector2 / 2i, Array, Dictionary, packed arrays, Color, NodePath, Rid. **Not supported:** full objects (a class name plus properties): encoding answers `Error.Unavailable`, decoding `Error.Unauthorized`
(or `Unavailable` with `allow_objects`); an object encodes as its instance id and decodes as an `EncodedObjectAsId` (as in Godot when `full_objects` is off). Callables and signals encode as headers and decode empty. Decoding
bounds depth (256) and element counts and returns an error for any truncation; it never throws. `GD.VarToBytes`, `GD.VarToBytesWithObjects`, `GD.BytesToVar`, `GD.BytesToVarWithObjects` use it.

## What is not here (gap report)

Checked in `scripting/csharp/Aver.Fabric` when NET1 was written: there was no `HTTPClient`, `HTTPRequest`, `WebSocketPeer`, `WebSocketMultiplayerPeer`, `WebRTCPeerConnection`, `SceneMultiplayer`, `MultiplayerAPI`, `MultiplayerSpawner`,
`MultiplayerSynchronizer`, `MultiplayerPeerExtension` or `PacketPeerDTLS` / `StreamPeerTLS` / `TLSOptions` / `X509Certificate`. NET2 added the multiplayer classes (the section below); the rest is still absent.
What each needs, for planning:

* `HTTPClient` / `HTTPRequest`: HTTP/1.1 over `StreamPeerTCP` is a few hundred lines of C# for plain `http://`; HTTPS needs TLS, which needs either the OS TLS stack (SChannel / Secure Transport / OpenSSL: a native
  seam per platform) or a library. Neither is in NET1. No dependency was added.
* `WebSocketPeer`: a framing layer on top of the same TCP (and TLS for `wss://`); RFC 6455 needs SHA-1 and base64, both in the BCL.
* WebRTC: a full ICE / DTLS / SCTP stack. Needs a library (libdatachannel is the usual one); not implementable here without a new dependency.
* `SceneMultiplayer` and the rest: NET2 (below); it uses `MultiplayerPeer` exactly as specified here.
* `MultiplayerPeerExtension` (a script-written transport) is still absent; NET2's `SceneMultiplayer` takes any `MultiplayerPeer` subclass, so a C# class deriving from `MultiplayerPeer` works as one.

## The NET1 contract for later packages

1. **The transport is `MultiplayerPeer`.** A later `SceneMultiplayer` takes any `MultiplayerPeer`, calls `Poll()` once per frame (frame pass, before scripts), reads `GetAvailablePacketCount()` /
   `GetPacket()` and `GetPacketPeer()` / `GetPacketChannel()` / `GetPacketMode()`, and sends with `TransferMode`, `TransferChannel`, `SetTargetPeer` and `PutPacket`. It must not touch `Aver.Net.Abi` directly.
2. **Signals are `PeerConnected(long)` and `PeerDisconnected(long)`**, emitted from `Poll()` (and immediately by `DisconnectPeer(id, true)`). The id is Godot's: 1 is the server.
3. **Packet sizes.** `GetMaxPacketSize()` is 16 MiB (the protocol's cap is 32 MiB); a packet over the mtu fragments transparently.
4. **Channel numbering** is as written above (0 reserved). A multiplayer package that needs its own channels (RPC config, spawn, sync) must map them through `transfer_channel` values >= 1 and set `max_channels` on create.
5. **Time.** The ABI host runs on a monotonic millisecond clock (`aver_net_now_ms`); C++ code that wants the deterministic simulated network uses `aver::net::Host` with `SimNetwork` directly.
6. **Threading.** Single game thread; no call locks. A later package that wants a worker must add its own queue.
7. **New protocol commands** go in the command table above with the next type number (12 on), and the version constant `kProtoVersion` is bumped only for an incompatible change.
8. **Defaults unchanged.** Nothing in NET1 changes an existing project, level or material; the module is opt-in and nothing links it except its own C seam.

## Tests

* `NetProtocolTest` (C++, simulated network, virtual time, 88 checks): CRC vector; handshake (channel and mtu negotiation, connect data, slot ids); handshake under 40% loss, 30% duplication and 80 ms jitter over twelve seeds;
  600 reliable packets in order and exactly once on two channels under 30% loss, 10% duplication and 10..120 ms reordering; both directions at once; unreliable ordered dropping a stale packet (deterministic) and staying
  monotonic under jitter; unsequenced delivery out of order and duplicate suppression; fragmentation of 1 MB (clean and through 15% loss with duplication), 3000 bytes behind it, 20 kB unreliable, unreliable fragments lost whole;
  channel independence (a 1 MB packet on channel 0 does not delay channel 1) and four lossy channels in order; graceful disconnect with data word, `disconnectNow`, `disconnectLater` flushing, timeout events (configured 800 ms
  minimum), connect failure, the peer limit, `refuseConnections`; idle 20 s on pings with RTT near 80 ms and no jitter, jitter shown as variance; bandwidth limits (out, advertised in, later change, unreliable flood dropped,
  reliable kept); a five-client server, broadcast, a restarted client replacing its stale peer; 44 000 hostile datagrams; determinism.
* `NetSocketTest` (C++, real loopback sockets only, 51 checks): addresses, `localhost`, UDP (ports, empty datagram, closed-port datagram, v6), TCP (accept, 5 kB, 1 MB through partial sends, orderly close, refused connect, v6), the
  protocol over real UDP (handshake, 300 packets and a 300 kB packet, unsequenced, disconnect, 600-byte MTU fragmentation).
* `NetAbiTest` (C++, 59 checks): the C seam end to end over loopback, stale handles and keys.
* `NetworkTests` (C#, 310 checks): byte-exact Variant encodings and round trips of all types, hostile bytes; `StreamPeerBuffer`; `IP`; `PacketPeerUDP`, `UDPServer`; `StreamPeerTCP`, `TCPServer` (1 MB in partial writes);
  `ENetConnection` / `ENetPacketPeer`; two `ENetMultiplayerPeer`s (and a third) in one process over loopback: ids, signals, transfer modes, channels, target peers, `put_var` / `get_var`, a 500 kB packet, graceful and forced
  disconnect, close, limits, refusing, failed connect, argument errors; `OfflineMultiplayerPeer`. Skipped (and says so) when `build/bin/Aver.Net.Abi` was not built.
* `AbiEnumTest`: seven enum groups and every `aver_net_*` signature against `NetworkNative.cs`.

## Headed checks for the owner

NET1 has no editor, game or GPU surface; nothing in it can be seen in a window. These are the first things to confirm on a real network when the engine-run hold is lifted, because loopback tests cannot:

1. **Two machines (or one machine and a phone hotspot).** Run a converted project whose script does `peer.create_server(7777)` on one machine (this binds every interface, so Windows Firewall will ask once: allow Private networks)
   and `peer.create_client("<server ip>", 7777)` on the other. Expect `peer_connected` on both sides within a second and the client status to read `CONNECTED`. A client pointed at a machine with nothing listening must end
   `DISCONNECTED` after about 30 seconds (the default connect timeout).
2. **Loss and latency.** With a network shaper (Clumsy on Windows, `tc netem` on Linux) at 10% loss and 100 ms delay, send reliable packets at 20 Hz: all of them must arrive in order; unreliable ones may be missing but never out of order
   (`TRANSFER_MODE_UNRELIABLE_ORDERED`). `ENetPacketPeer.get_statistic(PEER_ROUND_TRIP_TIME)` must read about 200 ms and `PEER_PACKET_LOSS` / 65536 about 0.1 to 0.3.
3. **A big packet.** `put_packet` of 4 MB reliable between the two machines: it must arrive whole; the sender's `get_statistic(PEER_PACKETS_LOST)` shows the resends if the link drops anything.
4. **Pull the cable.** With a connection up, unplug the client: the server must raise `peer_disconnected` after the timeout (30 s default; `ENetPacketPeer.set_timeout(32, 1000, 5000)` makes it 5 s).
5. **UDP and TCP from a script.** A `PacketPeerUDP` bound to `127.0.0.1` and one sending to it; a `TCPServer` on `127.0.0.1` and a `StreamPeerTCP` client exchanging `put_var` / `get_var` of a Dictionary. Expect the same values.
6. **Variant interop.** Save `GD.VarToBytes` of a Dictionary containing a Vector3, a Transform3D and a NodePath to a file and read it in a stock Godot 4.4 with `bytes_to_var`: the values must match (this is the one check that compares
   our encoding with Godot's own; the unit tests compare against layouts worked out from Godot's source).

---

# NET2: the high-level multiplayer API

Godot's `MultiplayerAPI` (as `SceneMultiplayer`), RPC, `MultiplayerSpawner` and `MultiplayerSynchronizer`, on any `MultiplayerPeer` (NET1's `ENetMultiplayerPeer` in practice). All of it is `Aver.Fabric` C#: no engine change, no
new native code, no new module, no new dependency, no new project or level key. Everything runs on the game thread inside `Poll()`; nothing opens a window, a GPU device or an audio device.

## Where it lives

| What | File |
|---|---|
| `MultiplayerApi` (abstract; `RpcMode`; the signals; `CreateDefaultInterface`), `Node.Multiplayer`, `Node.Rpc` / `RpcId` / `RpcConfig` / `GetRpcConfig`, node authority, `SceneTree.GetMultiplayer` / `SetMultiplayer` / `MultiplayerPoll`, the `RpcEntry` table | `scripting/csharp/Aver.Fabric/Nodes/MultiplayerApi.cs` |
| `SceneMultiplayer`: peers, authentication, relay, raw packets, node references (the path cache), RPC send and receive | `Nodes/SceneMultiplayer.cs` |
| the replication half of `SceneMultiplayer`: spawn, despawn, sync, delta, visibility | `Nodes/SceneMultiplayer.Replication.cs` |
| `MultiplayerSpawner` | `Nodes/MultiplayerSpawner.cs` |
| `MultiplayerSynchronizer`, `SceneReplicationConfig` | `Nodes/MultiplayerSynchronizer.cs` |
| the `[Rpc]` attribute (GodotSharp's shape: `RpcMode`, `CallLocal`, `TransferMode`, `TransferChannel`) | `Core/Enums.cs` |
| `Callable.Rpc` / `RpcId` | `Core/Callable.cs` |
| the hook that polls the APIs at the head of every process frame (`SceneTree.BeginPass`) | `Nodes/SceneTree.cs` (`partial` now) |
| converter, GDScript: `@rpc`, `func.rpc()`, `func.rpc_id()` (`rpc`, `rpc_id`, `multiplayer.*`, `set_multiplayer_authority` come through the API model) | `scripting/csharp/Aver.Fabric.Convert/GdScript/Emitter.Func.cs` (`EmitRpcAttribute`), `Emitter.Call.cs` (`CallableCall`) |
| converter, GodotSharp: `[Rpc(...)]` kept as written | `Aver.Fabric.Convert/GodotSharpRewriter.cs` |
| converter, scenes: the two nodes, `_spawnable_scenes`, `SceneReplicationConfig` | `tools/fabric/FabricNet.cpp` (+ one line each in `FabricScene.cpp`, `FabricResources.cpp`) |
| fixture | `tests/fabric/data/NetGame/` |

## Peers in one process are branches of one tree

`SceneTree` is a process singleton in `Aver.Fabric` (groups, GUI, input and the frame counters are process-wide), so a test, a tool or a split-screen game cannot run several trees. Godot's own answer for several multiplayer peers in one process
is the per-branch API: `SceneTree.SetMultiplayer(api, rootPath)` gives the subtree at `rootPath` its own `MultiplayerApi`; `Node.Multiplayer` is the API of the deepest registered ancestor path (else the tree's default); node paths in packets are
relative to that branch's root. The NET2 tests build the server and each client as `/root/S`, `/root/C1`, `/root/C2` with the same content under each, a `SceneMultiplayer` and an `ENetMultiplayerPeer` (or an in-memory peer) per branch,
and let the tree's frame poll move all of them. Set the API **before** the branch's nodes enter the tree: a spawner or synchronizer attaches to the API it finds when it enters.

## The default API

A tree's default API is a `SceneMultiplayer` over an `OfflineMultiplayerPeer`: id 1, the server, no peers, `has_multiplayer_peer()` true. `rpc` offline runs `call_local` methods and does nothing for the rest (no error). Assigning `null` to
`multiplayer_peer` puts the offline peer back; assigning a peer that is `Disconnected` is refused with an error. Replacing a peer forgets the old peer list and caches **without signals**. `MultiplayerPoll` (default true) makes the tree call
`Poll()` once per process frame, before the `process_frame` signal, on the default API and every branch API; the physics pass drives the `visibility_update_mode = physics` re-evaluation. A script that polls itself (`Poll()` on the API)
uses real elapsed time for the synchronizers' intervals; the tree passes the frame's delta, which makes intervals exactly countable in a test.

## The wire format (Aver's own; it is not Godot's, and a converted project only talks to another Aver process)

Every `MultiplayerPeer` packet is one command: a first byte (low 7 bits the command, bit 7 "a node path follows the node id"), then little-endian fields. Strings are a u16 length and UTF-8.

| Cmd | Name | Body |
|---|---|---|
| 0 | RPC | node ref, method name (string), u16 argument count, each argument as `Core/VariantBinary` bytes (no length prefix; the codec knows where a Variant ends) |
| 1 | CONFIRM | u32 node id (reliable, channel 0): "I cached the path for this id" |
| 2 | RELAY | i32 target, the inner packet. A client's way to reach anyone but the server (below) |
| 3 | RELAYED | i32 original sender, the inner packet. The server's forward |
| 4 | ROSTER | u8 op (0 list, 1 add, 2 remove), u16 count, i32 ids. Server to clients: the other clients |
| 5 | RAW | the bytes of `send_bytes` |
| 6 | AUTH | the bytes of `send_auth` |
| 7 | AUTH_DONE | nothing: the sender called `complete_auth` |
| 8 | SPAWN | spawner ref, u32 net id, i32 scene index (-1 = custom), name, spawn data (Variant), u16 synchronizer count, then per synchronizer (tree order): u16 value count and the values of its spawn-flag properties in config order |
| 9 | DESPAWN | spawner ref, u32 net id |
| 10 | SYNC | synchronizer ref, u32 sequence, the values of its always-mode properties in config order |
| 11 | DELTA | synchronizer ref, u16 count, then (u16 config index, value) |

A **node ref** is `u32 id` and, when bit 7 of the command is set, the node's path as a string. The path is relative to the API's root node (the branch root, or the tree's window for the default API). The id is the sender's, one per node, handed out in
order. This is Godot's path cache: the first packets to a peer for a node carry the path; the receiver stores `id -> path` per sender, resolves it (a path that leaves the branch, or names nothing, is refused and reported) and answers CONFIRM;
once every receiver of a send has confirmed the id, the sender writes only the id. A broadcast uses the id form only when **every** connected peer has confirmed it; a sync to a subset of peers always carries the path. Confirmations are tracked per
peer and dropped when the peer leaves. `FullRefsSent` / `CachedRefsSent` on `SceneMultiplayer` (internal) count the two forms; the tests read them. A node renamed or re-parented after its path was cached keeps the old path at the receiver (as in Godot,
the cache is per node, not re-sent): do not rename networked nodes.

Methods go by **name**, not by Godot's index into a sorted table: the receiver looks the name up in its own RPC table and says "not an RPC method of that node" when it is absent. A script that renamed a method on one side gets a clear error instead of
a call to the wrong method.

## Server relay and the peer list

NET1's `ENetMultiplayerPeer` reaches only the server from a client. `SceneMultiplayer` therefore does what Godot's ENet peer does: with `server_relay` on (default), the server sends every new client a ROSTER of the clients already there and tells
the others about the newcomer (and about a departure). Clients emit `peer_connected` / `peer_disconnected` for them and list them in `get_peers()`. A client sends to any peer but the server inside a RELAY envelope (target 0 broadcast,
> 0 one client, < 0 everyone but that peer); the server unwraps it, processes the inner packet itself when the target includes the server (target 0, 1, or a negative id other than -1), and forwards RELAYED packets (with the original sender) to the
named clients, in the original transfer mode and channel. The receiving client treats the inner packet as sent by the original sender: `get_remote_sender_id()`, authority checks and path confirmations all see the original peer, and the CONFIRM
travels back through the server. A packet arriving through the relay can not itself be a relay, roster or authentication packet. With `server_relay` off clients are not told of each other, `rpc_id(other client)` is refused with an error, and a
client's broadcast reaches the server alone.

## Signals and their order

* Server, a client connects: `peer_connected(id)` (after authentication when `auth_callback` is set). Client, connecting to the server: `peer_connected(1)`, then `connected_to_server`, then `peer_connected(other)` for each relayed client.
* A connect that never completes: `connection_failed`. The server vanishing or the peer being closed: `peer_disconnected(1)` (when the transport reports it), then `server_disconnected`. A peer that closes: `peer_disconnected(id)` on the server and, through ROSTER, on the other clients.
* `connected_to_server`, `connection_failed` and `server_disconnected` come from the status change of the transport peer and are emitted at the end of the poll that saw it, after the transport's own `peer_connected` / `peer_disconnected`.

## Authentication

`auth_callback` (a `Callable(peer_id, data)`) makes a new peer **pending**: `peer_authenticating(id)` is emitted on both sides, `send_auth(id, bytes)` delivers bytes to the other side's callback, `complete_auth(id)` says this side is content, and the peer is admitted
(`peer_connected`, the roster, late spawns) only when both sides have said so. `auth_timeout` (seconds of **frame time**) disconnects a peer that has not finished and emits `peer_authentication_failed(id)`; so does a transport disconnect while pending.
Errors as Godot's: `send_auth` / `complete_auth` for a peer that is not pending is `InvalidParameter`, a second `complete_auth` is `FileBadDrive`. Nothing but authentication packets is accepted from a pending peer. `get_authenticating_peers()` lists them.

## RPC

* **Table.** The methods carrying `[Rpc]` in the node's class and its bases, by C# method name, plus the per-node overrides of `rpc_config(method, dict)` (`{"rpc_mode", "transfer_mode", "call_local", "channel"}`; nil removes). `get_rpc_config()` returns the merged table as a dictionary of those dictionaries.
  `Rpc("take_damage")` finds the method by Godot or C# spelling (`take_damage`, `TakeDamage`, `takeDamage`, `_takeDamage`); the name on the wire is the C# name.
* **Defaults.** GodotSharp's `[Rpc]`: authority, call remote, **reliable**, channel 0. GDScript's `@rpc`: authority, call remote, **unreliable**, channel 0; the converter writes every option explicitly so the difference does not matter.
* **Sending** (`rpc`, `rpc_id`, `Callable.rpc`, `MultiplayerApi.rpc`): target 0 is everyone else, > 0 one peer, < 0 everyone but that peer. With `call_local` the method also runs here when the target includes this peer (and not at all when the target is a single other peer);
  `rpc_id(own id)` runs a `call_local` method once and sends nothing. The packet goes with the table's transfer mode and channel. Errors (logged, and returned as an `Error`): the node is not in the tree or not below the API's root (`Unconfigured` / `InvalidParameter`), the method is not in the table
  (`InvalidParameter`, "unable to get the RPC configuration"), the peer is still connecting or is disconnected (`Unconfigured`), the target peer is not connected (`InvalidParameter`), an argument cannot be encoded (`InvalidParameter`).
* **Receiving.** The node is found by the path cache; the method by name; the mode is checked **on the receiver**: `any_peer` always, `authority` only when the sender is the node's `get_multiplayer_authority()`, `disabled` never. A refusal is an error in the log with Godot's wording
  ("RPC 'X' is not allowed on node '/path' from peer N. Mode is authority, authority is M.") and the call is dropped. A call that throws is reported and the remote sender id is reset. The arguments are converted to the method's parameter types as `Object.call` does.
  A local `call_local` call is not mode-checked (as in Godot) and runs with `get_remote_sender_id() == 0`.
* **Remote sender.** `get_remote_sender_id()` is the sender during an RPC (the original sender through a relay, nested calls restore the outer one) and 0 otherwise.
* **Limits.** Arguments travel through the Variant codec, so objects are not sendable (an object encodes as its instance id, as with `full_objects` off); `allow_object_decoding` is stored and has no effect. `transfer_channel` n > 0 needs the peer created with `max_channels` >= n.

## Authority

`set_multiplayer_authority(id, recursive = true)`, `get_multiplayer_authority()` (default 1), `is_multiplayer_authority()` (compares with the node's API's `get_unique_id()`; true offline for the default 1). Authority is **local state** of each process: every peer has to be told (a spawned node's
authority is not transferred by the spawner; the usual Godot pattern is a script that sets it from the node's name in `_enter_tree`, which both sides do). Nothing is cached about authority, so a change takes effect at the next RPC check or sync tick.

## MultiplayerSpawner

`spawn_path` (relative to the spawner; empty spawns nothing), `_spawnable_scenes` / `add_spawnable_scene` / `clear_spawnable_scenes` / `get_spawnable_scene(_count)`, `spawn_limit`, `spawn_function` (Callable taking the data, returning a node not in the tree), `spawn(data)`, signals `spawned(node)` and
`despawned(node)` (**remote peers only**, as in Godot).

* **Auto-spawn.** When a node whose `scene_file_path` is in the spawnable list is added under the spawn node on the spawner's authority, it is replicated to every peer it is visible to. Scene paths compare by the file they stand for (`res://a.tscn` and the converted `Maps/a.ocworld` are one scene);
  the **position in the list** travels, so every peer needs the same list in the same order. Spawn state is read when the node enters the tree (after its `_enter_tree`, before `_ready`), so set properties before `add_child`.
* **Remote side.** The scene (or `spawn_function(data)`) is instanced, takes the authority's exact name (auto names such as `@NetPlayer@2` included), gets the spawn-flag property values applied to its synchronizers (matched in tree order) **before** it enters the tree, is added under the spawn node, and `spawned` is emitted.
  A spawn from a peer that is not the spawner's authority, for an unknown scene index, past the limit, or with no valid spawn path is refused and reported. A duplicate net id is ignored.
* **Despawn.** Freeing or removing the node on the authority (or freeing its parent: a per-frame check notices nodes that vanished without a signal) sends DESPAWN to the peers that have it; they emit `despawned` and `queue_free` it.
* **Late joiners.** A peer admitted later gets a SPAWN for every tracked node visible to it, then the synchronizers' state. Each node is sent to a peer at most once.
* **Limit.** `spawn_limit` > 0 counts the nodes the spawner replicates at once; one more than allowed stays local and is reported (the node itself is not removed).
* **Visibility.** A spawned node is visible to a peer when **every** synchronizer in its subtree is. A synchronizer's visibility changing spawns or despawns the node on that peer.
* **`spawn(data)`** runs only on the authority (otherwise an error and null), needs `spawn_function` (error), and adds the returned node under spawn_path.

## MultiplayerSynchronizer and SceneReplicationConfig

`SceneReplicationConfig`: a list of property paths (`.:position`, `Child:visible`, `:rotation:y`: node path relative to `root_path`, then the property chain, read and written with `Object.get` / `set`), each with a `spawn` flag and a `replication_mode`
(`Never`, `Always`, `OnChange`; `property_get/set_sync` and `_watch` are the 4.0 spellings). Defaults for a new entry: spawn on, `Always`. A path without a property part is refused.

`MultiplayerSynchronizer`: `root_path` (default `..`), `replication_config`, `replication_interval`, `delta_interval`, `public_visibility`, `visibility_update_mode` (`Idle`, `Physics`, `None`), `set_visibility_for`, `get_visibility_for`, `add_visibility_filter`,
`remove_visibility_filter`, `update_visibility`, signals `synchronized`, `delta_synchronized` (receivers) and `visibility_changed(peer)`.

* **Who sends.** The peer that is the synchronizer node's multiplayer authority. A peer that becomes the authority starts from nothing: its visibility is re-evaluated and every visible peer gets the full state.
* **Always properties** go out every `replication_interval` seconds of frame time (0 = every frame) as one **unreliable** packet to the visible peers with a sequence number; a receiver drops a packet that is not newer than the last one it applied from that sender (so a late unreliable packet never overwrites a newer value), applies the rest and emits `synchronized`.
* **OnChange properties** are compared every `delta_interval` seconds (0 = every frame) with what was last sent (by their Variant encoding); the changed ones go out as one **reliable** delta and `delta_synchronized` is emitted on receivers. With nobody to tell, values stay unsent.
* **Spawn properties** travel in the SPAWN packet (above).
* **A peer that becomes able to see a synchronizer** (it joined, or visibility changed) is sent the watched values and one always-sync immediately, so an on-change value does not wait for its next change.
* **Receivers refuse** a sync or delta from a peer that is not the node's authority (reported). A sync that arrives before its node exists is dropped silently (unreliable packets race the spawn).
* **Limits.** A sync state over `max_sync_packet_size` (1350) or a delta over `max_delta_packet_size` (65535) is reported once and not sent.
* **Visibility** (differs in detail from Godot; Godot's `update_visibility` internals were not copied): a peer sees a synchronizer when its own setting is on (`set_visibility_for(peer, v)`, else `public_visibility`) **and** every filter returns true. `set_visibility_for(0, v)` changes `public_visibility`. A peer with its own setting keeps it when
  `public_visibility` changes. `Idle` re-evaluates every process frame, `Physics` every physics step, `None` only on `update_visibility()` / `set_visibility_for` / a peer joining. A change emits `visibility_changed(peer)` and acts (spawn / despawn for a spawned node, full state when a peer starts seeing it).

## The converter

* **GDScript.** `@rpc(mode, sync, transfer_mode, channel)` -- the four told apart by value, in any order, as Godot reads them -- becomes `[Rpc(MultiplayerApi.RpcMode.X, CallLocal = true, TransferMode = MultiplayerPeer.TransferModeEnum.Y, TransferChannel = n)]` with every option that differs from GodotSharp's defaults written
  (so a bare `@rpc` is `Authority, TransferMode = Unreliable`). Rated converted; an argument that is none of the keywords (a computed value) is ignored and rated approximated. `rpc("m", ...)`, `rpc_id(id, "m", ...)`, `multiplayer.get_unique_id()` / `is_server()` / `get_remote_sender_id()` / `get_peers()`,
  `multiplayer.multiplayer_peer = peer`, `multiplayer.peer_connected.connect(f)` (and the other four signals), `set_multiplayer_authority`, `is_multiplayer_authority` map through the API model like any other member. `f.rpc(args)` and `f.rpc_id(id, args)` become `Callable.From<...>(F).Rpc(args)` /
  `.RpcId(id, args)`; `Callable.Rpc` finds the method behind the delegate, so a method reference works and a lambda (which has no name to send) is refused at run time with an error.
* **GodotSharp C#.** `[Rpc(...)]` is kept exactly as written (the attribute has Godot's constructor and named options); `Rpc()` / `RpcId()` / `Multiplayer` / `MultiplayerApi.RpcMode` / `MultiplayerPeer.TransferModeEnum` are runtime members. Rated converted. An older conversion's `[Rpc("AnyPeer")]` still compiles (a string constructor remains).
* **Scenes.** `MultiplayerSpawner` and `MultiplayerSynchronizer` are placed as their own classes. Spawner: `spawn_path` (kept a Godot path), `spawn_limit`, `_spawnable_scenes` (each scene written as its converted level path, `Maps/player.ocworld`; a scene the project does not have keeps its position as an empty entry and is rated unsupported). Synchronizer: `root_path`, `replication_interval`, `delta_interval`,
  `public_visibility`, `visibility_update_mode`, `replication_config`. `SceneReplicationConfig` (sub-resource or `.tres`) becomes `Object(SceneReplicationConfig, "properties": [{"path": NodePath(".:position"), "spawn": true, "replication_mode": 1}, ...])`: Godot 4.0 / 4.1's `sync` / `watch` keys become a mode, an entry whose path names no property or whose mode does not exist is
  dropped (unsupported, with the reason), an absolute path is approximated. A spawner whose `spawn_path` names no node of its scene file, a spawner or synchronizer without a path / config, and every unknown property are reported. `spawn_function` is script-only and not stored in a scene.

## Differences from Godot (known and chosen)

* The wire format is Aver's; a converted game cannot talk to a stock Godot peer (as NET1 already said).
* Methods go by name, not table index.
* `connected_to_server` / `connection_failed` / `server_disconnected` follow the transport's status change, so they come after the transport's own peer signals.
* `has_multiplayer_peer()` is always true (the offline peer counts).
* Visibility semantics are Aver's, stated above. `visibility_changed` is emitted on every peer that evaluates a change (the authority each frame, any peer on an explicit `update_visibility` / `set_visibility_for`).
* `multiplayer_peer = null` installs an offline peer; reading it back gives that peer, not null.
* Objects are not sendable as RPC arguments or sync values; `allow_object_decoding` has no effect. `max_sync_packet_size` / `max_delta_packet_size` are honoured as described.
* There is no compression of the node path or the argument list beyond the cache.
* An RPC whose mode is `authority` is not refused on the **sender** (the receiver refuses it), as in Godot.
* A SceneTree is a process singleton: separate peers in one process are branches (above), and a `set_multiplayer` after the branch's nodes entered the tree does not move the spawners and synchronizers already registered.

## NET2 contract for later packages

1. **A new network feature that targets nodes** (an interest manager, a lag compensator) adds a command number after 11 to `SceneMultiplayer.Dispatch` and addresses nodes with `BeginNodePacket` / `ReadNodeRef`, which give it the path cache and confirmations for free.
2. **Authority and the remote sender are read at the moment of the call**, never cached; keep it so.
3. **Everything polls from `SceneMultiplayer.Tick(dt)`**; time is the sum of the frame deltas (`Time`), never the wall clock, so a test counts intervals exactly.
4. **Nothing here changes a project or a level**; no new project or level key was added. A future key goes where the project keys are parsed, documented, with a default that means today's behaviour (no networking).
5. **The `[Rpc]` attribute and `RpcEntry`** are the one RPC table; a converter or a generator writes attributes, it does not call `rpc_config` for converted methods.

## NET2 tests

* `MultiplayerTests` (in `Aver.Fabric.Tests`): the offline default API (authority, recursion, `rpc_config` override and removal, `call_local`, the three error returns, `Callable.rpc` / `rpc_id`, per-branch APIs and the deepest-ancestor rule, `create_default_interface`); the in-memory link
  with every send logged (reliable / unreliable / unreliable-ordered with channel 1 and the targets), a link that drops every unreliable packet and one that reorders (the unreliable-ordered RPCs arrive increasing), 200 random hostile packets, `send_bytes` / `peer_packet`, replacing a peer;
  over ENet on 127.0.0.1: ids, peer lists (including relayed clients), signal order, a client leaving, `disconnect_peer`, `connection_failed`; RPC delivery (authority, any-peer, `call_local`, `rpc_id`, negative targets, remote sender, unreliable and unreliable-ordered, authority moved and the old authority refused);
  the rejection cases (not authority, disabled, a method the receiver does not expose, a node the receiver lacks, a handler that throws, an unknown method on the sender, an RPC while connecting, a node outside the branch); the path cache (full then id-only, every Variant argument kind, nodes freed and re-added);
  relay (client to client, negative target, confirmations through the relay, `server_relay` off); authentication (callback, `send_auth`, `complete_auth`, the timeout and the disconnect), `refuse_new_connections`; and a **lossy UDP proxy** (15% loss, 10% duplication, 0..30 ms delay, seeded) in front of the server:
  40 reliable RPCs each way arrive once and in order, unreliable ones may be lost, the path confirmation survives.
* `ReplicationTests`: `SceneReplicationConfig` and the synchronizer / spawner properties and `Duplicate`; on the in-memory link, whose clock is the frame delta: exactly 10 syncs in 60 frames at 0.1 s, exactly 4 deltas at 0.25 s, every-frame at 0, no delta without a change, no regression on a link that delays packets by up to 5 ticks, authority moved, a receiver that disagrees
  refusing; a converted GDScript (the NetGame scripts) compiled and called across three peers; over ENet: auto-spawn with spawn state and names, the non-spawnable and the non-authority, the limit, despawn by free, by remove and by freeing the parent, late joiners, `spawn(data)` with a custom function and its errors, visibility
  spawning and despawning (per peer, public, filter), always / on-change / spawn modes, a late joiner's state, authority changes, visibility filters and update modes, the size limit; and spawn / sync / delta / despawn through the lossy proxy.
* `FabricNetTest` (C++, `AverAssetC fabric --scenes-only` on `tests/fabric/data/NetGame`, 33 checks): the classes, the properties, the spawnable list, the config in both spellings and its dropped entries, and the report.
* `ConvertScriptsTests` was updated: `[Rpc]` is kept and rated converted.

## NET2 headed checks for the owner

Nothing here has run in the editor or the game, and nothing left 127.0.0.1. When the engine-run hold is lifted:

1. **Open `tests/fabric/data/NetGame` converted** (File > Import Godot Project, or `AverAssetC fabric tests/fabric/data/NetGame <out>`) and run it twice: a script calls `Lobby.host(7777)` on one and `Lobby.join("127.0.0.1", 7777)` on the other. Expect `peer_connected` on both sides, and `Players` on both to show the same player nodes (the spawner at work);
   close the client: the server's `Players` loses that player when the script frees it.
2. **Two machines.** Same, with a second PC. The converted `Main` calls `start_round.rpc(1)` from the server; both must print the same round. Pull the cable on the client: the server must report `peer_disconnected` after the transport's timeout (30 s by default).
3. **Sync feel.** On a player whose `MultiplayerSynchronizer` has `replication_interval = 0.1`: move the owner's player and watch it on the other machine: motion in 10 Hz steps with no backwards jumps. With a network shaper at 10% loss the motion is coarser but never goes back.
4. **A late joiner** connects after three players exist: all three appear at their current positions and health (health is on-change: it must not wait for the next hit).
5. **Visibility.** Put a `public_visibility = false` synchronizer in a spawned player and call `set_visibility_for(<peer>, true)` from the server: the player appears on that peer only and disappears again on `false`.
6. **Authority.** Give a node to a client (`set_multiplayer_authority(id)` on every peer) and call an `authority` RPC on it from the client: it runs on the others; from the server it is refused with the "not allowed" error in the console.
7. **Server relay on a real network.** Three machines: client A `rpc_id(<client B's id>, ...)` reaches B with `get_remote_sender_id()` equal to A's id. With `server_relay = false` set on the server's `multiplayer` it must fail with the error in the console.
