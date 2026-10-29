// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G6 NET2: the replication half of SceneMultiplayer -- MultiplayerSpawner (spawn / despawn, late joiners, visibility) and MultiplayerSynchronizer (Always
// syncs on an interval, OnChange deltas, spawn-time state, visibility filters). See G6_NETWORK.md, NET2, for the packets and the rules.

namespace Aver.Fabric;

public partial class SceneMultiplayer
{
    private readonly List<MultiplayerSpawner> _spawners = new();
    private readonly List<MultiplayerSynchronizer> _syncs = new();

    internal void RegisterSpawner(MultiplayerSpawner s) { if (!_spawners.Contains(s)) _spawners.Add(s); }
    internal void UnregisterSpawner(MultiplayerSpawner s) => _spawners.Remove(s);
    internal void RegisterSynchronizer(MultiplayerSynchronizer s) { if (!_syncs.Contains(s)) _syncs.Add(s); }
    internal void UnregisterSynchronizer(MultiplayerSynchronizer s) => _syncs.Remove(s);

    public override Error ObjectConfigurationAdd(GodotObject? @object, Variant configuration)
    {
        switch (@object)
        {
            case MultiplayerSpawner sp: RegisterSpawner(sp); sp._api = this; return Error.Ok;
            case MultiplayerSynchronizer sy: RegisterSynchronizer(sy); sy._api = this; return Error.Ok;
            default: return Error.InvalidParameter;
        }
    }

    public override Error ObjectConfigurationRemove(GodotObject? @object, Variant configuration)
    {
        switch (@object)
        {
            case MultiplayerSpawner sp: UnregisterSpawner(sp); return Error.Ok;
            case MultiplayerSynchronizer sy: UnregisterSynchronizer(sy); return Error.Ok;
            default: return Error.InvalidParameter;
        }
    }

    // The peer set was replaced or emptied: nothing has been sent to anyone any more.
    private void ReplicationReset()
    {
        foreach (MultiplayerSpawner sp in _spawners) foreach (SpawnInfo i in sp.Local.Values) i.SentTo.Clear();
        foreach (MultiplayerSynchronizer s in _syncs) { s.Vis.Clear(); s.RecvSeq.Clear(); s.WasAuthority = false; }
    }

    private void ReplicationPeerRemoved(int id)
    {
        foreach (MultiplayerSpawner sp in _spawners) foreach (SpawnInfo i in sp.Local.Values) i.SentTo.Remove(id);
        foreach (MultiplayerSynchronizer s in _syncs) { s.Vis.Remove(id); s.RecvSeq.Remove(id); }
    }

    // A peer arrived: send it what already exists, then the synchronizers' state.
    private void ReplicationPeerAdmitted(int id)
    {
        foreach (MultiplayerSpawner sp in _spawners.ToArray())
            foreach (SpawnInfo info in sp.Local.Values.ToArray()) RefreshSpawn(info, id);
        foreach (MultiplayerSynchronizer s in _syncs.ToArray())
            if (IsAuthorityOf(s)) RefreshSyncVisibility(s, id);
    }

    private bool IsAuthorityOf(Node n) => n.GetMultiplayerAuthority() == _peer.GetUniqueId();

    // ---- the frame ----

    private void ReplicationTick(double dt)
    {
        if (_spawners.Count == 0 && _syncs.Count == 0) return;
        foreach (MultiplayerSpawner sp in _spawners.ToArray())
        {
            sp.EnsureBound();
            foreach (KeyValuePair<Node, SpawnInfo> kv in sp.Local.ToArray())
                if (!GodotObject.IsInstanceValid(kv.Key) || !kv.Key.IsInsideTree()) UntrackSpawn(sp, kv.Key);   // freed with its parent: no signal told us
        }
        foreach (MultiplayerSynchronizer s in _syncs.ToArray())
        {
            if (!s.IsAlive || !IsAuthorityOf(s)) { s.WasAuthority = false; continue; }
            if (!s.WasAuthority)
            {
                // this peer just became (or started as) the authority: nothing it knows has been sent
                s.WasAuthority = true;
                s.Vis.Clear();
                System.Array.Clear(s.LastWatch);
                s.SyncAccum = s.DeltaAccum = 0;
                RefreshSyncVisibility(s, 0);   // whatever the update mode, the first look is now
            }
            else if (s.VisibilityUpdateMode == MultiplayerSynchronizer.VisibilityUpdateModeEnum.Idle) RefreshSyncVisibility(s, 0);
            if (Due(ref s.SyncAccum, s.ReplicationInterval, dt)) SendSync(s);
            if (Due(ref s.DeltaAccum, s.DeltaInterval, dt)) SendDelta(s);
        }
        RefreshAllSpawnVisibility(physics: false);
    }

