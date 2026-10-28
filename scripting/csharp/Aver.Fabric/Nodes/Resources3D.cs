// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The resource types the 3D node stubs hold: meshes, materials and collision shapes. Values only; F2b gives them loaders.

namespace Aver.Fabric;

/// <summary>A mesh resource (Godot's Mesh). <see cref="Resource.ResourcePath"/> names the Aver mesh asset ("res://x.ocmesh" or a content path).</summary>
public class Mesh : Resource
{
}

public class ArrayMesh : Mesh
{
}

/// <summary>A box mesh (Godot's BoxMesh), size in metres.</summary>
public class BoxMesh : Mesh
{
    public Vector3 Size { get; set; } = Vector3.One;
}

public class SphereMesh : Mesh
{
    public float Radius { get; set; } = 0.5f;
    public float Height { get; set; } = 1f;
}

public class Material : Resource
{
    public int RenderPriority { get; set; }
}

public class BaseMaterial3D : Material
{
    public Color AlbedoColor { get; set; } = new(1, 1, 1, 1);
    public float Metallic { get; set; }
    public float Roughness { get; set; } = 1f;
}

public class StandardMaterial3D : BaseMaterial3D
{
}

public class Texture2D : Resource
{
}

/// <summary>A collision shape resource (Godot's Shape3D). Sizes are in metres.</summary>
public class Shape3D : Resource
{
    public float Margin { get; set; } = 0.04f;
}

public class BoxShape3D : Shape3D
{
    /// <summary>The full size of the box (not the half extents).</summary>
    public Vector3 Size { get; set; } = Vector3.One;
}

public class SphereShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
}

public class CapsuleShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
    /// <summary>The whole height, including both caps.</summary>
    public float Height { get; set; } = 2f;
}

public class CylinderShape3D : Shape3D
{
    public float Radius { get; set; } = 0.5f;
    public float Height { get; set; } = 2f;
}

public class WorldBoundaryShape3D : Shape3D
{
    public Plane Plane { get; set; } = new(0, 1, 0, 0);
}

public class ConvexPolygonShape3D : Shape3D
{
    public Vector3[] Points { get; set; } = System.Array.Empty<Vector3>();
}

public class ConcavePolygonShape3D : Shape3D
{
    public Vector3[] Data { get; set; } = System.Array.Empty<Vector3>();
}
