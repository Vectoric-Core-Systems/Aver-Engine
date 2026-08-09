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

    // MIRRORS aver::scene::kAnimator* in Components.hpp. The flags are NEGATIVE deliberately: a
    // component added from a script arrives ZERO-FILLED because addComponent does not run member
    // initialisers, so zero has to mean playing and looping. A positive "playing" bit gave an
    // animator that attached perfectly and then did nothing, which is a bug this codebase has
    // already had once with CMeshRenderer's visible bit.
    private const int AnimatorPaused = 0x1;
    private const int AnimatorOnce = 0x2;
}