    private void ReplicationPhysicsTick()
    {
        foreach (MultiplayerSynchronizer s in _syncs.ToArray())
            if (s.IsAlive && IsAuthorityOf(s) && s.VisibilityUpdateMode == MultiplayerSynchronizer.VisibilityUpdateModeEnum.Physics) RefreshSyncVisibility(s, 0);
        RefreshAllSpawnVisibility(physics: true);
    }

    // Time accumulates by the frame's delta; an interval fires when it has passed (a lag spike makes one send, not a burst).
    private static bool Due(ref double accum, double interval, double dt)
    {
        accum += dt;
        if (interval <= 0) { accum = 0; return true; }
        if (accum < interval - 1e-9) return false;
        accum -= interval;
        if (accum >= interval) accum = 0;
        return true;
    }

    // ---- visibility ----

    // A script asked (update_visibility, set_visibility_for): looked at on any peer, so the signal and the spawn follow even where the synchronizer is not the authority's own.
    internal void RefreshVisibility(MultiplayerSynchronizer s, int forPeer)
    {
        RefreshSyncVisibility(s, forPeer);
        foreach (SpawnInfo info in SpawnsContaining(s))
            foreach (int p in forPeer == 0 ? _peers.ToArray() : new[] { forPeer }) RefreshSpawn(info, p);
    }

    // Re-evaluates the visibility of a synchronizer for one peer (0 = all) and, on a change, tells the script and (the authority) sends the peer the state it missed.
    private void RefreshSyncVisibility(MultiplayerSynchronizer s, int forPeer)
    {
        foreach (int p in forPeer == 0 ? _peers.ToArray() : new[] { forPeer })
        {
            if (!_peers.Contains(p)) continue;
            bool now = s.EvaluateVisibility(p);
            bool was = s.Vis.TryGetValue(p, out bool v) && v;
            if (now == was) continue;
            s.Vis[p] = now;
            s.EmitSignal(MultiplayerSynchronizer.SignalName.VisibilityChanged, (long)p);
            foreach (SpawnInfo info in SpawnsContaining(s)) RefreshSpawn(info, p);
            if (now && IsAuthorityOf(s)) SendFullState(s, p);
        }
    }

    private void RefreshAllSpawnVisibility(bool physics)
    {
        foreach (MultiplayerSpawner sp in _spawners.ToArray())
            foreach (SpawnInfo info in sp.Local.Values.ToArray())
            {
                List<MultiplayerSynchronizer> syncs = SynchronizersIn(info.Node);
                if (syncs.Count == 0) continue;
                bool applies = syncs.Any(x => x.VisibilityUpdateMode == (physics ? MultiplayerSynchronizer.VisibilityUpdateModeEnum.Physics : MultiplayerSynchronizer.VisibilityUpdateModeEnum.Idle));
                if (!applies) continue;
                foreach (int p in _peers.ToArray()) RefreshSpawn(info, p);
            }
    }

    private IEnumerable<SpawnInfo> SpawnsContaining(MultiplayerSynchronizer s)
    {
        foreach (MultiplayerSpawner sp in _spawners)
            foreach (SpawnInfo info in sp.Local.Values)
                if (info.Node == s || info.Node.IsAncestorOf(s)) yield return info;
    }

    // A spawned node is visible to a peer when every synchronizer in it is.
    private static bool SpawnVisibleTo(SpawnInfo info, int peer)
    {
        foreach (MultiplayerSynchronizer s in SynchronizersIn(info.Node)) if (!s.EvaluateVisibility(peer)) return false;
        return true;
    }

