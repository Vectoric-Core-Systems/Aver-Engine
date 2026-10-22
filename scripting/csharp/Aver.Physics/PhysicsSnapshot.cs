// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Snapshots of the whole physics simulation, exact stepping and the state hash (physics_state_abi.h). A snapshot holds what
// the simulation derives between inputs -- bodies, contacts, constraints, characters, vehicle driver-aid state, the fixed-step
// accumulator -- and restores all-or-nothing: a body, character, vehicle or joint that was removed since, or ANY constraint that
// was added or removed, refuses the restore and changes nothing. Inputs (pedals set this frame, forces, kinematic targets,
// tune values) are not state; a rollback layer re-issues them for each resimulated tick.
//
// A snapshot only restores into a world built the same way (handles change when a level reloads), and a single-precision
// snapshot is refused by a double-precision build. It is not a save file.

using System;

namespace Aver.Physics;

/// <summary>What a snapshot holds (AVER_PHYS_SNAP_*). OR them together.</summary>
[Flags]
public enum SnapshotParts : uint
{
    /// <summary>Dynamic and kinematic bodies, a soft body's vertices included.</summary>
    Bodies = 0x01,
    /// <summary>The contact cache: without it the next step re-detects contacts and loses its warm start.</summary>
    Contacts = 0x02,
    /// <summary>Joints (a broken one comes back unbroken or stays broken as it was), ragdolls, vehicle wheels, engine and gearbox.</summary>
    Constraints = 0x04,
    /// <summary>Character state, which Jolt's own save does not include.</summary>
    Characters = 0x08,
    /// <summary>The fixed-step accumulator, vehicle driver-aid state and a character's ground carry.</summary>
    Engine = 0x10,
    /// <summary>Also static bodies (off by default: about 48 bytes each).</summary>
    Statics = 0x20,
    /// <summary>Everything except statics.</summary>
    All = 0x1F,
}

/// <summary>How strictly a restore checks the world (AVER_PHYS_RESTORE_STRICT). The default, 0, allows bodies, characters,
/// vehicles and joints that were ADDED since the capture: they keep their current state.</summary>
[Flags]
public enum RestoreOptions : uint
{
    /// <summary>Also refuse when anything was added since the capture.</summary>
    Strict = 0x01,
}

/// <summary>The outcome of the last capture, restore or import (AVER_PHYS_RESTORE_*).</summary>
public enum RestoreResult
{
    Ok = 0,
    /// <summary>No such snapshot, or it belongs to a world that is gone.</summary>
    BadHandle = -1,
    /// <summary>Not a snapshot, or truncated.</summary>
    BadHeader = -2,
    /// <summary>Precision, Jolt version, state layout version or determinism build differ.</summary>
    BuildMismatch = -3,
    /// <summary>A captured body, character, vehicle, joint or ragdoll is gone, or the constraint set changed.</summary>
    Topology = -4,
    /// <summary>The payload does not match its checksum, or Jolt refused the stream.</summary>
    Corrupt = -5,
}

/// <summary>The slots of aver_phys_snapshot_info (AVER_PHYS_SI_*).</summary>
internal enum SnapshotInfoSlot
{
    Flags = 0,
    Bodies = 1,
    Characters = 2,
    Vehicles = 3,
    Joints = 4,
    Ragdolls = 5,
    TopologyEpoch = 6,
    ConstraintEpoch = 7,
    Precision = 8,
    StepsLow = 9,
    StepsHigh = 10,
    Bytes = 11,
    Count = 12,
}

/// <summary>What a snapshot holds, as counted when it was taken.</summary>
/// <param name="Parts">The parts held.</param>
/// <param name="Bodies">Bodies Jolt saved (a character's inner body included).</param>
/// <param name="Steps">Fixed steps since the world was created, at the capture.</param>
/// <param name="Bytes">The blob size.</param>
public readonly record struct SnapshotInfo(SnapshotParts Parts, int Bodies, int Characters, int Vehicles, int Joints, int Ragdolls,
                                           uint TopologyEpoch, uint ConstraintEpoch, PhysicsPrecision Precision, long Steps, int Bytes);

