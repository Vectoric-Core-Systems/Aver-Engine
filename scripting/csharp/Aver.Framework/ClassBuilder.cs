// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// ClassBuilder: writes a gameplay class's archetype defaults, once per class at load.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Writes a class's defaults into its registry row. A class exposes its recipe as <c>static void Configure(ClassBuilder b)</c>.</summary>
public sealed class ClassBuilder
{
    private readonly int _c;
    internal ClassBuilder(int c) => _c = c;

    // Read back by the bridge after Configure runs, to bucket instances by tick group.
    internal bool WantsTick { get; private set; }
    internal int TickGroupId { get; private set; }

    /// <summary>Gives the class's own entity a mesh, and a material if one is named.</summary>
    public void Mesh(string meshPath, string material = "")
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CMeshRenderer);
        Fw.aver_fw_class_set_default_i64(_c, SceneIds.MeshMesh, Assets.ObjectIdOf(meshPath));
        if (!string.IsNullOrEmpty(material))
            Fw.aver_fw_class_set_default_i32(_c, SceneIds.MeshMaterial, SceneNative.aver_scene_material(0, material));
    }

    /// <summary>Adds a point light. Intensity in lux, range in centimetres.</summary>
    public void PointLight(float intensityLux, float rangeCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CLight);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CLight.intensityLux"), intensityLux);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CLight.rangeCm"), rangeCm);
    }

    /// <summary>Adds a camera. FoV authored in degrees (the field stores radians); clip planes in centimetres.</summary>
    public void Camera(float fovDegrees, float nearCm, float farCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CCamera);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.fovYRad"), fovDegrees * (MathF.PI / 180f));
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.nearCm"), nearCm);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.farCm"), farCm);
    }

    /// <summary>Declares that the class ticks, in <paramref name="group"/> at <paramref name="order"/>.</summary>
    public void Ticks(TickGroup group, int order = 0)
    {
        Fw.aver_fw_class_set_flags(_c, Fw.aver_fw_class_get_flags(_c) | ClassFlags.Ticks);
        Fw.aver_fw_class_set_tick(_c, (int)group, order);
        WantsTick = true;
        TickGroupId = (int)group;
    }
}
