// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Blend shapes (morph targets), face poses and lip-sync from a script. Design: docs/plans/wave9/MORPH_TARGETS.md;
// guide: docs/ANIM_MORPH_FACE.md.
//
// RELAYED through the framework like montages and IK (aver_fw_morph_*): the framework links neither the animation
// module nor the renderer, so a host that installed no provider answers false from every call here rather than
// throwing. Weights set from script are TRANSIENT (never saved) and are released when Play stops. Target and pose names
// are the names inside the mesh and the face rig; FaceShapes / Visemes (FaceShapes.cs) spell the 52 ARKit shapes and
// the 15 visemes.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private static long MorphHash(string? name) => string.IsNullOrEmpty(name) ? 0 : Assets.ObjectIdOf(name);

    /// <summary>Sets a blend-shape weight by the target's name in the mesh (-4..4; 0 is neutral, 1 the full shape),
    /// ramping to it over <paramref name="blendSeconds"/> (0 = at once). A script weight wins over the animation until
    /// <see cref="ClearMorphWeights"/>. The first call on an entity with a mesh attaches Morph Targets for you. False
    /// when the entity is dead, has no mesh, or the host has no morph provider.</summary>
    public bool SetMorphWeight(string target, float weight, float blendSeconds = 0f) =>
        !string.IsNullOrEmpty(target) && FwMorph.aver_fw_morph_set_target(Handle, MorphHash(target), weight, blendSeconds) != 0;

    /// <summary>Sets a FACE POSE weight: a pose of the face rig, an ARKit shape name or a viseme name (see
    /// <see cref="FaceShapes"/> and <see cref="Visemes"/>). A pose expands to several blend shapes and, in a rig that says
    /// so, bone movement (a jaw).</summary>
    public bool SetFacePose(string pose, float weight, float blendSeconds = 0f) =>
        !string.IsNullOrEmpty(pose) && FwMorph.aver_fw_morph_set_pose(Handle, MorphHash(pose), weight, blendSeconds) != 0;

    /// <summary>Releases script weights: one target or pose by name, or (null) every script override, targets and poses
    /// alike. The animation takes over again, over <paramref name="blendSeconds"/>.</summary>
    public bool ClearMorphWeights(string? name = null, float blendSeconds = 0f) =>
        FwMorph.aver_fw_morph_clear(Handle, MorphHash(name), 3, blendSeconds) != 0;

    /// <summary>The FINAL weight of a blend shape this frame, after animation, script, lip-sync and the master weight
    /// (0 when it is not active). False when the entity has no morph state yet.</summary>
    public bool TryGetMorphWeight(string target, out float weight)
    {
        weight = 0f;
        return !string.IsNullOrEmpty(target) && FwMorph.aver_fw_morph_get_weight(Handle, MorphHash(target), out weight) != 0;
    }

    /// <summary>Chooses the face rig (.ocface) by asset path; null goes back to the built-in ARKit / viseme defaults.
    /// The entity needs Morph Targets already (add it in the editor, or call <see cref="SetMorphWeight"/> first).</summary>
    public bool SetFaceRig(string? rigAsset) => FwMorph.aver_fw_morph_set_rig(Handle, MorphHash(rigAsset)) != 0;

    /// <summary>The master scale of every blend shape on this entity (Morph Targets.weight; 0 reads as 1).</summary>
    public bool SetMorphMaster(float weight) => SetFloat("CMorphTargets.weight", weight);

    /// <summary>Plays an authored lip-sync track (.ocvis) at <paramref name="rate"/> from
    /// <paramref name="startSeconds"/>, scaled by <paramref name="strength"/>. It plays on top of the animation and ends
    /// by itself.</summary>
    public bool PlayLipSync(string trackAsset, float rate = 1f, float startSeconds = 0f, float strength = 1f) =>
        !string.IsNullOrEmpty(trackAsset) &&
        FwMorph.aver_fw_morph_lipsync_play(Handle, MorphHash(trackAsset), rate, startSeconds, strength) != 0;

    /// <summary>Stops lip-sync, fading the mouth out over <paramref name="blendSeconds"/>.</summary>
    public bool StopLipSync(float blendSeconds = 0.1f) => FwMorph.aver_fw_morph_lipsync_stop(Handle, blendSeconds) != 0;

    /// <summary>Slaves the lip-sync clock to an audio voice: call it every frame with the voice's playhead and the track
    /// follows the sound exactly (no drift). False when no track is playing.</summary>
    public bool SetLipSyncTime(float seconds) => FwMorph.aver_fw_morph_lipsync_set_time(Handle, seconds) != 0;

    /// <summary>Queues one viseme or phoneme (a name from <see cref="Visemes"/>, or an ARPAbet symbol such as "HH")
    /// to start <paramref name="startSeconds"/> from now and last <paramref name="durationSeconds"/>. For speech you
    /// get one word at a time (a text-to-speech result). False for a symbol that is neither.</summary>
    public bool QueueViseme(string symbol, float startSeconds, float durationSeconds, float intensity = 1f) =>
        !string.IsNullOrEmpty(symbol) &&
        FwMorph.aver_fw_morph_lipsync_queue(Handle, MorphHash(symbol), startSeconds, durationSeconds, intensity) != 0;

    /// <summary>The lip-sync clock (seconds), the track's length (0 = only queued visemes) and the strength. True while
    /// something is playing.</summary>
    public bool TryGetLipSyncState(out float time, out float length, out float strength)
    {
        var v = new float[3];
        bool playing = FwMorph.aver_fw_morph_lipsync_state(Handle, v) != 0;
        time = v[0]; length = v[1]; strength = v[2];
        return playing;
    }

    /// <summary>True while a lip-sync track or queued viseme is playing.</summary>
    public bool IsLipSyncPlaying => TryGetLipSyncState(out _, out _, out _);

    /// <summary>How many blend shapes the entity's mesh declares (0 until the renderer has seen the mesh, or when it has
    /// none).</summary>
    public int MorphTargetCount
    {
        get
        {
            var i = new long[3];
            var f = new float[1];
            return FwMorph.aver_fw_morph_info(Handle, i, f) != 0 ? (int)i[0] : 0;
        }
    }
}