/// <summary>A captured physics state. Main thread, between steps. <see cref="Dispose"/> releases it; snapshots that are never
/// released die with the world (shutdown or rebuild). There is no finalizer: the native table is main-thread only.</summary>
public sealed class PhysicsSnapshot : IDisposable
{
    private int _handle;

    private PhysicsSnapshot(int handle) { _handle = handle; }

    /// <summary>Takes a snapshot of the running world. With <paramref name="reuse"/> that snapshot's buffer is overwritten in
    /// place (no allocation once warm) and it is returned; if its handle died with an earlier world it simply becomes a new
    /// one. Null (see <see cref="LastResult"/> and the physics error) when there is no world or a part is unknown.</summary>
    public static PhysicsSnapshot? Capture(SnapshotParts parts = SnapshotParts.All, PhysicsSnapshot? reuse = null)
    {
        int h = Native.aver_phys_snapshot_capture(reuse?._handle ?? 0, (uint)parts);
        if (h == 0)
        {
            if (reuse is not null) reuse._handle = 0;
            return null;
        }
        if (reuse is null) return new PhysicsSnapshot(h);
        reuse._handle = h;
        return reuse;
    }

    /// <summary>Validates and adopts a blob from <see cref="ToBytes"/> (a peer, a test). Null when it is not a snapshot of this
    /// build or fails its checksum; <see cref="LastResult"/> says which. Needs a running world.</summary>
    public static PhysicsSnapshot? FromBytes(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty) return null;
        int h = Native.aver_phys_snapshot_import(data.ToArray(), data.Length);
        return h == 0 ? null : new PhysicsSnapshot(h);
    }

    /// <summary>Why the last capture, restore or import failed (<see cref="RestoreResult.Ok"/> after a success).</summary>
    public static RestoreResult LastResult => (RestoreResult)Native.aver_phys_snapshot_last_result();

    /// <summary>How many snapshots are alive in the current world.</summary>
    public static int LiveCount => Native.aver_phys_snapshot_count();

    /// <summary>True while the snapshot is alive in the current world.</summary>
    public bool IsValid => _handle != 0 && Native.aver_phys_snapshot_bytes(_handle) > 0;

    /// <summary>Puts the world back as it was. False, with the reason in <see cref="LastResult"/>, and nothing changed, when the
    /// world no longer matches. On success the event queues are empty and <see cref="Physics.StepTotal"/> is the capture's.</summary>
    public bool Restore(RestoreOptions options = 0) => _handle != 0 && Native.aver_phys_snapshot_restore(_handle, (uint)options) != 0;

    /// <summary>FNV-1a 64 of the state (not of the capture time): two worlds in the same state have the same hash.</summary>
    public ulong Hash => _handle == 0 ? 0UL : Native.aver_phys_snapshot_hash(_handle);

    /// <summary>The blob size in bytes; 0 when dead.</summary>
    public int ByteCount => _handle == 0 ? 0 : Native.aver_phys_snapshot_bytes(_handle);

    /// <summary>A copy of the blob for transport. Empty when dead.</summary>
    public byte[] ToBytes()
    {
        if (_handle == 0) return Array.Empty<byte>();
        int n = Native.aver_phys_snapshot_read(_handle, null, 0);
        if (n <= 0) return Array.Empty<byte>();
        var buf = new byte[n];
        return Native.aver_phys_snapshot_read(_handle, buf, n) == n ? buf : Array.Empty<byte>();
    }

    /// <summary>What it holds.</summary>
    public SnapshotInfo Info
    {
        get
        {
            var v = new int[(int)SnapshotInfoSlot.Count];
            if (_handle == 0 || Native.aver_phys_snapshot_info(_handle, v, v.Length) == 0) return default;
            long steps = (long)(((ulong)(uint)v[(int)SnapshotInfoSlot.StepsHigh] << 32) | (uint)v[(int)SnapshotInfoSlot.StepsLow]);
            return new SnapshotInfo((SnapshotParts)(uint)v[(int)SnapshotInfoSlot.Flags], v[(int)SnapshotInfoSlot.Bodies],
                v[(int)SnapshotInfoSlot.Characters], v[(int)SnapshotInfoSlot.Vehicles], v[(int)SnapshotInfoSlot.Joints],
                v[(int)SnapshotInfoSlot.Ragdolls], (uint)v[(int)SnapshotInfoSlot.TopologyEpoch],
                (uint)v[(int)SnapshotInfoSlot.ConstraintEpoch], (PhysicsPrecision)v[(int)SnapshotInfoSlot.Precision],
                steps, v[(int)SnapshotInfoSlot.Bytes]);
        }
    }

    /// <summary>The offset of the first state byte at which two snapshots differ, -1 when they are equal, -2 when either is dead.
    /// Finding WHERE two runs diverge.</summary>
    public static int FirstDifference(PhysicsSnapshot a, PhysicsSnapshot b) => Native.aver_phys_snapshot_diff(a._handle, b._handle);

    /// <summary>Releases the native copy. Safe to call twice.</summary>
    public void Dispose()
    {
        if (_handle != 0) Native.aver_phys_snapshot_release(_handle);
        _handle = 0;
    }
}