    private static List<MultiplayerSynchronizer> SynchronizersIn(Node root)
    {
        var found = new List<MultiplayerSynchronizer>();
        void Walk(Node n)
        {
            if (n is MultiplayerSynchronizer s) found.Add(s);
            foreach (Node c in n.ChildrenSnapshot()) Walk(c);
        }
        Walk(root);
        return found;
    }

    // ---- spawn: the authority ----

    internal bool TrackSpawn(MultiplayerSpawner spawner, Node node, int sceneIndex, Variant data)
    {
        if (spawner.SpawnLimit > 0 && spawner.Local.Count >= spawner.SpawnLimit)
        {
            Aver.Scripting.Log.Error($"[Fabric] MultiplayerSpawner: the spawn limit ({spawner.SpawnLimit}) is reached; '{node.Name}' stays local and is not replicated");
            return false;
        }
        var info = new SpawnInfo { Spawner = spawner, Node = node, NetId = spawner.NextNetId++, SceneIndex = sceneIndex, Data = data };
        spawner.Local[node] = info;
        foreach (int p in _peers.ToArray()) RefreshSpawn(info, p);
        return true;
    }

    internal void UntrackSpawn(MultiplayerSpawner spawner, Node node)
    {
        if (!spawner.Local.Remove(node, out SpawnInfo? info)) return;
        foreach (int p in info.SentTo.ToArray()) if (_peers.Contains(p)) SendDespawn(info, p);
        info.SentTo.Clear();
    }

    // Brings a peer in line with what it should have: the node when it is visible to it and it has none, no node when it is not.
    private void RefreshSpawn(SpawnInfo info, int peer)
    {
        if (!_peers.Contains(peer)) return;
        bool want = SpawnVisibleTo(info, peer);
        bool has = info.SentTo.Contains(peer);
        if (want && !has) { if (SendSpawn(info, peer)) info.SentTo.Add(peer); }
        else if (!want && has) { SendDespawn(info, peer); info.SentTo.Remove(peer); }
    }

    private bool SendSpawn(SpawnInfo info, int peer)
    {
        NetWriter? w = BeginNodePacket(CmdSpawn, info.Spawner, peer);
        if (w == null) return false;
        w.U32(info.NetId).I32(info.SceneIndex).Str(info.Node.Name);
        if (w.Var(info.Data) != Error.Ok) { Aver.Scripting.Log.Error("[Fabric] MultiplayerSpawner: the spawn data cannot be sent"); return false; }
        List<MultiplayerSynchronizer> syncs = SynchronizersIn(info.Node);
        w.U16(syncs.Count);
        foreach (MultiplayerSynchronizer s in syncs)
        {
            int[] idx = s.ReplicationConfig == null ? System.Array.Empty<int>() : s.SpawnIndices().ToArray();
            w.U16(idx.Length);
            foreach (int i in idx)
            {
                s.ReadProperty(i, out Variant val);
                if (w.Var(val) != Error.Ok) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSynchronizer: the spawn value of '{s.ReplicationConfig!.Props[i].Path}' cannot be sent"); w.Var(default); }
            }
        }
        return SendRaw(peer, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0) == Error.Ok;
    }