/// <summary>The graph-node surface for the morph nodes: the same Entity calls with the int-entity / primitive-only
/// signatures GraphCompiler's emitted IL needs. An empty string means "not given".</summary>
internal static class MorphGraphInterop
{
    internal static bool SetMorphWeightForGraph(int entity, string target, float weight, float blendSeconds) =>
        new Entity(entity).SetMorphWeight(target, weight, blendSeconds);

    internal static bool SetFacePoseForGraph(int entity, string pose, float weight, float blendSeconds) =>
        new Entity(entity).SetFacePose(pose, weight, blendSeconds);

    internal static bool ClearMorphWeightsForGraph(int entity, string target, float blendSeconds) =>
        new Entity(entity).ClearMorphWeights(string.IsNullOrEmpty(target) ? null : target, blendSeconds);

    internal static bool GetMorphWeightForGraph(int entity, string target, out float weight) =>
        new Entity(entity).TryGetMorphWeight(target, out weight);

    internal static bool SetFaceRigForGraph(int entity, string rig) =>
        new Entity(entity).SetFaceRig(string.IsNullOrEmpty(rig) ? null : rig);

    internal static bool PlayLipSyncForGraph(int entity, string track, float rate, float startSeconds, float strength) =>
        new Entity(entity).PlayLipSync(track, rate, startSeconds, strength);

    internal static bool StopLipSyncForGraph(int entity, float blendSeconds) =>
        new Entity(entity).StopLipSync(blendSeconds);

    internal static bool QueueVisemeForGraph(int entity, string viseme, float startSeconds, float durationSeconds, float intensity) =>
        new Entity(entity).QueueViseme(viseme, startSeconds, durationSeconds, intensity);

    internal static bool GetLipSyncStateForGraph(int entity, out float time, out float length, out float strength) =>
        new Entity(entity).TryGetLipSyncState(out time, out length, out strength);
}