/// <summary>A fixed-capacity ring of snapshots keyed by tick: the rollback helper. Buffers are allocated once and reused. After
/// <see cref="Rewind"/> the entries newer than the tick are dropped (that history never happened), and pushing a tick that is not
/// newer than the newest drops the stale entries the same way. Main thread.</summary>
public sealed class PhysicsStateRing
{
    private readonly PhysicsSnapshot?[] _snaps;
    private readonly long[] _ticks;
    private readonly SnapshotParts _parts;
    private int _head;     // index of the oldest entry
    private int _count;

    public PhysicsStateRing(int capacity, SnapshotParts parts = SnapshotParts.All)
    {
        if (capacity < 1) throw new ArgumentOutOfRangeException(nameof(capacity));
        _snaps = new PhysicsSnapshot?[capacity];
        _ticks = new long[capacity];
        _parts = parts;
    }

    /// <summary>How many ticks the ring can hold.</summary>
    public int Capacity => _snaps.Length;

    /// <summary>How many ticks it holds now.</summary>
    public int Count => _count;

    /// <summary>The oldest tick held; -1 when empty.</summary>
    public long OldestTick => _count == 0 ? -1 : _ticks[Slot(0)];

    /// <summary>The newest tick held; -1 when empty.</summary>
    public long NewestTick => _count == 0 ? -1 : _ticks[Slot(_count - 1)];

    private int Slot(int i) => (_head + i) % _snaps.Length;

    /// <summary>Captures the world as tick <paramref name="tick"/>. A failed capture (no world) leaves the ring without that tick.</summary>
    public void Push(long tick)
    {
        while (_count > 0 && _ticks[Slot(_count - 1)] >= tick) _count--;
        if (_count == _snaps.Length)
        {
            _head = (_head + 1) % _snaps.Length;   // the oldest is overwritten
            _count--;
        }
        int slot = Slot(_count);
        PhysicsSnapshot? snap = PhysicsSnapshot.Capture(_parts, _snaps[slot]);
        _snaps[slot] = snap;
        if (snap is null) return;
        _ticks[slot] = tick;
        _count++;
    }

    /// <summary>Restores the world to tick <paramref name="tick"/>, which must still be in the ring. False when it is not, or
    /// when the restore was refused (see <see cref="PhysicsSnapshot.LastResult"/>).</summary>
    public bool Rewind(long tick, RestoreOptions options = 0)
    {
        for (int i = _count - 1; i >= 0; i--)
        {
            int slot = Slot(i);
            if (_ticks[slot] != tick) continue;
            PhysicsSnapshot? snap = _snaps[slot];
            if (snap is null || !snap.Restore(options)) return false;
            _count = i + 1;
            return true;
        }
        return false;
    }

