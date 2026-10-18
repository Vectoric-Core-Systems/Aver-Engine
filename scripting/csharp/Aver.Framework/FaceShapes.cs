// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The 52 ARKit blend-shape names and the 15 viseme names, as constants. They are a NAMING CONVENTION only (no vendor code
// or data): a mesh whose blend shapes carry these names works with no face-rig asset at all, and the built-in rig turns
// the visemes into ARKit shapes for a mesh that has only those. The C++ twin is anim::kArkitShapes / kVisemeNames
// (modules/anim/include/aver/anim/FaceRig.hpp); the two lists are checked against each other by MorphNodeTests.

namespace Aver.Framework;

/// <summary>The 52 ARKit blend-shape names. Use with <c>Entity.SetMorphWeight</c> / <c>SetFacePose</c>.</summary>
public static class FaceShapes
{
    public const string BrowDownLeft = "browDownLeft";
    public const string BrowDownRight = "browDownRight";
    public const string BrowInnerUp = "browInnerUp";
    public const string BrowOuterUpLeft = "browOuterUpLeft";
    public const string BrowOuterUpRight = "browOuterUpRight";
    public const string CheekPuff = "cheekPuff";
    public const string CheekSquintLeft = "cheekSquintLeft";
    public const string CheekSquintRight = "cheekSquintRight";
    public const string EyeBlinkLeft = "eyeBlinkLeft";
    public const string EyeBlinkRight = "eyeBlinkRight";
    public const string EyeLookDownLeft = "eyeLookDownLeft";
    public const string EyeLookDownRight = "eyeLookDownRight";
    public const string EyeLookInLeft = "eyeLookInLeft";
    public const string EyeLookInRight = "eyeLookInRight";
    public const string EyeLookOutLeft = "eyeLookOutLeft";
    public const string EyeLookOutRight = "eyeLookOutRight";
    public const string EyeLookUpLeft = "eyeLookUpLeft";
    public const string EyeLookUpRight = "eyeLookUpRight";
    public const string EyeSquintLeft = "eyeSquintLeft";
    public const string EyeSquintRight = "eyeSquintRight";
    public const string EyeWideLeft = "eyeWideLeft";
    public const string EyeWideRight = "eyeWideRight";
    public const string JawForward = "jawForward";
    public const string JawLeft = "jawLeft";
    public const string JawOpen = "jawOpen";
    public const string JawRight = "jawRight";
    public const string MouthClose = "mouthClose";
    public const string MouthDimpleLeft = "mouthDimpleLeft";
    public const string MouthDimpleRight = "mouthDimpleRight";
    public const string MouthFrownLeft = "mouthFrownLeft";
    public const string MouthFrownRight = "mouthFrownRight";
    public const string MouthFunnel = "mouthFunnel";
    public const string MouthLeft = "mouthLeft";
    public const string MouthLowerDownLeft = "mouthLowerDownLeft";
    public const string MouthLowerDownRight = "mouthLowerDownRight";
    public const string MouthPressLeft = "mouthPressLeft";
    public const string MouthPressRight = "mouthPressRight";
    public const string MouthPucker = "mouthPucker";
    public const string MouthRight = "mouthRight";
    public const string MouthRollLower = "mouthRollLower";
    public const string MouthRollUpper = "mouthRollUpper";
    public const string MouthShrugLower = "mouthShrugLower";
    public const string MouthShrugUpper = "mouthShrugUpper";
    public const string MouthSmileLeft = "mouthSmileLeft";
    public const string MouthSmileRight = "mouthSmileRight";
    public const string MouthStretchLeft = "mouthStretchLeft";
    public const string MouthStretchRight = "mouthStretchRight";
    public const string MouthUpperUpLeft = "mouthUpperUpLeft";
    public const string MouthUpperUpRight = "mouthUpperUpRight";
    public const string NoseSneerLeft = "noseSneerLeft";
    public const string NoseSneerRight = "noseSneerRight";
    public const string TongueOut = "tongueOut";

    /// <summary>All 52, in ARKit's alphabetical order.</summary>
    public static readonly IReadOnlyList<string> All = new[]
    {
        BrowDownLeft, BrowDownRight, BrowInnerUp, BrowOuterUpLeft, BrowOuterUpRight, CheekPuff, CheekSquintLeft,
        CheekSquintRight, EyeBlinkLeft, EyeBlinkRight, EyeLookDownLeft, EyeLookDownRight, EyeLookInLeft, EyeLookInRight,
        EyeLookOutLeft, EyeLookOutRight, EyeLookUpLeft, EyeLookUpRight, EyeSquintLeft, EyeSquintRight, EyeWideLeft,
        EyeWideRight, JawForward, JawLeft, JawOpen, JawRight, MouthClose, MouthDimpleLeft, MouthDimpleRight,
        MouthFrownLeft, MouthFrownRight, MouthFunnel, MouthLeft, MouthLowerDownLeft, MouthLowerDownRight, MouthPressLeft,
        MouthPressRight, MouthPucker, MouthRight, MouthRollLower, MouthRollUpper, MouthShrugLower, MouthShrugUpper,
        MouthSmileLeft, MouthSmileRight, MouthStretchLeft, MouthStretchRight, MouthUpperUpLeft, MouthUpperUpRight,
        NoseSneerLeft, NoseSneerRight, TongueOut,
    };

    /// <summary>Every ARKit shape (the same list as <see cref="All"/>).</summary>
    public static IReadOnlyList<string> AllShapes => All;
}

/// <summary>The 15 viseme pose names. Use with <c>Entity.SetFacePose</c> or <c>QueueViseme</c> (the short form without
/// "viseme_" works there too).</summary>
public static class Visemes
{
    public const string Sil = "viseme_sil";
    public const string PP = "viseme_PP";
    public const string FF = "viseme_FF";
    public const string TH = "viseme_TH";
    public const string DD = "viseme_DD";
    public const string Kk = "viseme_kk";
    public const string CH = "viseme_CH";
    public const string SS = "viseme_SS";
    public const string Nn = "viseme_nn";
    public const string RR = "viseme_RR";
    public const string Aa = "viseme_aa";
    public const string E = "viseme_E";
    public const string Ih = "viseme_ih";
    public const string Oh = "viseme_oh";
    public const string Ou = "viseme_ou";

    /// <summary>All 15, in the engine's table order.</summary>
    public static readonly IReadOnlyList<string> All = new[]
    {
        Sil, PP, FF, TH, DD, Kk, CH, SS, Nn, RR, Aa, E, Ih, Oh, Ou,
    };
}
