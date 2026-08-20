// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Playing an animation from a script: the CAnimator and CSkeletalMesh surface on Entity.
//
// No new P/Invoke. Every call here is a field write over the generic scene ABI, exactly as SetMesh
// is, so neither the scene nor the framework seam grows a version.

using Aver.Scene;

namespace Aver.Framework;

public readonly partial struct Entity
{
    /// <summary>Binds a skeleton asset to this entity, so its mesh can be posed.</summary>
    public bool SetSkeleton(string skeletonAsset)
    {
        if (!HasComponent(Component.SkeletalMesh)) AddComponent(Component.SkeletalMesh);
        return SetInt64("CSkeletalMesh.skeleton", Assets.ObjectIdOf(skeletonAsset));
    }

    /// <summary>Plays a clip from the start, adding an animator if there is not one.</summary>
    public bool PlayAnimation(string clipAsset, bool loop = true)
    {
        if (!HasComponent(Component.Animator)) AddComponent(Component.Animator);
        int flags = GetInt("CAnimator.flags") & ~(AnimatorPaused | AnimatorOnce);
        if (!loop) flags |= AnimatorOnce;
        SetInt("CAnimator.flags", flags);
        SetFloat("CAnimator.time", 0.0f);
        return SetInt64("CAnimator.clip", Assets.ObjectIdOf(clipAsset));
    }

    /// <summary>Holds the clip where it is. The pose stays; Resume continues from the same instant.</summary>
    public bool PauseAnimation() => SetInt("CAnimator.flags", GetInt("CAnimator.flags") | AnimatorPaused);

    /// <summary>Continues a paused clip.</summary>
    public bool ResumeAnimation() => SetInt("CAnimator.flags", GetInt("CAnimator.flags") & ~AnimatorPaused);

    /// <summary>Stops the clip and rewinds it.</summary>
    public bool StopAnimation()
    {
        SetFloat("CAnimator.time", 0.0f);
        return PauseAnimation();
    }

    /// <summary>True while a clip is advancing.</summary>
    public bool IsAnimating =>
        HasComponent(Component.Animator) && (GetInt("CAnimator.flags") & AnimatorPaused) == 0;

    /// <summary>Seconds into the current clip. Settable, so a script can scrub or sync.</summary>
    public float AnimationTime
    {
        get => GetFloat("CAnimator.time");
        set => SetFloat("CAnimator.time", value);
    }

    /// <summary>Playback rate: 1 is normal, negative runs the clip backwards, 0 is read as 1.</summary>
    public float AnimationSpeed
    {
        get => GetFloat("CAnimator.speed");
        set => SetFloat("CAnimator.speed", value);
    }

    /// <summary>How much of the clip reaches the pose, 0..1. 0 is read as 1.</summary>
    public float AnimationWeight
    {
        get => GetFloat("CAnimator.blendWeight");
        set => SetFloat("CAnimator.blendWeight", value);
    }

    /// <summary>Bones in the bound skeleton, or 0 until the asset has resolved.</summary>
    public int BoneCount => GetInt("CSkeletalMesh.boneCount");

    /// <summary>Hangs this entity on a named socket of <paramref name="parent"/>'s rig, so it rides
    /// the posed bone every frame -- a weapon in a hand, a scabbard on a hip.
    ///
    /// PARENTING IS HALF OF IT, and deliberately the half that already existed: the parent link is
    /// what says WHO this rides, so this is SetParent plus a socket name rather than a private second
    /// notion of "attached to" that could drift from the hierarchy. Detaching is therefore two
    /// separate questions -- <see cref="DetachFromSocket"/> stops the ride and leaves the parenting,
    /// SetParent(default) unparents.
    ///
    /// THE SOCKET MUST EXIST ON THE PARENT'S SKELETON. A name that matches nothing leaves this entity
    /// exactly where it is rather than snapping it to the parent's origin: "it did not attach" is a
    /// diagnosable symptom, "it attached somewhere wrong" is not. Returns whether the fields were
    /// written, which is NOT whether the socket resolved -- the rig may not even be loaded yet.</summary>
    public bool AttachToSocket(Entity parent, string socket)
    {
        if (!SetParent(parent)) return false;
        if (!HasComponent(Component.Attachment)) AddComponent(Component.Attachment);
        // THE SAME HASH THE ENGINE USES. A component holds numbers, so the socket is named by
        // fnv1a64 exactly as a mesh is named by its asset ObjectId -- Assets.ObjectIdOf IS that
        // function, which is why this needs no new P/Invoke and no string crossing the boundary.
        return SetInt64("CAttachment.socket", Assets.ObjectIdOf(socket));
    }

    /// <summary>Reads a named float curve off the clip this entity is playing, at its current
    /// playhead -- "how far through the reload am I", "how hard is the foot planted".
    ///
    /// A CURVE IS NOT A NOTIFY. A notify is an event that happens once and runs something; a curve
    /// always has a value wherever the playhead is and runs nothing. Poll this from OnTick; do not
    /// try to express one with the other.
    ///
    /// Returns <paramref name="fallback"/> when the entity has no clip, the clip declares no such
    /// curve, or the host installed no animation system. Use <see cref="TryGetAnimationCurve"/>
    /// when the difference between "not there" and "reads zero" matters -- and it usually does.</summary>
    public float GetAnimationCurve(string curve, float fallback = 0.0f) =>
        TryGetAnimationCurve(curve, out float v) ? v : fallback;

    /// <summary>As <see cref="GetAnimationCurve"/>, but says whether the curve was there at all.
    /// <paramref name="value"/> is 0 on false, and that 0 means NOTHING -- a curve that genuinely
    /// holds zero returns true.</summary>
    public bool TryGetAnimationCurve(string curve, out float value)
    {
        value = 0.0f;
        if (string.IsNullOrEmpty(curve)) return false;
        // The same fnv1a64 the engine hashes a socket and an asset path with; no string crosses.
        return Fw.aver_fw_anim_curve(Handle, Assets.ObjectIdOf(curve), out value) != 0;
    }

    /// <summary>Stops riding a socket. The entity keeps its parent and stays where it last was.</summary>
    public bool DetachFromSocket() =>
        HasComponent(Component.Attachment) && SetInt64("CAttachment.socket", 0);

    /// <summary>The socket this entity is riding, as the fnv1a64 the component stores, or 0.
    /// Compare against <c>Assets.ObjectIdOf(name)</c> -- the name itself is not stored anywhere a
    /// component can reach.</summary>
    public long AttachedSocketId =>
        HasComponent(Component.Attachment) ? GetInt64("CAttachment.socket") : 0;

    // MIRRORS aver::scene::kAnimator* in Components.hpp. The flags are NEGATIVE deliberately: a
    // component added from a script arrives ZERO-FILLED because addComponent does not run member
    // initialisers, so zero has to mean playing and looping. A positive "playing" bit gave an
    // animator that attached perfectly and then did nothing, which is a bug this codebase has
    // already had once with CMeshRenderer's visible bit.
    private const int AnimatorPaused = 0x1;
    private const int AnimatorOnce = 0x2;
}