    private void SendDespawn(SpawnInfo info, int peer)
    {
        NetWriter? w = BeginNodePacket(CmdDespawn, info.Spawner, peer);
        if (w == null) return;
        w.U32(info.NetId);
        SendRaw(peer, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
    }

    // ---- spawn: the other peers ----

    private void HandleSpawn(int from, int cmdByte, NetReader r)
    {
        Node? target = ReadNodeRef(from, cmdByte, r, quiet: false);
        uint netId = r.U32();
        int sceneIndex = r.I32();
        string name = r.Str();
        Variant data = r.Var(AllowObjectDecoding);
        int groups = r.U16();
        var states = new List<Variant[]>();
        for (int g = 0; g < groups; g++)
        {
            int n = r.U16();
            if (n > r.Remaining) throw new FormatException("spawn state");
            var vals = new Variant[n];
            for (int i = 0; i < n; i++) vals[i] = r.Var(AllowObjectDecoding);
            states.Add(vals);
        }
        if (target is not MultiplayerSpawner spawner)
        {
            if (target != null) Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} spawned through '{target.GetPath()}', which is not a MultiplayerSpawner");
            return;
        }
        if (from != spawner.GetMultiplayerAuthority())
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} is not the authority of the spawner '{spawner.GetPath()}' and cannot spawn through it");
            return;
        }
        Node? parent = spawner.SpawnNode();
        if (parent == null) { Aver.Scripting.Log.Error($"[Fabric] multiplayer: the spawner '{spawner.GetPath()}' has no valid spawn_path"); return; }
        if (spawner.Remote.TryGetValue(netId, out Node? existing) && GodotObject.IsInstanceValid(existing)) return;   // a duplicate
        if (spawner.SpawnLimit > 0 && spawner.Remote.Count(kv => GodotObject.IsInstanceValid(kv.Value)) >= spawner.SpawnLimit)
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: the spawn limit of '{spawner.GetPath()}' ({spawner.SpawnLimit}) is reached");
            return;
        }
        Node? node = MakeSpawned(spawner, sceneIndex, data);
        if (node == null) return;
        node.AdoptName(name);
        List<MultiplayerSynchronizer> syncs = SynchronizersIn(node);
        for (int g = 0; g < states.Count && g < syncs.Count; g++)
        {
            int[] idx = syncs[g].ReplicationConfig == null ? System.Array.Empty<int>() : syncs[g].SpawnIndices().ToArray();
            for (int i = 0; i < idx.Length && i < states[g].Length; i++) syncs[g].WriteProperty(idx[i], states[g][i]);
        }
        if (states.Count != syncs.Count)
            Aver.Scripting.Log.Warn($"[Fabric] multiplayer: '{name}' spawned with {states.Count} synchronizer state(s) and {syncs.Count} synchronizer(s) here; the scenes differ between peers");
        spawner.Remote[netId] = node;
        parent.AddChild(node);
        spawner.EmitSignal(MultiplayerSpawner.SignalName.Spawned, node);
    }

    private static Node? MakeSpawned(MultiplayerSpawner spawner, int sceneIndex, Variant data)
    {
        if (sceneIndex >= 0)
        {
            string path = spawner.GetSpawnableScene(sceneIndex);
            if (path.Length == 0) { Aver.Scripting.Log.Error($"[Fabric] multiplayer: the spawn names scene #{sceneIndex}, which '{spawner.GetPath()}' does not list"); return null; }
            if (ResourceLoader.Load(path, typeof(PackedScene)) is not PackedScene scene || !scene.CanInstantiate())
            {
                Aver.Scripting.Log.Error($"[Fabric] multiplayer: the spawnable scene '{path}' cannot be loaded");
                return null;
            }
            return scene.Instantiate();
        }
        if (!spawner.SpawnFunction.IsValid()) { Aver.Scripting.Log.Error($"[Fabric] multiplayer: a custom spawn arrived and '{spawner.GetPath()}' has no spawn_function"); return null; }
        try
        {
            Node? n = spawner.SpawnFunction.Call(data).AsGodotObject() as Node;
            if (n == null || n.GetParent() != null) Aver.Scripting.Log.Error("[Fabric] multiplayer: spawn_function must return a new node that is not in the tree");
            return n is { } ok && ok.GetParent() == null ? ok : null;
        }
        catch (Exception ex)
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: spawn_function threw: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private void HandleDespawn(int from, int cmdByte, NetReader r)
    {
        Node? target = ReadNodeRef(from, cmdByte, r, quiet: false);
        uint netId = r.U32();
        if (target is not MultiplayerSpawner spawner || from != spawner.GetMultiplayerAuthority()) return;
        if (!spawner.Remote.Remove(netId, out Node? node)) return;
        if (!GodotObject.IsInstanceValid(node)) return;
        spawner.EmitSignal(MultiplayerSpawner.SignalName.Despawned, node);
        node.QueueFree();
    }

    // ---- synchronizers: sending ----

    private IEnumerable<int> VisiblePeers(MultiplayerSynchronizer s) => _peers.Where(p => s.Vis.TryGetValue(p, out bool v) && v);

    // Sends to the peers the synchronizer is visible to: one broadcast when that is everyone, else one packet each.
    private void SendToVisible(MultiplayerSynchronizer s, byte[] packet, MultiplayerPeer.TransferModeEnum mode)
    {
        int[] to = VisiblePeers(s).ToArray();
        if (to.Length == 0) return;
        if (to.Length == _peers.Count) { SendRaw(0, packet, mode, 0); return; }
        foreach (int p in to) SendRaw(p, packet, mode, 0);
    }

    // The path is included until every receiver confirmed it. One receiver, or everyone, has a clear audience; a subset gets a packet that always carries the path.
    private NetWriter? BeginSyncPacket(int cmd, MultiplayerSynchronizer s, int[] to)
    {
        if (to.Length == _peers.Count) return BeginNodePacket(cmd, s, 0);
        if (to.Length == 1) return BeginNodePacket(cmd, s, to[0]);
        return ForceFullRef(cmd, s);
    }

    private void SendSync(MultiplayerSynchronizer s)
    {
        if (s.ReplicationConfig == null) return;
        int[] always = s.AlwaysIndices().ToArray();
        if (always.Length == 0) return;
        int[] to = VisiblePeers(s).ToArray();
        if (to.Length == 0) return;
        NetWriter? w = BeginSyncPacket(CmdSync, s, to);
        if (w == null) return;
        w.U32(++s.SyncSeq);
        foreach (int i in always)
        {
            s.ReadProperty(i, out Variant v);
            if (w.Var(v) != Error.Ok) { Aver.Scripting.Log.Error($"[Fabric] MultiplayerSynchronizer: '{s.ReplicationConfig.Props[i].Path}' cannot be encoded"); return; }
        }
        if (w.B.Count > MaxSyncPacketSize)
        {
            if (!s.SizeWarned) Aver.Scripting.Log.Error($"[Fabric] MultiplayerSynchronizer '{s.Name}': the sync state is {w.B.Count} bytes, over max_sync_packet_size ({MaxSyncPacketSize}); nothing is sent");
            s.SizeWarned = true;
            return;
        }
        s.SizeWarned = false;
        SendToVisible(s, w.ToArray(), MultiplayerPeer.TransferModeEnum.Unreliable);
    }

    // A reference that always carries the path (for a packet that goes to peers in different cache states).
    private NetWriter? ForceFullRef(int cmd, MultiplayerSynchronizer s)
    {
        string? rel = RelativePath(s);
        if (rel == null) return null;
        if (!_nodeIds.TryGetValue(s, out uint id)) _nodeIds[s] = id = _nextNodeId++;
        return new NetWriter().U8(cmd | PathFlag).U32(id).Str(rel);
    }

    private static byte[] Encoded(Variant v)
    {
        var l = new List<byte>();
        VariantBinary.Encode(v, l);
        return l.ToArray();
    }

    private void SendDelta(MultiplayerSynchronizer s)
    {
        if (s.ReplicationConfig == null) return;
        int[] watch = s.WatchIndices().ToArray();
        if (watch.Length == 0) return;
        if (s.LastWatch.Length != s.ReplicationConfig.Props.Count) s.LastWatch = new byte[]?[s.ReplicationConfig.Props.Count];
        var changed = new List<(int Index, Variant Value, byte[] Bytes)>();
        foreach (int i in watch)
        {
            if (!s.ReadProperty(i, out Variant v)) continue;
            byte[] now = Encoded(v);
            if (s.LastWatch[i] == null || !s.LastWatch[i]!.AsSpan().SequenceEqual(now)) changed.Add((i, v, now));
        }
        if (changed.Count == 0) return;
        int[] to = VisiblePeers(s).ToArray();
        if (to.Length > 0)
        {
            NetWriter? w = BeginSyncPacket(CmdDelta, s, to);
            if (w == null) return;
            w.U16(changed.Count);
            foreach ((int idx, Variant val, _) in changed) { w.U16(idx); w.Var(val); }
            if (w.B.Count > MaxDeltaPacketSize)
            {
                Aver.Scripting.Log.Error($"[Fabric] MultiplayerSynchronizer '{s.Name}': the delta is {w.B.Count} bytes, over max_delta_packet_size ({MaxDeltaPacketSize}); nothing is sent");
                return;
            }
            SendToVisible(s, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable);
        }
        // with nobody to tell, the values stay unsent so the first peer to appear gets them
        if (to.Length > 0) foreach ((int idx, _, byte[] bytes) in changed) s.LastWatch[idx] = bytes;
    }

    // A peer that has just become able to see the synchronizer gets every watched value and one Always sync, not a wait for the next change.
    private void SendFullState(MultiplayerSynchronizer s, int peer)
    {
        if (s.ReplicationConfig == null) return;
        int[] watch = s.WatchIndices().ToArray();
        if (watch.Length > 0)
        {
            NetWriter? w = ForceFullRef(CmdDelta, s);
            if (w != null)
            {
                var vals = new List<(int, Variant)>();
                foreach (int i in watch) if (s.ReadProperty(i, out Variant v)) vals.Add((i, v));
                w.U16(vals.Count);
                foreach ((int idx, Variant val) in vals) { w.U16(idx); w.Var(val); }
                if (w.B.Count <= MaxDeltaPacketSize) SendRaw(peer, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
            }
        }
        int[] always = s.AlwaysIndices().ToArray();
        if (always.Length > 0)
        {
            NetWriter? w = ForceFullRef(CmdSync, s);
            if (w == null) return;
            w.U32(++s.SyncSeq);
            foreach (int i in always) { s.ReadProperty(i, out Variant v); w.Var(v); }
            if (w.B.Count <= MaxSyncPacketSize) SendRaw(peer, w.ToArray(), MultiplayerPeer.TransferModeEnum.Reliable, 0);
        }
    }

    // ---- synchronizers: receiving ----

    private void HandleSync(int from, int cmdByte, NetReader r)
    {
        if (ReadNodeRef(from, cmdByte, r, quiet: true) is not MultiplayerSynchronizer s) return;
        if (from != s.GetMultiplayerAuthority())
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} is not the authority of the synchronizer '{s.GetPath()}'; its state was dropped");
            return;
        }
        uint seq = r.U32();
        if (s.RecvSeq.TryGetValue(from, out uint last) && (int)(seq - last) <= 0) return;   // an old unreliable packet that arrived late
        int[] always = s.AlwaysIndices().ToArray();
        var vals = new Variant[always.Length];
        for (int i = 0; i < always.Length; i++) vals[i] = r.Var(AllowObjectDecoding);
        s.RecvSeq[from] = seq;
        for (int i = 0; i < always.Length; i++) s.WriteProperty(always[i], vals[i]);
        s.EmitSignal(MultiplayerSynchronizer.SignalName.Synchronized);
    }

    private void HandleDelta(int from, int cmdByte, NetReader r)
    {
        if (ReadNodeRef(from, cmdByte, r, quiet: true) is not MultiplayerSynchronizer s) return;
        if (from != s.GetMultiplayerAuthority())
        {
            Aver.Scripting.Log.Error($"[Fabric] multiplayer: peer {from} is not the authority of the synchronizer '{s.GetPath()}'; its delta was dropped");
            return;
        }
        int n = r.U16();
        if (n > r.Remaining) throw new FormatException("delta count");
        int count = s.ReplicationConfig?.Props.Count ?? 0;
        var got = new List<(int, Variant)>();
        for (int i = 0; i < n; i++)
        {
            int idx = r.U16();
            Variant v = r.Var(AllowObjectDecoding);
            if (idx < count && s.ReplicationConfig!.Props[idx].Mode == SceneReplicationConfig.ReplicationMode.OnChange) got.Add((idx, v));
        }
        foreach ((int idx, Variant v) in got) s.WriteProperty(idx, v);
        s.EmitSignal(MultiplayerSynchronizer.SignalName.DeltaSynchronized);
    }
}
