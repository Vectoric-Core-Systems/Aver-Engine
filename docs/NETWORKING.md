# Networking: replication and RPCs

Part 1 of UE5 parity for networking (gap #1 in [plans/UE5_GAPS.md](plans/UE5_GAPS.md)). All of it is C# in `scripting/csharp/Aver.Framework/Net/`, namespace `Aver.Framework.Net`, over the reliable-UDP transport of `modules/net` (`Aver.Net.Abi`, see [ABI.md](ABI.md) section 13). No engine change, no new dependency.

## Pieces

| File | What |
|---|---|
| `NetTransport.cs` | `INetTransport` (listen, connect, poll, send on a reliable or an unreliable channel, graceful disconnect). `NativeNetTransport` drives `Aver.Net.Abi`; `LoopbackNetwork` / `LoopbackTransport` is the in-memory one (tests, optional unreliable loss). |
| `NetCodec.cs` | `NetWriter` / `NetReader` (little-endian, bounds-checked, `NetFormatException`), and the supported wire types: bool, the integer and float types, string, enums, `Vec3`, `Quat`, one-dimensional arrays of those. |
| `NetObject.cs` | `NetObject` base class, `[Replicated]`, `[Rpc]`, `[NetType]`, `NetTypes` (registry and reflection tables), `NetRelevance.Distance`. |
| `NetSession.cs` | `NetSession`: server and client roles, handshake, spawn / destroy, change detection and delta send, RPC routing, ownership, relevance, cleanup. |

## Model

* One `NetSession` per process end. `StartServer(ip, port, maxClients)` or `StartClient(ip, port, helloData)`; call `Tick()` once per frame (it polls the transport and, on a server, runs a replication pass every `SendIntervalMs`). Single game thread.
* The **server is the authority**. Only it spawns, destroys and changes ownership; clients hold proxies. `NetObject.HasAuthority` is true on the server; `IsOwner` is true on the owning client (and on the server for objects it owns itself, owner 0).
* **Handshake.** The client sends a hello (protocol version, free text such as a name or token). The server runs `ConnectionApproval` (return a reason to refuse), assigns a connection id (1, 2, ...) and replies; a refused client gets `Disconnected("rejected: <reason>")`. Nothing but a hello is accepted from an unauthenticated peer.
* **Objects.** Derive from `NetObject`, mark state `[Replicated]` and methods `[Rpc]`, give the class a `[NetType("name")]` and a public parameterless constructor, register with `NetTypes.Register<T>()` / `RegisterAssembly`. `session.Spawn<T>(ownerId)` on the server creates it and replicates it to every connection it is relevant to; the spawn message carries the full visible state, so a late joiner gets the live values. `Destroy` removes it everywhere. A game ties an object to an actor in `OnNetSpawn` / `OnNetDestroy` (create or find the entity there); the replication code itself needs no engine.
* **Properties.** Each pass the server serializes every `[Replicated]` member and compares it with the last bytes; a difference bumps that member's version. Each connection remembers the version it was last sent, so only changed members go out (one message per channel per object). `OnChanged = "Method"` runs on the client after a received change (and for the initial value), with the previous value if the method takes one. `OwnerOnly = true` sends the member only to the owner. `Reliable = false` puts it on the unreliable-ordered channel and resends it in full every `UnreliableResyncMs`, so a lost packet only delays convergence.
* **RPCs.** `CallRpc(nameof(Method), args...)` from a method on the object. `[Rpc(RpcTarget.Server)]` runs on the server; a client may call it only on an object it owns, and the **server rejects** any other sender (`RpcRejected` event, `Stats.RpcsRejected`, no effect). `RpcTarget.Client` runs on the owning client (on the server when the server owns it); `RpcTarget.Multicast` runs on the server and on every connection the object is spawned on. Client and Multicast calls from a client are refused locally. `Reliable = false` rides the unreliable channel. Arguments are the method's parameter types; an unknown method, bad arguments or a malformed packet is counted, never fatal.
* **Ownership.** `Spawn(type, ownerId)`, `SetOwner`. `OnOwnerChanged(previous)` runs on every side; the new owner is sent the owner-only state.
* **Relevance.** `session.RelevanceFilter` (null = everything to everyone). `NetRelevance.Distance(radius)` uses `NetObject.NetPosition` and `NetConnection.ViewPosition` (the game updates the latter from the player's pawn); owned and `AlwaysRelevant` objects always pass. Leaving relevance sends a destroy to that client only (the server object lives on, `ObjectDestroyed` fires on the client); entering sends a spawn with the current state. A Multicast skips clients the object is not relevant to.
* **Disconnect.** A leaving client's owned objects are destroyed everywhere (`DestroyOwnedOnDisconnect`, else they go to owner 0); `ClientDisconnected` fires. A client that loses the server (or whose server `Stop()`s) has all its proxies removed (`OnNetDestroy`, `ObjectDestroyed`) and gets `Disconnected(reason)`.

## Tests

`scripting/csharp/Aver.Framework.Tests/NetReplicationTests.cs`, run by `dotnet run` in that folder: the codec, then the same eight scenarios (handshake and refusal, spawn and delta, owner-only state and handover, unreliable properties, the three RPC targets, authority rejection, destroy / disconnect / server-stop cleanup, distance relevance) once on the in-memory network and once on real UDP over 127.0.0.1 through `Aver.Net.Abi`, plus unreliable-loss recovery and a garbage / stranger test on the in-memory network. The UDP half needs `Aver.Net.Abi.dll`: the test project copies it from `AVER_NET_BIN` or the repository's `build/bin`, and prints SKIP without it. The wire format is pinned only by these tests and `NetSession.ProtocolVersion` (1).

## Not done in part 1

* **Engine wiring.** Nothing in `Runtime` or the editor creates a `NetSession` or calls `Tick` yet; a game does it from a behaviour. Next: a `Game.Net` owner of the session ticked in the frame pass before scripts, a command-line / project-setting way to start as server or client, and an actor binding (a `NetObject` that owns an `Entity`, spawned by class name, with transform replication through `Entity.LocalPosition` and the PIE two-process path).
* **GameState / PlayerState**, `GameMode` on the server only, and `PlayerController` possession over the network.
* **Gas**: `IGasReplicationSink` / `IGasClientRequests` (see [GAMEPLAY_ABILITIES.md](GAMEPLAY_ABILITIES.md)) are still unconnected to a `NetObject`.
* **Fabric**: map Godot's `MultiplayerAPI` (`@rpc`, `MultiplayerSpawner`, `MultiplayerSynchronizer`) onto this layer, or keep Fabric's own NET2 and only share the transport; decide when the actor binding exists.
* Priority / bandwidth budgeting, quantized floats, property compression, join-in-progress snapshots larger than one packet, encryption, NAT punch, dedicated-server packaging. Fragmentation is the transport's (32 MiB packets).

## Part 2: client prediction and reconciliation (plan, not built)

Goal: the owning client moves its pawn immediately, the server stays authoritative, and the client corrects without visible snapping.

1. **Input as a command stream.** The owning client stamps each frame's input (move axes, look, buttons) with a monotonically increasing sequence number and sends the last N (default 3, for loss cover) on the unreliable channel in one packet; the server applies each sequence once, in order, and acks the highest applied.
2. **Fixed-step simulation.** Prediction needs the same step on both ends: the pawn's movement runs on the physics tick (`OnPhysicsTick`, fixed dt), one input per step. The server steps each client's commands at the same dt (buffering 1-2 commands to absorb jitter) and never runs a client ahead of the commands it has.
3. **Prediction.** The client applies each command locally at once and keeps a ring buffer of (sequence, input, resulting state) for the last ~1 s.
4. **Reconciliation.** The server replicates the owner's authoritative state plus `lastAppliedSequence` (an `OwnerOnly` property pair). On receipt the client drops buffered commands up to that sequence, compares the server state with its own stored state for that sequence, and if the error exceeds a tolerance rewinds to the server state and re-simulates the remaining commands. Small errors are blended out over a few frames (visual offset decay on the render transform, not the simulation); large ones snap.
5. **Determinism requirement.** Re-simulation needs the character movement step to be a pure function of (state, input, dt) with no dependence on frame time or unseeded randomness. Character-controller movement through Jolt's `CharacterVirtual` qualifies if stepped in isolation; rigid-body pawns do not (use server authority plus interpolation for those).
6. **Other clients: interpolation.** Proxies of objects the client does not own render ~100 ms in the past, interpolating between the two snapshots around that time (a snapshot buffer keyed by server tick; `Reliable=false` transform properties feed it); on a gap, short extrapolation, then hold.
7. **Server-side lag compensation** for hitscan: keep ~250 ms of historical hitboxes and rewind to the shooting client's view time (its render time minus its interpolation delay) when validating a shot.
8. **Tests (headless, deterministic).** Loopback transport with a delaying / dropping wrapper on a virtual clock: converge under 100 ms RTT and 5% unreliable loss, a forced server correction reconciles to the server state within a bounded frame count, no client ever ends ahead of the server, and replaying the same command log gives byte-identical state.

Open decision for part 2: whether movement runs through `Aver.Physics` characters (needs a state-snapshot and restore call that does not exist yet, see `PhysicsStateApi`) or a simple kinematic controller written for prediction first.
