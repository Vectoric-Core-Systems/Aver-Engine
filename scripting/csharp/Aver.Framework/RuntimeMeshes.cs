// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Meshes a script builds from arrays, and MultiMesh instance tables drawn as one mesh. Design: docs/plans/fabric/G4_RENDER.md R7.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The runtime-mesh and MultiMesh C ABI (framework_rtmesh_abi.h). Only this assembly has the DllImport resolver.</summary>
internal static class FwRtMesh
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_clear(int id);
    [DllImport(Lib)]
    internal static extern int aver_fw_rtmesh_add_surface(int id, int vertexCount, float[] pos, float[]? nrm, float[]? uv, float[]? col, float[]? uv1, float[]? tan,
                                                          int indexCount, uint[]? idx, [MarshalAs(UnmanagedType.LPUTF8Str)] string? material);
    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_surface_count(int id);
    [DllImport(Lib)] internal static extern int aver_fw_rtmesh_info(int id, int[] out4);

    [DllImport(Lib)] internal static extern int aver_fw_multimesh_create([MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_destroy(int id);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_source(int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string meshPath);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_flags(int id, int useColors, int useCustom);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_count(int id, int instanceCount);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_visible(int id, int visibleCount);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_transform(int id, int index, float[] t12);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_color(int id, int index, float r, float g, float b, float a);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_custom(int id, int index, float x, float y, float z, float w);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_set_buffer(int id, int count, float[]? transforms12, float[]? colors4, float[]? custom4);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_info(int id, int[] out4);
    [DllImport(Lib)] internal static extern int aver_fw_multimesh_mesh(int id);
}

/// <summary>Meshes built from arrays at run time. Everything is in engine space (centimetres, +Z up, the engine's triangle order); a layer
/// with another convention (an extension's array-mesh, surface-tool and immediate-mesh classes) converts before it calls. A mesh is drawn by an entity whose mesh is <see cref="PathOf"/> the key.</summary>
public static class RuntimeMeshes
{
    /// <summary>The asset path an entity's mesh renderer names for the runtime mesh <paramref name="key"/>: <c>@rtmesh/&lt;key&gt;</c>.</summary>
    public static string PathOf(string key) => "@rtmesh/" + key;

    /// <summary>Makes the mesh named <paramref name="key"/> (or returns the id it has). 0 when refused (bad key, or the engine's mesh budget is full).</summary>
    public static int Create(string key) => string.IsNullOrEmpty(key) ? 0 : FwRtMesh.aver_fw_rtmesh_create(key);

    public static bool Destroy(int id) => id > 0 && FwRtMesh.aver_fw_rtmesh_destroy(id) != 0;

    /// <summary>Removes every surface; the mesh keeps its key.</summary>
    public static bool Clear(int id) => id > 0 && FwRtMesh.aver_fw_rtmesh_clear(id) != 0;

    /// <summary>Appends a surface: <paramref name="positions"/> is 3 floats per vertex; the optional arrays are 3 (normals), 2 (UV), 4 (colour), 2 (second UV)
    /// and 4 (tangent xyz and handedness) floats per vertex; <paramref name="indices"/> is a triangle list (null draws the vertices in order). Returns the
    /// surface's index, or -1 when the engine refused it.</summary>
    public static int AddSurface(int id, float[] positions, float[]? normals, float[]? uv, float[]? colors, float[]? uv1, float[]? tangents, uint[]? indices, string? material)
    {
        if (id <= 0 || positions.Length < 3) return -1;
        return FwRtMesh.aver_fw_rtmesh_add_surface(id, positions.Length / 3, positions, normals, uv, colors, uv1, tangents, indices?.Length ?? 0, indices, material);
    }

    public static int SurfaceCount(int id) => id > 0 ? FwRtMesh.aver_fw_rtmesh_surface_count(id) : 0;

    /// <summary>Surfaces, vertices, triangles and the generation counter of a mesh; false for an unknown id.</summary>
    public static bool TryGetInfo(int id, out int surfaces, out int vertices, out int triangles, out int generation)
    {
        var buf = new int[4];
        bool ok = id > 0 && FwRtMesh.aver_fw_rtmesh_info(id, buf) != 0;
        surfaces = buf[0]; vertices = buf[1]; triangles = buf[2]; generation = buf[3];
        return ok;
    }
}

/// <summary>Instance tables drawn as one mesh (a multi-mesh). The engine bakes the first N instances of the source mesh into a runtime mesh whenever the
/// table changes (at most once a frame); <see cref="MeshOf"/> names it.</summary>
public static class MultiMeshes
{
    public static int Create(string key) => string.IsNullOrEmpty(key) ? 0 : FwRtMesh.aver_fw_multimesh_create(key);
    public static bool Destroy(int id) => id > 0 && FwRtMesh.aver_fw_multimesh_destroy(id) != 0;

    /// <summary>The mesh to instance: a content path ("Meshes/Rock.ocmesh") or a runtime mesh by its path (<see cref="RuntimeMeshes.PathOf"/>).</summary>
    public static bool SetSource(int id, string meshPath) => id > 0 && FwRtMesh.aver_fw_multimesh_set_source(id, meshPath ?? "") != 0;
    public static bool SetFlags(int id, bool useColors, bool useCustom) => id > 0 && FwRtMesh.aver_fw_multimesh_set_flags(id, useColors ? 1 : 0, useCustom ? 1 : 0) != 0;
    public static bool SetCount(int id, int count) => id > 0 && FwRtMesh.aver_fw_multimesh_set_count(id, count) != 0;

    /// <summary>How many instances are drawn (the first N); -1 draws all of them.</summary>
    public static bool SetVisible(int id, int count) => id > 0 && FwRtMesh.aver_fw_multimesh_set_visible(id, count) != 0;

    /// <summary>One instance's transform as 12 floats: the engine's row-vector world matrix with its 4th column dropped (three basis rows, then the translation).</summary>
    public static bool SetTransform(int id, int index, float[] t12) => id > 0 && t12.Length >= 12 && FwRtMesh.aver_fw_multimesh_set_transform(id, index, t12) != 0;
    public static bool SetColor(int id, int index, float r, float g, float b, float a) => id > 0 && FwRtMesh.aver_fw_multimesh_set_color(id, index, r, g, b, a) != 0;
    public static bool SetCustom(int id, int index, float x, float y, float z, float w) => id > 0 && FwRtMesh.aver_fw_multimesh_set_custom(id, index, x, y, z, w) != 0;

    /// <summary>The whole table at once; <paramref name="count"/> instances (the table is resized to it); colours and custom data may be null.</summary>
    public static bool SetBuffer(int id, int count, float[]? transforms12, float[]? colors4, float[]? custom4) =>
        id > 0 && FwRtMesh.aver_fw_multimesh_set_buffer(id, count, transforms12, colors4, custom4) != 0;

    /// <summary>The runtime mesh the MultiMesh bakes into (its path is <see cref="RuntimeMeshes.PathOf"/> of "multimesh/&lt;key&gt;"); 0 for an unknown id.</summary>
    public static int MeshOf(int id) => id > 0 ? FwRtMesh.aver_fw_multimesh_mesh(id) : 0;

    /// <summary>Instances, visible (requested), drawn (after the engine's vertex budget; 0 until the first bake) and the baked mesh's generation.</summary>
    public static bool TryGetInfo(int id, out int instances, out int visible, out int drawn, out int generation)
    {
        var buf = new int[4];
        bool ok = id > 0 && FwRtMesh.aver_fw_multimesh_info(id, buf) != 0;
        instances = buf[0]; visible = buf[1]; drawn = buf[2]; generation = buf[3];
        return ok;
    }
}