    /// <summary>Releases every snapshot in the ring.</summary>
    public void Clear()
    {
        for (int i = 0; i < _snaps.Length; i++)
        {
            _snaps[i]?.Dispose();
            _snaps[i] = null;
        }
        _head = 0;
        _count = 0;
    }
}

public static partial class Physics
{
    /// <summary>Runs exactly <paramref name="steps"/> fixed steps (1 to 600), ignoring the accumulator and leaving it as it was.
    /// Returns the steps run; 0 for a count outside the range or no world. The replay primitive.</summary>
    public static int StepExact(int steps) => Native.aver_phys_step_exact(steps);

    /// <summary>Releases every snapshot of the current world and returns how many there were (a level unload calls this).</summary>
    public static int ReleaseAllSnapshots() => Native.aver_phys_snapshot_release_all();

    /// <summary>Fixed steps since the world was created (a restore sets it back to the capture's); 0 with no world.</summary>
    public static long StepTotal => Native.aver_phys_step_total();

    /// <summary>The hash a snapshot taken now would have, without keeping it. 0 with no world.</summary>
    public static ulong StateHash(SnapshotParts parts = SnapshotParts.All) => Native.aver_phys_state_hash((uint)parts);

    /// <summary>Starts (a path) or stops (null) a log of one "step hash" line per fixed step, so two runs diff directly.
    /// Costs about a snapshot per step; a diagnostic.</summary>
    public static bool HashLog(string? path) => Native.aver_phys_hash_log(path) != 0;

    /// <summary>Seconds of real time carried toward the next fixed step; set it to restore a mid-frame position in a replay.</summary>
    public static float Accumulator
    {
        get => Native.aver_phys_accumulator_get();
        set => Native.aver_phys_accumulator_set(value);
    }

    // ---- The shape binary cache (physics_state_abi.h). A mesh or heightfield shape to bytes and back, so a host can keep the
    // BVH on disk. A restore is a miss (0), never a failure. ------------------------------------------------------------

    /// <summary>Bumped by any change to unit conversion, winding or the blob layout; part of every blob.</summary>
    public static uint ShapeCacheVersion => Native.aver_phys_shape_cache_version();

    /// <summary>Whether the host should use the shape cache (PHYSICS.SHAPECACHE, or AVER_PHYS_SHAPE_CACHE=1 for tools). Off by default.</summary>
    public static bool ShapeCacheEnabled
    {
        get => Native.aver_phys_shape_cache_enabled() != 0;
        set => Native.aver_phys_shape_cache_enable(value ? 1 : 0);
    }

    /// <summary>A square heightfield shape (row-major samples, centimetres) in the shape-handle table; 0 on failure.</summary>
    public static int CreateHeightfieldShape(float[] samples, int sampleCount, float spacingCm)
        => Native.aver_phys_create_heightfield_shape(samples, sampleCount, spacingCm);

    /// <summary>The cache blob for a mesh or heightfield shape under <paramref name="key"/>; null when the shape cannot be saved
    /// (not a mesh or heightfield, or it carries surface materials).</summary>
    public static byte[]? ShapeSave(int shape, ulong key)
    {
        int need = Native.aver_phys_shape_save(shape, key, null, 0);
        if (need <= 0) return null;
        var buf = new byte[need];
        return Native.aver_phys_shape_save(shape, key, buf, need) == need ? buf : null;
    }

    /// <summary>A shape handle rebuilt from a blob, or 0 for a miss (wrong key, version, build or precision, or a damaged blob).</summary>
    public static int ShapeRestore(byte[] blob, ulong key)
        => blob is null || blob.Length == 0 ? 0 : Native.aver_phys_shape_restore(blob, blob.Length, key);
}
