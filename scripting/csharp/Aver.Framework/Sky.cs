// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
namespace Aver.Framework;

/// <summary>
/// The sky's cloud layer, as a script sees it.
/// </summary>
/// <remarks>
/// <para>
/// A REQUEST, NOT THE TRUTH. Until something calls <see cref="SetClouds"/> the host renders the sky
/// the level authored, so a project that never touches this behaves exactly as it did before this
/// type existed. <see cref="Clear"/> hands the sky back.
/// </para>
/// <para>
/// This is the only route from a script to the sky. Nothing under the scripting or framework
/// modules mentioned PCG at all before it, so a project's sky could only ever be numbers typed into
/// its <c>.ocworld</c> — which is fine for a fixed sky and useless for one that is supposed to be
/// generated.
/// </para>
/// <para>
/// Lengths are CENTIMETRES and wind is centimetres per second, like everything else in this engine.
/// </para>
/// </remarks>
public static class Sky
{
    /// <summary>Publishes a cloud layer. Any later call replaces the whole request.</summary>
    /// <param name="seed">Which sky this is. Two seeds give different cloud fields from one set of
    /// settings; 0 is the unseeded field and reproduces the engine's original output exactly.</param>
    /// <param name="coverage">0 clear, 1 overcast. Clamped by the host.</param>
    /// <param name="density">Optical thickness of what is there. Negative is clamped to 0.</param>
    /// <param name="bottomCm">World Z of the layer's base.</param>
    /// <param name="topCm">World Z of its top.</param>
    /// <param name="featureScale">1 / the width of one noise feature, in world units.</param>
    /// <param name="windXCmPerSec">Drift along +X.</param>
    /// <param name="windYCmPerSec">Drift along +Y.</param>
    public static void SetClouds(int seed, float coverage, float density = 1.0f,
                                 float bottomCm = 150000.0f, float topCm = 280000.0f,
                                 float featureScale = 0.00002f,
                                 float windXCmPerSec = 900.0f, float windYCmPerSec = 260.0f)
        => Fw.aver_fw_set_sky_clouds(seed, coverage, density, bottomCm, topCm,
                                     featureScale, windXCmPerSec, windYCmPerSec);

    /// <summary>Drops the request, so the level's own sky takes over again.</summary>
    public static void Clear() => Fw.aver_fw_clear_sky_clouds();
}
