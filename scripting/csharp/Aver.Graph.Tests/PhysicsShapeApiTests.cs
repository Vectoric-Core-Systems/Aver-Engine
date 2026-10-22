// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Tests for the managed side of the shape and surface API (docs/plans/wave11/SHAPES_MATERIALS.md section 2.10) that needs no
// native library: the surface id, which Aver.Physics.Surfaces and Aver.Framework.Surface must compute identically (the
// id space a ray hit, a contact and a landscape sample share), and that name lookups degrade to "none" with no physics
// library beside this bare process instead of throwing.
using System;
using Aver.Framework;
using PhysSurfaceHit = Aver.Physics.SurfaceHit;
using PhysSurfaces = Aver.Physics.Surfaces;
using PhysTerrain = Aver.Physics.Terrain;

[AutoRun]
static class PhysicsShapeApiTests
{
    public static int RunAll()
    {
        int failures = 0;
        failures += TestGoldenSurfaceIds();
        failures += TestFrameworkAndPhysicsAgree();
        failures += TestNameLookupWithoutNativeLibrary();
        failures += TestDefaultHitIsAMiss();
        failures += TestTerrainArgumentGuards();
        failures += TestFrameworkSurfaceHelpersWithoutNativeLibrary();
        return failures;
    }

    private static int Fail(string msg)
    {
        Console.WriteLine($"  FAIL: {msg}");
        return 1;
    }

    private static int TestGoldenSurfaceIds()
    {
        Console.WriteLine("Test: surface ids match the golden values the native tests pin");
        (string name, int id)[] golden =
        {
            ("Ice", -714267872), ("Grass", 1479220765), ("Rubber", 1606840731), ("Wood", 81868168),
        };
        foreach (var (name, id) in golden)
            if (PhysSurfaces.IdOf(name) != id) return Fail($"IdOf(\"{name}\") = {PhysSurfaces.IdOf(name)}, expected {id}");
        if (PhysSurfaces.IdOf(null) != 0 || PhysSurfaces.IdOf("") != 0) return Fail("a null or empty name must be 0");
        return 0;
    }

    private static int TestFrameworkAndPhysicsAgree()
    {
        Console.WriteLine("Test: Surface.IdOf and Surfaces.IdOf agree on 50 names");
        var names = new System.Collections.Generic.List<string?> { null, "", " ", "ice", "Ice", "ICE", "caf\u00e9", "\u6c37", "\U0001F9CA", "a" };
        for (int i = 0; names.Count < 50; i++) names.Add($"Surface{i}_{i * 7919}");
        foreach (string? n in names)
            if (Surface.IdOf(n) != PhysSurfaces.IdOf(n)) return Fail($"'{n}': Framework {Surface.IdOf(n)} vs Physics {PhysSurfaces.IdOf(n)}");
        return 0;
    }

    private static int TestNameLookupWithoutNativeLibrary()
    {
        Console.WriteLine("Test: Surface.NameOf answers an empty string rather than throwing with no physics library");
        try
        {
            if (Surface.NameOf(0).Length != 0) return Fail("NameOf(0) must be empty");
            if (Surface.NameOf(PhysSurfaces.IdOf("Ice")).Length != 0) return Fail("an unregistered id must have an empty name");
        }
        catch (Exception e)
        {
            return Fail($"NameOf threw {e.GetType().Name}: {e.Message}");
        }
        return 0;
    }

    // Terrain's managed guards run BEFORE any native call, so a bad argument is answered the same with or without the library.
    private static int TestTerrainArgumentGuards()
    {
        Console.WriteLine("Test: Terrain refuses bad arguments before it reaches the native library");
        var none = Aver.Physics.CollisionShape.None;
        if (PhysTerrain.Heightfield(null!, 4, 4, 100f) != none) return Fail("null samples must give no shape");
        if (PhysTerrain.Heightfield(new float[15], 4, 4, 100f) != none) return Fail("fewer samples than cols * rows must give no shape");
        if (PhysTerrain.Heightfield(new float[16], 1, 16, 100f) != none) return Fail("one column must give no shape");
        if (PhysTerrain.Heightfield(new float[4], 16385, 1, 100f) != none) return Fail("more than 16384 columns must give no shape");
        if (PhysTerrain.Heightfield(new float[16], 4, 4, 100f, null, new int[257]) != none) return Fail("a palette of 257 must give no shape");
        if (PhysTerrain.Heightfield(new float[16], 4, 4, 100f, new byte[9], null) != none) return Fail("cell indices without a palette must give no shape");
        if (PhysTerrain.Heightfield(new float[16], 4, 4, 100f, new byte[8], new[] { 1 }) != none) return Fail("too few cell indices must give no shape");
        var body = new Aver.Physics.Body(1);
        if (PhysTerrain.SetHeights(body, 0, 0, 2, 2, new float[3])) return Fail("SetHeights with too few heights must be false");
        if (PhysTerrain.SetHeights(body, 0, 0, 0, 2, new float[4])) return Fail("SetHeights with a zero width must be false");
        if (PhysTerrain.GetHeights(body, 0, 0, 2, 2, new float[3])) return Fail("GetHeights into too small a buffer must be false");
        if (PhysTerrain.SetCellSurfaces(body, 0, 0, 2, 2, new byte[3], new[] { 1 })) return Fail("SetCellSurfaces with too few indices must be false");
        if (PhysTerrain.SetCellSurfaces(body, 0, 0, 1, 1, new byte[1], new int[257])) return Fail("SetCellSurfaces with 257 surfaces must be false");
        return 0;
    }

    // The Framework flavour of the surface helpers: pure ones work anywhere, native ones answer "none" instead of throwing.
    private static int TestFrameworkSurfaceHelpersWithoutNativeLibrary()
    {
        Console.WriteLine("Test: PhysicalSurfaces and the Entity surface members answer none/false with no native library");
        try
        {
            if (PhysicalSurfaces.Raycast(new Aver.Scene.Vec3(0, 0, 100), new Aver.Scene.Vec3(0, 0, -1), 500f).Hit) return Fail("a raycast hit with no physics world");
            if (PhysicalSurfaces.Beneath(Entity.None).Hit) return Fail("Beneath on Entity.None must be a miss");
            if (PhysicalSurfaces.Find("Nothing") != 0) return Fail("an unregistered name must not be found");
            if (Entity.None.PhysicalSurface != 0 || Entity.None.SetPhysicalSurface(5) || Entity.None.SetBodyScale(new Aver.Scene.Vec3(1, 1, 1)))
                return Fail("Entity.None must have no surface and refuse every body change");
        }
        catch (Exception e)
        {
            return Fail($"threw {e.GetType().Name}: {e.Message}");
        }
        return 0;
    }

    private static int TestDefaultHitIsAMiss()
    {
        Console.WriteLine("Test: a default SurfaceHit is a miss with no body");
        PhysSurfaceHit miss = default;
        if (miss.Hit || miss.Body.IsValid || miss.SurfaceId != 0) return Fail("default SurfaceHit must read as no hit");
        return 0;
    }
}
