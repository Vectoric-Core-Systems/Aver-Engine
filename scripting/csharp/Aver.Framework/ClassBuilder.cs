using Aver.Scene;

namespace Aver.Framework;

/// <summary>
/// Writes a class's DEFAULTS — the archetype recipe run once per class at load, never per instance. Every
/// call is a registry write against the sealed-later class row; nothing is instanced here. A class exposes
/// its recipe through a <c>static void Configure(ClassBuilder b)</c> the loader calls after declaring the
/// class and before sealing it.
/// </summary>
/// <remarks>
/// <para>
/// This is the hand-written half of a class's shape: the components it always has and the tick group it
/// runs in. It is the sibling of <see cref="ActorBuilder"/>, which writes the editor-owned model tree. A
/// component you add here lives on the actor's OWN entity; a model placed through the builder is a child
/// entity. A simple static-mesh actor uses <see cref="Mesh"/> and no models; a rich actor uses models and
/// leaves its own entity mesh-less.
/// </para>
/// <para>
/// <b>The mesh/material kinds are the built kinds (contradiction #2, resolved).</b> <c>CMeshRenderer.mesh</c>
/// is a <c>u64</c> ObjectId (field kind I64) and <c>material</c> is an <c>i32</c> opaque handle (I32). So
/// the mesh PATH is hashed to its ObjectId in managed code (<see cref="Assets.ObjectIdOf"/>) and written
/// through <c>set_default_i64</c>, and the material NAME is resolved to its i32 handle through
/// <c>aver_scene_material</c> and written through <c>set_default_i32</c>. Neither is ever written as a
/// string — a <c>set_default_str</c> into either field would be rejected by the ABI's kind check.
/// </para>
/// </remarks>
public sealed class ClassBuilder
{
    private readonly int _c;
    internal ClassBuilder(int c) => _c = c;

    /// <summary>
    /// Give the class's own entity a mesh. <paramref name="meshPath"/> is hashed to the I64 ObjectId the
    /// field stores; <paramref name="material"/>, if given, is resolved to its I32 handle. An empty
    /// material leaves the field at 0 (== unset), which the renderer reads as "default material".
    /// </summary>
    public void Mesh(string meshPath, string material = "")
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CMeshRenderer);
        Fw.aver_fw_class_set_default_i64(_c, SceneIds.MeshMesh, Assets.ObjectIdOf(meshPath));
        if (!string.IsNullOrEmpty(material))
            Fw.aver_fw_class_set_default_i32(_c, SceneIds.MeshMaterial, SceneNative.aver_scene_material(0, material));
    }

    /// <summary>Add a point light. Intensity in lux, range in centimetres, per the coordinate contract.</summary>
    public void PointLight(float intensityLux, float rangeCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CLight);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CLight.intensityLux"), intensityLux);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CLight.rangeCm"), rangeCm);
    }

    /// <summary>Add a camera. FoV authored in degrees (the field stores radians); clip planes in centimetres.</summary>
    public void Camera(float fovDegrees, float nearCm, float farCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CCamera);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.fovYRad"), fovDegrees * (MathF.PI / 180f));
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.nearCm"), nearCm);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.Field("CCamera.farCm"), farCm);
    }

    /// <summary>
    /// Declare that the class ticks, in <paramref name="group"/> at <paramref name="order"/>. Setting the
    /// TICKS flag AND the tick group in one call is deliberate: the group is recorded on the class row here
    /// (not left at the zero default), so the bind path reads a real group off the class rather than
    /// defaulting every managed actor into PrePhysics (contradiction #3, resolved).
    /// </summary>
    public void Ticks(TickGroup group, int order = 0)
    {
        Fw.aver_fw_class_set_flags(_c, Fw.aver_fw_class_get_flags(_c) | ClassFlags.Ticks);
        Fw.aver_fw_class_set_tick(_c, (int)group, order);
    }
}
