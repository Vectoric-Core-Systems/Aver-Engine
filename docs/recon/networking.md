I now have a complete, faithful picture across all three layers (UE client module, external C# server/coordinator, and the native C ABI cores). Writing the spec.

---

# OpenConstructor27 — Networking / Replication / Session Architecture Spec (for a UE-free port)

## 0. Scope & source map

| Concern | Authoritative source |
|---|---|
| UE-side data contracts, framed-batch wire | `Net/OCNetTypes.h` |
| UE-native RPC/RepNotify transport | `Net/OCVehicleReplicator.h/.cpp` |
| Per-vehicle authority seam | `Net/OCVehicleNetBridge.h/.cpp` |
| Grid-wide server API (byte in/out) | `Net/OCNetAuthoritySubsystem.h/.cpp` |
| Modular channel framework (successor) | `Net/OCNetChannel.h`, `OCNetModule.h`, `OCNetCoordinator.h/.cpp`, `OCVehicleNetModules.h` |
| **Live playable UDP client** | `Net/OCNetSocketClient.h/.cpp` |
| Loopback/reference server harness | `Net/OCLoopbackServer.h/.cpp` |
| **Authoritative game server (C#)** | `OCServer/OCServerHost/Net/Wire.cs`, `GameServer.cs`, `Session/SessionManager.cs` |
| **Matchmaking coordinator (C#)** | `OCCoordinator/OCCoordinatorHost/Net/Wire.cs`, `CoordinatorServer.cs` |
| UE-free sim core (C ABI) | `OCServer/OCSimCore/include/oc_sim.h` |
| UE-free match core (C ABI) | `OCCoordinator/OCMatchCore/include/oc_match.h` |

**Critical faithfulness note:** there are **three distinct serialization schemes**, and they are NOT interchangeable:

- **(A) Raw hand-packed little-endian UDP protocol** — `OCServerHost/Net/Wire.cs` ⇄ `OCNetSocketClient.cpp`. This is the **actual live client↔server protocol** and the **only thing that talks to the existing OCServer/OCCoordinator**. It is already engine-neutral (Wire.cs header comment: "Explicit little-endian byte layout so it ports 1:1 to the UE/C++ side", `Wire.cs:5-7`).
- **(B) `OCNetWire` framed-batch** (`OCNetTypes.h:243-353`) — a UE-reflection binary format used only by `UOCNetAuthoritySubsystem::ServerApplyInbound/ServerCollectOutbound` and the loopback harness. **OCServerHost does not speak this.** It is UE-version-locked (`OCNetTypes.h:242` "both peers must be the same engine version").
- **(C) UE-native RPC + RepNotify** (`UOCVehicleReplicator`) — UE netdriver reflection serialization; only for a UE-dedicated-server build.

**To keep the existing OCServer/OCCoordinator interoperable you only need to re-implement (A) + the coordinator protocol.** (B) and (C) are UE-internal and are what you are replacing.

---

## 1. Coordinate system, units, endianness

| Property | Value | Evidence |
|---|---|---|
| Handedness / axes | UE left-handed, X forward, Y right, Z up | `oc_sim.h:31` ("cm, vehicle/world Z-up, mirrors FVector"), capsule extents X=half-length Y=half-width `OCNetSocketClient.cpp:106-113` |
| Position units | centimeters (cm), `float` | `oc_sim.h:31`, `FVector_NetQuantize100` on `WorldPoint` `OCNetTypes.h:118` |
| Rotation on the UDP wire | yaw only, **degrees**, `f32` | `Wire.cs:18,32`; `SendLocalPose` uses `GetActorRotation().Yaw` `OCNetSocketClient.cpp:534,539` |
| Velocity | cm/s, `float` (wide pose only) | `OCNetSocketClient.cpp:83-88,542-544` |
| Mass | kg, `float`, clamped [200,5000] on wire | `OCNetSocketClient.cpp:543` |
| Impact speed | km/h, `float` | `OCNetTypes.h:120`, `Wire.cs:28` |
| Plastic beam deformation | field `PlasticDeltaCm` in cm (`float`); **on UDP wire quantized to `i16` millimetres** (`cm*10`) | `OCNetTypes.h:150`; `OCNetSocketClient.cpp:615-616` (`Mm = round(PlasticCm*10)`), `Wire.cs:26` |
| Endianness | **little-endian, explicit** on both sides | writer/reader `Wire.cs:141-182`; client `PutU*/GetU*` `OCNetSocketClient.cpp:38-48` |
| Framing / magic | **none.** Each UDP datagram = `[u8 MsgType]` then fields. No length prefix, no version header per packet (version only inside Join) | `HandlePacket` switches on `Data[0]` `OCNetSocketClient.cpp:744-747`; `HandlePacket` server `GameServer.cs:173-185` |
| String encoding | UTF-8, length-prefixed. Two conventions: `[u16 len][bytes]` (`Str()`) and `[u8 len][bytes]` (Join name) | `Wire.cs:174-181`; Join name uses `u8` `Wire.cs:220`, `OCNetSocketClient.cpp:519` |
| MTU budget | 1400 bytes (`Proto.MaxPacket`); pose/timing paged, damage fragmented | `Wire.cs:137`; `GameServer.cs:34` |

---

## 2. Transport & topology

- **Transport = raw UDP** on both the game plane and the matchmaking plane. UE side uses `FSocket`/`FInternetAddr`/`ISocketSubsystem` (`OCNetSocketClient.h:21,319-322`); server uses `UdpPeer` over a .NET `Socket`. No TCP, no reliability layer, no ACKs on the game plane — "newest wins, loss self-heals" (`OCNetChannel.h:24`).
- **Default ports:** game server **7777** (`Wire.cs:135`, `OCNetSocketClient.h:118`); coordinator **7788** (`OCCoordinator Wire.cs:33`, `OCNetSocketClient.h:211`).
- **Two independent sockets on the client:** a game socket (`Socket`) and a separate coordinator socket (`CoordSocket`) live only while matchmaking (`OCNetSocketClient.h:319-323`).
- **Client receive loop:** non-blocking, `HasPendingData` guarded, bounded to 256 datagrams/tick (`OCNetSocketClient.cpp:734-741`).
- **Server main loop:** fixed-tick (`RunLoop`, default derived from `TickHz`), each tick: drain inbound → lobby tick → hold-sweep → `_core.Step(dt)` → broadcast poses at `PoseSendHz` (decoupled, lower than tick) → timeout sweep → coordinator heartbeat → race-timing broadcast at `RaceStateSendHz` (`GameServer.cs:97-157`).
- **Per-session flood control (server):** packet-rate token bucket (`MaxPacketsPerSecond`/`PacketBurst`), impact-rate bucket (`MaxImpactsPerSecond`), pose plausibility gate (teleport/speed-hack), NaN/Inf poison rejection (`GameServer.cs:180-184,291-315,331-340`).

---

## 3. Authority model

**The live Alpha is client-authoritative relay, with a documented migration path to server-authoritative.**

- Alpha (shipping): each client runs its own car locally (`UOCVehicleMovementComponent`), sends its pose via `ClientPose`, and the server **stores + rebroadcasts** it (no server sim consumed for that car). See `OCNetSocketClient.h:6-10`, `Wire.cs:32-35`, `GameServer.cs:276-321,537-540` (server prefers `s.HasPose` relay pose, falls back to sim-core pose).
- Server-authoritative path exists but is dormant: `Input` (msg 3) feeds `oc_sim` `SetInput`/`Step` (`GameServer.cs:260-274`), but `OCSimCore` is a **STUB** (trivial forward motion; real physics is "P2", `oc_sim.h:6-10`). The client does not currently send `Input`.
- **Damage is always author-once-replicate:** each car's damage is authored only by its owner and replayed onto everyone else's proxy of that car (`OCNetSocketClient.h:150-152`).
- The **UE-native** role model (for a UE dedicated build) is genuinely server-authoritative: `EOCNetRole { Standalone, Authority, Replica }` (`OCNetTypes.h:35-43`), auto-detected from `GetNetMode()/HasAuthority()` (`OCVehicleNetBridge.cpp:28-33`). A `Replica` has `bAutonomousDamageDecisions=false` — it dents from seeds but never self-decides a detach (`OCVehicleNetBridge.cpp:82-95`).
- **Coordinator = matchmaking control plane only**, never gameplay: it queues players, groups them (`OCMatchCore`), reserves/launches game servers, and hands the client a `(serverIp, port, token)` (`OCCoordinator Wire.cs:6-9`). The game server validates the match token at Join if it holds a live reservation (`GameServer.cs:225-233`).

---

## 4. Identity & cross-references

| Handle | Type | Assigned by | Notes |
|---|---|---|---|
| `FOCVehicleId.Value` | `int32`, 0 = invalid | `UOCNetAuthoritySubsystem` (`NextId++` from 1) | Deliberately NOT the UE `FNetworkGUID`, so an external server addresses cars by the same id (`OCNetTypes.h:69-72`, `OCNetAuthoritySubsystem.cpp:22-27`) |
| Server `VehicleId` | `int` (`_nextId++` from 1) | `SessionManager.Join` | The same id space; broadcast in `Welcome`, keyed everywhere (`SessionManager.cs:60,77`) |
| `JoinOrder` | `int` monotonic | `SessionManager` | Stable grid ordering even after id reuse (`SessionManager.cs:42,77`) |
| Coordinator `ticketId` | `u32` | coordinator | Player queue ticket |
| Coordinator `instanceId` | `u32` | coordinator | Registered game-server handle |
| `match token` | `u64` | coordinator | Reservation proof; carried in Join (`Wire.cs:11-13`) |
| Map parity key | `u64 mapContentId` + `u8 hashAlgo` + `32B mapRoot` + `u32 buildId` | cook pipeline | The Join parity gate (`Wire.cs:11`, `OCNetSocketClient.h:120-123`) |

`sourceCarId` in relayed `Impact`/`Snapshot`, and `carId` in `PoseBatch`/timing, all == `VehicleId`.

---

## 5. GAME PROTOCOL (A) — exact byte layouts (little-endian, no packet header)

Every datagram begins with `[u8 MsgType]`. Enum values are load-bearing and must be preserved (`Wire.cs:9-80`, client mirror `OCNetSocketClient.cpp:24-30`).

### 5.1 MsgType values
`Join=1, Welcome=2, Input=3, PoseBatch=4, Leave=5, Heartbeat=6, Reject=7, ClientPose=8, Impact=9, Snapshot=10, Despawn=11, LobbyState=12, LobbyControl=13, LobbyClientInfo=14, PenaltyNotify=15, TimingState=16, SectorSplit=17, VehicleConfig=18, ConfigStatus=19`

### 5.2 Client → Server

| Msg | Layout after `[u8 type]` | Source |
|---|---|---|
| **Join (1)** | `[u16 protoVer=1][u32 clientBuildId][u64 mapContentId][u8 hashAlgo][32B mapRoot][u8 caps][u8 nameLen][name UTF8][u64 matchToken]` | `OCNetSocketClient.cpp:512-521`; parse `GameServer.cs:189-233` |
| **Input (3)** *(reserved / server-auth)* | `[u32 clientFrame][u8 throttle][u8 steer][u8 brake][u8 flags]` | `Wire.cs:17`; `GameServer.cs:260-274` |
| **ClientPose (8)** *(live path)* | `[u32 frame][f32 x][f32 y][f32 z][f32 yawDeg]` **+ optional wide** `[f32 vx][f32 vy][f32 vz][f32 massKg]` | `OCNetSocketClient.cpp:535-546`; `GameServer.cs:276-321` |
| **Impact (9)** | `[f32 lx][f32 ly][f32 lz][f32 nx][f32 ny][f32 nz][f32 speedKmh]` (car-**local** frame) | `OCNetSocketClient.cpp:590-596`; `GameServer.cs:323-346` |
| **Snapshot (10)** | `[u16 seq][u8 chunkIdx][u8 chunkCount][chunkBytes]`; chunk ≤1200B | `OCNetSocketClient.cpp:619-632` |
| **Heartbeat (6)** | `[u32 clientFrame]` | `Wire.cs:20` |
| **Leave (5)** | *(empty)* | `OCNetSocketClient.cpp:274` |
| **LobbyClientInfo (14)** | `[u16 nameLen][name][u8 ready]` | `GameServer.cs:376-386` |
| **VehicleConfig (18)** | `[u8 category][u16 pathLen][path UTF8]` (path opaque, ≤220 printable ASCII) | `Wire.cs:71-76`; `GameServer.cs:387-414` |
| **LobbyControl (13)** | `[u16 cmdLen][cmd UTF8]` (accepted only from host car) | `GameServer.cs:415-433` |

Reassembled Snapshot payload = `[i32 beamCount]( [i32 beamIdx][i16 plasticMm] )*` (`OCNetSocketClient.cpp:608-617`, `Wire.cs:26`). **This UDP damage form carries only sparse plastic deltas — no broken bitset, detached parts, or repair counter** (unlike the richer struct in §7).

### 5.3 Server → Client

| Msg | Layout after `[u8 type]` | Source |
|---|---|---|
| **Welcome (2)** | `[i32 vehicleId][u16 tickHz][u64 mapContentId][u16 mapNameLen][mapName UTF8][u8 serverFlags][u8 phase]` | `GameServer.cs:244-252`; client parses through mapName only `OCNetSocketClient.cpp:749-757` |
| **Reject (7)** | `[u8 reasonCode][u16 detailLen][detail UTF8]` | `GameServer.cs:437-446` |
| **PoseBatch (4)** | `[u16 count]( [i32 id][f32 x][f32 y][f32 z][f32 yawDeg] )*`; record 20B, **or 36B wide** (`+[f32 vx][f32 vy][f32 vz][f32 massKg]`) | `GameServer.cs:517-558`; **self-describing stride** on client `OCNetSocketClient.cpp:763-812` |
| **Impact (9)** | `[i32 sourceCarId][f32 lx][f32 ly][f32 lz][f32 nx][f32 ny][f32 nz][f32 speedKmh]` | `GameServer.cs:341-345`; `OCNetSocketClient.cpp:829-847` |
| **Snapshot (10)** | `[i32 carId][u16 seq][u8 idx][u8 count][chunkBytes]` | `GameServer.cs:358-363`; `OCNetSocketClient.cpp:849-892` |
| **Despawn (11)** | `[i32 carId]` | `GameServer.cs:451-457` |
| **LobbyState (12)** | `[u32 epoch][u8 phase][u8 format][u16 laps][u16 durationMin][u16 phaseRemainingSec][u16 maxPlayers][u16 flags][u64 mapContentId][u16 mapNameLen][mapName][u16 umapLen][umap][u16 rosterCount]( [i32 carId][u8 pflags][u8 nameLen][name] )*` | `Wire.cs:43-48`; `GameServer.cs:665-713` |
| **ConfigStatus (19)** | `[u8 status][u16 detailLen][detail UTF8]` (status: Required=0, Accepted=1, Rejected=2) | `Wire.cs:84-89`; `GameServer.cs:472-478` |
| **PenaltyNotify (15)** | `[i32 carId][u8 ruleId][u8 action][u16 addedTenthsSec][u32 lapNumber][u16 detailLen][detail]` | `GameServer.cs:617-627`; `OCNetSocketClient.cpp:940-963` |
| **TimingState (16)** | `[u16 count]( [i32 carId][u8 position][u8 lapsDone][u32 lastLapMs][u32 bestLapMs][i32 gapMs][u16 penTenths][u8 flags] )*` — entry 21B, paged | `GameServer.cs:564-600`; `OCNetSocketClient.cpp:911-938` |
| **SectorSplit (17)** | `[i32 carId][u8 sectorIdx][u32 sectorMs][i32 deltaToBestMs][u8 flags]` | `GameServer.cs:629-636`; `OCNetSocketClient.cpp:965-976` |

Bitfields: `serverFlags` bit0 LobbyEnabled, bit1 VehicleConfigRequired (`GameServer.cs:248-249`). LobbyState `flags` bit0 InSession, bit1 LateJoinAllowed, bit2 StartCountdown (`GameServer.cs:671-675`); roster `pflags` bit0 Ready, bit1 Host, bit2 Configuring (`GameServer.cs:704-705`). TimingState `flags` bit0 DQ, bit1 lapInvalid, bit2 inPit(reserved), bit3 wrongWay (`Wire.cs:63`; client `OCNetSocketClient.cpp:929`). SectorSplit `flags` bit0 personalBest, bit1 sessionBest, bit2 invalidated (`Wire.cs:65`).

### 5.4 Enums (byte-for-byte mirrored client/server)
`RejectReason { None=0, ProtocolMismatch=1, MapUnknown=2, MapHashMismatch=3, UnsupportedHash=4, ServerFull=5, Banned=6, SessionInProgress=7, InvalidMatchToken=8, VehicleConfigTimeout=9 }` (`Wire.cs:108-120`); `HashAlgo { None=0, XxHash128=1, Blake3=2, Sha256=3 }` (`Wire.cs:124-130`, default 3 `OCNetSocketClient.h:123`); `SessionPhase { Lobby=0, Practice=1, Qualifying=2, Race=3, Results=4 }` (`Wire.cs:100`); `SessionFormat { FreeRoam=0, Practice=1, Qualifying=2, Race=3 }` (`Wire.cs:104`); `PenaltyRule { TrackLimits=0, CornerCut=1, WrongWay=2, PitSpeed=3, CausingCollision=4, ImpossibleLap=5 }`; `PenaltyAction { Warning=0, LapInvalidated=1, AddedTime=2, Disqualified=3 }` (`Wire.cs:92-96`).

> **Ambiguity flagged:** the `UOCNetSocketClient` I read is a **non-lobby Alpha client** — its `OCMsg` enum omits 12/13/14/18/19 (`OCNetSocketClient.cpp:27-28`), so it does not parse LobbyState/ConfigStatus or send VehicleConfig/LobbyControl. A lobby-aware client that exercises those messages exists per the server, but is outside the files provided. Treat §5.2/5.3 rows 12–19 as authoritative from the **server** side (`Wire.cs`).

---

## 6. MATCHMAKING PROTOCOL — coordinator plane (little-endian, `[u8 CoordMsg]` header)

`CoordMsg { QueueJoin=1, QueueUpdate=2, MatchFound=3, QueueLeave=4, QueueReject=5, SrvRegister=10, SrvWelcome=11, SrvHeartbeat=12, SrvReserve=13, SrvGoodbye=14 }` (`OCCoordinator Wire.cs:10-25`). `str = [u16 len][UTF8]`.

| Msg | Dir | Layout after `[u8 type]` |
|---|---|---|
| QueueJoin (1) | C→Co | `[u16 protoVer][u8 mode][str mapPref][str name]` (mode 0=Casual,1=Premier) |
| QueueUpdate (2) | Co→C | `[u32 ticketId][u8 state][u16 position][u16 found][u16 needed][u16 etaSec]` |
| MatchFound (3) | Co→C | `[u32 ticketId][u64 token][str serverIp][u16 port][str map]` |
| QueueLeave (4) | C→Co | `[u32 ticketId]` |
| QueueReject (5) | Co→C | `[u32 ticketId][u8 reason]` |
| SrvRegister (10) | S→Co | `[u16 protoVer][str advertiseIp][u16 port][u16 maxPlayers][str map]` |
| SrvWelcome (11) | Co→S | `[u32 instanceId]` |
| SrvHeartbeat (12) | S→Co | `[u32 instanceId][u16 players][u8 phase][str map]` |
| SrvReserve (13) | Co→S | `[u64 token][str map][u16 count]( [i32 playerId] )*` |
| SrvGoodbye (14) | S→Co | `[u32 instanceId]` |

Client mirror and parse: `OCNetSocketClient.cpp:33,335-401`. `QueueState { Searching=0, Matched=1 }`, coordinator `RejectReason { None=0, PremierRequiresSubscription=1, BadRequest=2, NoCapacity=3 }` (`Wire.cs:27-28`; client `EOCQueueRejectReason` `OCNetSocketClient.h:57`).

**Client flow (`OCNetSocketClient.cpp`):** `StartMatchmaking` opens `CoordSocket`, `SendQueueJoin` (`:332-341`), re-sends every 2s as keepalive (`:346-347`); on `MatchFound` it sets `ServerHost/ServerPort`, stores `MatchToken`, closes the coord socket, and auto-`Connect()`s to the game server (`:380-393`). Anti-spoof: only datagrams from the known coordinator address are accepted (`:359-361`).

**Coordinator behavior (`CoordinatorServer.cs` + `oc_match.h`):** bucket tickets by map, greedily form matches oldest-first; `OcMatchParams { minPlayers, maxPlayers, maxWaitMs, eloBand* (reserved) }`, `oc_match_form(...)` is pure/deterministic (`oc_match.h:44-69`). Casual is live; Premier (Elo-banded, subscription) is reserved (`oc_match.h:6-10`). Optional on-demand server spawning via `ServerLauncher` (config `CoordinatorServer.cs:44-54`).

---

## 7. UE-side data contracts (structs) and the framed-batch (B)

These are the richer contracts used by the UE RPC path (C) and the `OCNetWire` framed-batch API (B). A UE-free port replaces the reflection serializer with explicit field marshalling; the field tables below ARE that layout.

### 7.1 Structs (`OCNetTypes.h`)

**FOCInputState** (`:92-106`) — client→authority, byte-quantized:
| field | type | units/meaning |
|---|---|---|
| Throttle | u8 | 0..255 → 0..1 |
| Steer | u8 | 0..255, 128=center → -1..+1 (`(s-128)/127`) |
| Brake | u8 | 0..255 → 0..1 |
| Flags | u8 | `EOCInputFlags` bitmask: Handbrake=1, ShiftUp=2, ShiftDown=4, Reverse=8 (`:56-63`) |
| ClientFrame | i32 | reconciliation/ordering |

**FOCImpactEvent** (crash seed, `:114-122`): `WorldPoint` (cm, quantize100), `WorldNormal` (unit, quantizeNormal), `SpeedKmh` (f32), `PartHint` (u8, 0xFF = client picks nearest). WORLD-space on the UE wire; each client applies its own Y-mirror (`:108-112`). *(Contrast: the UDP `Impact` in §5 is **car-local**.)*

**FOCDetachEvent** (`:127-135`): `PartIndex` (u8, index into cage Parts), `Impulse` (cm/s?, quantize10), `Mode` (`EOCDetachMode { DetachWhole=0, Shatter=1, PanelShed=2 }`, `:47-52`). Always server-decided.

**FOCBeamDamage** (`:145-151`): `BeamIndex` (i32), `PlasticDeltaCm` (f32, permanent shortening in cm). Sparse — only yielded beams.

**FOCDamageSnapshot** (durable end-state, `:157-168`):
| field | type | meaning |
|---|---|---|
| YieldedBeams | `FOCBeamDamage[]` | sparse plastic deltas |
| BrokenBitset | `u8[]` | 1 bit/beam; beam *i* → bit `(i&7)` of byte `(i>>3)` |
| DetachedParts | `u8[]` | part indices already gone |
| RepairCounter | i32 | monotonic; change ⇒ "reset to factory" |

This is a **durable end-state, never an impact log** (`:153-156`) — replaying impacts would double-apply. `bTorn/bFreed` are derived locally from `BrokenBitset` (`:156`).

**FOCVehicleConfigBlob** (`:180-185`): wraps `FCarBuild Build` (chassis soft-ref, livery ops, etc.), sent **once** at join. Content-parity gated (`ValidateConfigContent`, `OCNetAuthoritySubsystem.cpp:191-204`).

Tagged bulk entries `FOCVehicleInput / …DamageEntry / …ImpactEntry / …DetachEntry / …RepairEntry` = `{FOCVehicleId Id; <payload>}` (`:191-229`).

### 7.2 The `OCNetWire` framed batch (B) (`OCNetTypes.h:299-338`)
Frame = `[u8 version=1][i32 count]( [u8 type][i32 id][i32 len][bytes] )*`. `EMsg { None=0, Input=1, Impact=2, Detach=3, Repair=4, Damage=5, Config=6 }` (`:279-288`; note these tag values differ from the UDP MsgType values in §5.1 — do not conflate). Sanity bound: count ≤ 1,000,000, malformed frame dropped whole (`:324,330`). Each `bytes` is `T::StaticStruct()->SerializeBin` via `FObjectAndNameAsStringProxyArchive` (soft-object refs → path strings, `:249-265`) — **UE-reflection, engine-version-locked**; a non-UE peer must marshal the §7.1 fields explicitly.

---

## 8. Tick / snapshot / delta model

- **Server tick:** fixed-rate loop (`GameServer.RunLoop`, `:97-157`); sim stepped every tick, but **poses broadcast at a lower `PoseSendHz`** and **timing at `RaceStateSendHz`**, both decoupled from tick and from render frame rate.
- **Client send cadence:** `SendHz` default 30 (`OCNetSocketClient.h:143`); UE RPC path coalesces per-frame input and flushes latest at `NetSendHz` default 64 (`OCVehicleReplicator.h:56-62`, `.cpp:108-121`).
- **What is a "snapshot" vs a "delta":**
  - Continuous **pose** = unreliable, newest-wins, no delta baseline (each PoseBatch is a full absolute pose). Interpolated on the client (`RemoteInterpSpeed`, first update snaps, `OCNetSocketClient.h:34,144`).
  - **Impacts** = one-shot events (unreliable in UE path).
  - **Detaches** = one-shot events (reliable in UE path).
  - **Damage** = durable **state** channel: server pushes a fresh snapshot only when dirty, at most every `SnapshotPushInterval` (default 0.2s, `OCVehicleReplicator.h:64-67`). "Grid-scaling" property: a clean race sends nothing (`OCNetAuthoritySubsystem.h:69`, `ServerCollectOutbound` emits empty when nothing changed, `.cpp:145-165`).
  - **Config** = once at join, on change.
- **Dirty tracking:** `UVehicleDamage::GetDamageChangeToken()` / `IsDamageDirtySince(token)`; bridge captures with `bDeltaOnly` which clears the token (`OCVehicleNetBridge.cpp:194-222`).
- **Delta-baseline / ACK** exist only as a *declared contract* for the modular framework (`FOCNetChannelProfile.bDeltaBaseline`, `OCNetChannel.h:62-63`) — not yet implemented; current damage sends are full end-state snapshots.
- **Sequence clock:** `UOCNetCoordinator::NextSeq()` monotonic int32, stamped on outbound payloads so a module can order/dedup across actors (`OCNetChannel.h:92-95`, `OCNetCoordinator.cpp:82,100`). UE only guarantees ordering within one actor's channel.

---

## 9. What is actually replicated for a vehicle (the answer to "transform + cage node deltas?")

| Data | How it crosses | Cadence | Reconstruction |
|---|---|---|---|
| Body pose | `ClientPose`→`PoseBatch` (pos cm + yaw°); optional velocity+mass | `SendHz`/`PoseSendHz` | absolute; interpolated |
| Full transform (UE build) | UE movement replication (`SetReplicateMovement`) | `NetUpdateFrequency`=`NetSendHz` | UE built-in |
| Driving input | UDP `Input` (dormant) / UE `Server_SendInput` | `NetSendHz` | server sim (P2) |
| Crash seed | `Impact` (car-local UDP; world-space UE) | event | **replayed through the deterministic cage** → dents reproduced locally (`OCVehicleNetBridge.cpp:124-134`) |
| Part detach | UE `Multicast_Detach` (reliable) / snapshot `DetachedParts` | event + state | `CommandDetachPart` |
| Damage end-state | UDP `Snapshot` (plastic only) / `FOCDamageSnapshot` (full) | dirty ≤ `SnapshotPushInterval` | overwrite, no replay |
| Car config | `VehicleConfig`/`Config` blob | once at join | rebuild locally |

**No per-node / per-beam live transform stream is ever sent.** "Cage node deltas" are transmitted only as (a) the sparse **yielded-beam plastic deltas** + **broken-beam bitset** inside the damage snapshot, and (b) crash **seeds** that each peer replays through an identical deterministic soft-body cage. Everything visual (dents, debris, aero, audio) is reconstructed from `config + impacts + replicated movement` (`OCNetTypes.h:18-21`). This is the founding design rule and the key bandwidth property to preserve.

---

## 10. Modular channel framework (the successor architecture — build the port around this)

Newer than the vehicle-specific bridge; a general, physics-agnostic replication core (`OCNetChannel.h:8-17`).

- **One thing crosses the seam:** `FOCNetPayload { FName Channel; u8 Type; u8 Flags; i32 Seq; u8[] Bytes }` (`OCNetChannel.h:78-104`). Transport never inspects `Bytes`.
- **Channel profile** drives scheduling without the transport knowing semantics: `FOCNetChannelProfile { Channel; EOCNetReliability{Unreliable,Reliable,State}; TargetHz; EOCNetRelevancy{Always,DistanceScaled,OwnerOnly}; bDeltaBaseline; BasePriority }` (`OCNetChannel.h:21-67`).
- **`UOCNetModule`** = one concern owning one channel: `ApplyNetCommand / ValidateNetCommand / DrainNetEvents / IsNetDirty / CaptureNetState / RestoreNetState` + designed-in esports hooks `PredictTick / Reconcile / Interpolate` (no-ops today) (`OCNetModule.h:31-99`).
- **`UOCNetCoordinator`** hosts modules and exposes the transport-facing API `ApplyCommand / DrainEvents / CaptureState / RestoreState` + the sequence clock (`OCNetCoordinator.h`).
- Built-in vehicle modules: **Drive** (input; predict/reconcile stubbed), **Damage** (impact/detach/repair events + snapshot), **Config** (`OCVehicleNetModules.h`). Canonical channel ids `Drive/Damage/Config` (`OCNetChannel.h:107-112`). Sub-types: Drive `Cmd_SetInput=0`; Damage `Cmd_ApplyImpact=0, Cmd_DetachPart=1, Cmd_Repair=2`; Config `Cmd_ApplyConfig=0` (`OCVehicleNetModules.h:34,53-55,97`).

For a UE-free engine, **re-implement this module/coordinator/channel triad as plain C++** and drive it with either protocol (A) framing or a new framing of your choice — but keep protocol (A) for OCServer interop (§12).

---

## 11. Server-facing grid API (the shape your host loop must provide)

`UOCNetAuthoritySubsystem` is the per-tick contract an external headless host drives (`OCNetAuthoritySubsystem.h:22-113`):

```
ServerApplyInbound(recvBytes)   // decode+apply this tick's client msgs (Input/Repair/Config)
// world ticks
ServerCollectOutbound(sendBytes) // one framed batch: impacts+detaches+repairs+dirty snapshots
```
Granular pumps: `RegisterVehicle/Unregister/Find`, `PumpInputs`, `CollectDirtySnapshots`, `DrainImpactEvents/DrainDetachEvents/DrainRepairEvents` (`.cpp` throughout). **Repair rides the snapshot's `RepairCounter`, never a bare event** (so a dropped repair can't leave a client permanently wrecked, `.cpp:154-157`). `Harvest()` is once-per-frame (`GFrameCounter` guard) so the three drains share one pass (`.cpp:78-104`). The **loopback server** (`OCLoopbackServer.cpp`) is the reference implementation of this exact loop with a memcpy standing in for the socket.

The native **`oc_sim` C ABI** is the UE-free authoritative sim seam already used by the C# host and intended for the client to link the *same source* (`oc_sim.h:1-11`): `oc_world_create/destroy`, `oc_world_add_vehicle(id)/remove`, `oc_vehicle_set_input(id, OCInput)`, `oc_world_step(dt)`, `oc_vehicle_position(id, OCVec3*)`. `OCInput {throttle,steer,brake,handbrake}`, `OCVec3 {x,y,z}` (cm, Z-up). ABI guarded by `oc_abi_version()`. **This is your ready-made physics interop boundary for the port.**

---

## 12. Preserving interop with the existing OCServer / OCCoordinator

To keep the **existing** C# `OCServerHost` and `OCCoordinatorHost` binaries working against the new engine, the port must reproduce, exactly:

1. **Protocol (A), the raw LE UDP game protocol** (§5) — same MsgType byte values, same field order/widths/endianness, no packet header, MTU ≤1400, pose paging, damage fragmentation ≤1200B, the `[u16 len]`/`[u8 len]` string conventions, and the additive-trailing-field rule (old clients omit `caps`/`name`/`matchToken`; guarded reads on the server, `GameServer.cs:218-225`).
2. **The coordinator protocol** (§6) — CoordMsg byte values, the 2s queue keepalive, and the auto-connect-with-token handoff.
3. **Join parity fields** — `protoVer=1`, `mapContentId (u64)`, `hashAlgo` (default Sha256=3), `32B mapRoot`, `buildId`. Mismatches map to the `RejectReason` enum. Verification is a compatibility/parity gate, not the cheat boundary (`Wire.cs:106-107`).
4. **Client-authoritative relay semantics** — send `ClientPose`, expect `PoseBatch` back; author your own damage and relay seeds/snapshots; the server does not simulate your car in the Alpha.
5. **Identity space** — `VehicleId` is a server-assigned `int32` from 1; use it as `sourceCarId`/`carId` everywhere.

You do **not** need `OCNetWire` (B) or UE RPC (C) for this interop — they never reach OCServerHost. If you want the framed-batch host API internally, re-derive each `EMsg` payload from the §7.1 field tables rather than UE `SerializeBin`.

---

## 13. UE dependencies to strip / replace in the port

| UE dependency | Where | Replace with |
|---|---|---|
| `USTRUCT/UCLASS/UENUM/GENERATED_BODY/UPROPERTY/UFUNCTION/UObject` reflection | all `Net/*.h` | plain C++ structs/enums/classes |
| `FVector_NetQuantize100/10/Normal`, `Engine/NetSerialization.h` | `OCNetTypes.h:6,118-133` | plain `vec3` + explicit quantization (cm×100 etc.) |
| `FVector, FTransform, FRotator, FName, FString, TArray, TMap, TSet, TWeakObjectPtr, TObjectPtr, TSoftObjectPtr` | pervasive | your math/containers; soft-refs → stable content-id strings |
| `FMemoryWriter/Reader`, `FObjectAndNameAsStringProxyArchive`, `StaticStruct()->SerializeBin` (the whole `OCNetWire` codec) | `OCNetTypes.h:243-353` | explicit little-endian field marshalling (already the model in Wire.cs) |
| `FSocket, FInternetAddr, ISocketSubsystem, PLATFORM_SOCKETSUBSYSTEM` | `OCNetSocketClient.*` | BSD sockets / asio / enet UDP |
| `FTSTicker` / `FTickerDelegate` | `OCNetSocketClient.h:323,330-331` | your own timer/loop |
| UE replication: `UFUNCTION(Server/NetMulticast/WithValidation)`, `DOREPLIFETIME`, `ReplicatedUsing`, `GetLifetimeReplicatedProps`, `ForceNetUpdate`, `SetNetUpdateFrequency`, `FlushNetDormancy`, `ENetRole/GetLocalRole/HasAuthority/GetNetMode`, `SetReplicates/SetReplicateMovement` | `OCVehicleReplicator.*` | your net stack; role from your own session state |
| `DECLARE_DYNAMIC_MULTICAST_DELEGATE*` | `OCNetTypes.h:235-237`, `OCNetSocketClient.h:60-108` | plain callbacks/`std::function` |
| `UActorComponent, UWorldSubsystem, UGameInstanceSubsystem, FSubsystemCollectionBase` | components/subsystems | your ECS/systems |
| `FCarBuild`, `Build/CarBuild.h`, `FLivery`, chassis `TSoftObjectPtr<UObject>` | config blob | engine-neutral build POD + content-id registry |
| `UVehicleDamage, UVehicleBuilder, UVehicleAerodynamics, UCageVehicleAssemblerComponent, UOCVehicleMovementComponent` sim seam | `OCVehicleNetBridge.cpp:38-52` | your sim components exposing the same seam (`bAutonomousDamageDecisions`, `CommandDetachPart`, `Get/ApplyPlasticState`, `Get/ApplyBrokenBeamBitset`, `GetDetachedPartIndices`, `GetRepairCounter`, `GetDamageChangeToken`, `IsDamageDirtySince`, `ReportImpact`, `RouteImpact`, `RepairVehicle`) |
| `GFrameCounter, UE_LOG, FMath, FMemory::Memcpy, FTCHARToUTF8, FSoftObjectPath::TryLoad` | pervasive | std equivalents; UTF-8 conv |
| Blueprint bindings (`BlueprintCallable/Assignable/Pure`, `TSubclassOf<APawn>`, `APawn/AActor`) | `OCNetSocketClient.h` | native API + your actor/pawn types |

**Already UE-free (reuse directly):** `OCSimCore` (`oc_sim.h/.cpp`), `OCMatchCore` (`oc_match.h/.cpp`), and the entire C# `OCServerHost`/`OCCoordinatorHost` including both `Wire.cs` protocol definitions. These are the intended shared interop cores and the definitive wire spec — port the UE client to speak them, do not reinvent them.