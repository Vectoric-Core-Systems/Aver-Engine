// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Physical surfaces (physics_surface_abi.h): a name hashed to a 32-bit id, with the friction and restitution shapes made
// of it carry. The id is the one Aver.Framework.Surface.IdOf and the graph rows an_surfaceid / an_landscapesurface hand
// scripts, so a raycast, a contact and a landscape sample all answer in one id space.

using System.Text;

namespace Aver.Physics;

/// <summary>What the registry holds for one surface. <see cref="Friction"/> and <see cref="Restitution"/> are -1 when the
/// surface defers to the body's own number.</summary>
public readonly record struct SurfaceInfo(string Name, float Friction, float Restitution, int Tag);

/// <summary>The process-wide surface registry. Ids need no registering to be used on a shape; registering adds a name and
/// numbers. Between-step calls on the thread that steps the world.</summary>
public static class Surfaces
{
    private const int NameCap = 256;

    /// <summary>The 32-bit FNV-1a hash of the UTF-8 name, never 0 (a zero hash becomes 1); 0 for a null or empty name.
    /// Case-sensitive. Pure managed code, so it works with no native library; the same value as aver_phys_surface_id.</summary>
    public static int IdOf(string? name)
    {
        if (string.IsNullOrEmpty(name)) return 0;
        uint h = 2166136261u;
        foreach (byte b in Encoding.UTF8.GetBytes(name))
        {
            h ^= b;
            h *= 16777619u;
        }
        if (h == 0) h = 1;
        return unchecked((int)h);
    }

    /// <summary>Adds or updates a named surface and returns its id (0 when refused: an empty name, a non-finite number or a
    /// hash collision with a different name). Friction and restitution below 0 defer to the body; both clamp to 10.</summary>
    public static int Register(string name, float friction = -1, float restitution = -1, int tag = 0) =>
        string.IsNullOrEmpty(name) ? 0 : Native.aver_phys_surface_register(name, friction, restitution, tag);

    /// <summary>Changes the numbers of a surface that is registered or used by a shape. Live: every body on it takes the new
    /// value at its next contact evaluation. False for an id nothing has used.</summary>
    public static bool Tune(int surface, float friction, float restitution, int tag) =>
        Native.aver_phys_surface_tune(surface, friction, restitution, tag) != 0;

    /// <summary>The registry entry of <paramref name="surface"/>. A surface used only by a shape has an empty name and the
    /// numbers -1, -1, 0.</summary>
    public static bool TryGet(int surface, out SurfaceInfo info)
    {
        var buf = new byte[NameCap];
        if (Native.aver_phys_surface_info(surface, out float f, out float r, out int tag, buf, NameCap) == 0)
        {
            info = default;
            return false;
        }
        int len = Array.IndexOf(buf, (byte)0);
        info = new SurfaceInfo(Encoding.UTF8.GetString(buf, 0, len < 0 ? buf.Length : len), f, r, tag);
        return true;
    }

    /// <summary>The registered name of an id; an empty string when it has none.</summary>
    public static string NameOf(int surface) => TryGet(surface, out var info) ? info.Name : string.Empty;

    /// <summary>How many surfaces have a registered name.</summary>
    public static int Count => Native.aver_phys_surface_count();

    /// <summary>The id at a dense index in registration order; 0 out of range.</summary>
    public static int At(int index) => Native.aver_phys_surface_at(index);

    /// <summary>Forgets every name and returns every surface's numbers to unset. Shapes keep their materials and then defer
    /// to their bodies.</summary>
    public static void Reset() => Native.aver_phys_surface_reset();
}

public readonly partial struct Body
{
    /// <summary>The body's surface label (0 = none): identity for queries, not contact friction. Written by
    /// <see cref="SetSurface"/>; survives <see cref="SetEntity"/> and a static-to-movable rebuild.</summary>
    public int Surface
    {
        get
        {
            // -1 is the ABI's "dead handle" (and, 1 in 4 billion, a real hash): both read as no surface here.
            int s = Native.aver_phys_body_surface(Handle);
            return s == -1 ? 0 : s;
        }
    }

    /// <summary>Labels the body with <paramref name="surface"/> (0 clears it). <paramref name="copyPhysics"/> also copies the
    /// registered friction and restitution into the body once.</summary>
    public bool SetSurface(int surface, bool copyPhysics = false) =>
        Native.aver_phys_body_set_surface(Handle, surface, copyPhysics ? 1 : 0) != 0;
}
