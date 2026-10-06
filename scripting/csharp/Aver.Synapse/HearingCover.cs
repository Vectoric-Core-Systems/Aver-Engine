// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
namespace Aver.Synapse;

/// <summary>What a listener remembers of the strongest noise it heard.</summary>
public readonly record struct HeardNoise(float X, float Y, float Z, float Level, int Tag, float Confidence, float TimeSinceSec);

/// <summary>Noise events and listeners. Positions in centimetres.</summary>
public static class Hearing
{
    /// <summary>Makes the entity a listener.</summary>
    public static bool Attach(int entity) => Native.aver_syn_hearing_attach(entity) != 0;

    public static bool Configure(int entity, float sensitivity = 1f, float maxRangeCm = 5000f, float memorySec = 8f)
        => Native.aver_syn_hearing_configure(entity, sensitivity, maxRangeCm, memorySec) != 0;

    /// <summary>
    /// Raises a noise. <paramref name="loudnessCm"/> is how far it carries in the open to a listener of
    /// sensitivity 1; walls shorten that. <paramref name="tag"/> is yours (footstep, gunshot, ...).
    /// </summary>
    public static bool EmitNoise(float x, float y, float z, float loudnessCm, int tag = 0, int sourceEntity = 0)
        => Native.aver_syn_emit_noise(x, y, z, loudnessCm, tag, sourceEntity) != 0;

    /// <summary>The strongest live memory, if any. Confidence falls from 1 to 0 over the listener's memory time.</summary>
    public static bool TryGetHeard(int entity, out HeardNoise heard)
    {
        if (Native.aver_syn_hearing_get(entity, out float x, out float y, out float z, out float level,
                                        out int tag, out float confidence, out float since) == 0)
        {
            heard = default;
            return false;
        }
        heard = new HeardNoise(x, y, z, level, tag, confidence, since);
        return true;
    }

    public static bool Forget(int entity) => Native.aver_syn_hearing_forget(entity) != 0;
}

/// <summary>Cover points, searches and reservations.</summary>
public static class Cover
{
    /// <summary>Adds a cover point at (x, y); (dirX, dirY) points at the obstacle that protects it. Returns its id, 0 on failure.</summary>
    public static int Add(float x, float y, float dirX, float dirY, bool high = true, float arcHalfAngleDeg = 70f)
        => Native.aver_syn_cover_add(x, y, dirX, dirY, high ? 1 : 0, arcHalfAngleDeg);

    public static bool Remove(int coverId) => Native.aver_syn_cover_remove(coverId) != 0;

    /// <summary>Turns an entity into a cover marker: its position is where to stand, its +X axis points at the obstacle.</summary>
    public static bool AttachMarker(int entity, bool high = true, float arcHalfAngleDeg = 70f)
        => Native.aver_syn_cover_marker_attach(entity, high ? 1 : 0, arcHalfAngleDeg) != 0;

    /// <summary>Derives cover from the baked navigation grid's walls, in addition to authored points.</summary>
    public static void SetAutoGenerate(bool on, float minSpacingCm = 150f) => Native.aver_syn_cover_set_auto_generate(on ? 1 : 0, minSpacingCm);

    public static int Count => Native.aver_syn_cover_count();

    /// <summary>
    /// Finds and reserves the cheapest point that shields <paramref name="seeker"/> from the threat. No
    /// other seeker is offered the same point until it is released. False when nothing protects.
    /// </summary>
    public static bool Find(int seeker, float threatX, float threatY, float threatZ, out int coverId, out float x, out float y,
                            float maxSeekCm = 2500f, float minThreatDistCm = 300f, bool requireHigh = false)
    {
        coverId = Native.aver_syn_cover_find(seeker, threatX, threatY, threatZ, maxSeekCm, minThreatDistCm,
                                             requireHigh ? 1 : 0, out x, out y);
        return coverId != 0;
    }

    public static bool Release(int seeker) => Native.aver_syn_cover_release(seeker) != 0;

    /// <summary>True when the seeker stands on its reserved point and that point still shields it from the threat.</summary>
    public static bool IsCovered(int seeker, float threatX, float threatY, float threatZ)
        => Native.aver_syn_cover_is_covered(seeker, threatX, threatY, threatZ) != 0;
}

/// <summary>What a squad member is asked to do.</summary>
public enum SquadRole
{
    None = 0,
    /// <summary>Nearest the target; holds position.</summary>
    Anchor = 1,
    FlankLeft = 2,
    FlankRight = 3,
    /// <summary>Behind the anchor, spread out.</summary>
    Support = 4,
}

/// <summary>Squad membership, targets and the slots they produce.</summary>
public static class Squad
{
    public static bool Join(int entity, int squadId, float spacingCm = 200f) => Native.aver_syn_squad_attach(entity, squadId, spacingCm) != 0;

    /// <summary>Gives a squad something to act on; roles and slots appear once it has one.</summary>
    public static bool SetTarget(int squadId, float x, float y, float z) => Native.aver_syn_squad_set_target(squadId, x, y, z) != 0;

    public static bool ClearTarget(int squadId) => Native.aver_syn_squad_clear_target(squadId) != 0;

    /// <summary>This member's role and the point it should hold. False until the squad has a target.</summary>
    public static bool TryGetSlot(int entity, out SquadRole role, out float x, out float y, out float z)
    {
        bool ok = Native.aver_syn_squad_slot(entity, out int r, out x, out y, out z) != 0;
        role = ok ? (SquadRole)r : SquadRole.None;
        return ok;
    }

    /// <summary>The step that takes this member out of its squad-mates' spacing circles; zero when it has room.</summary>
    public static bool TryGetSpacingPush(int entity, out float dx, out float dy)
        => Native.aver_syn_squad_spacing_push(entity, out dx, out dy) != 0;
}
