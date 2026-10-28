// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The 3D node classes of a converted scene: markers, meshes, lights, cameras and collision shapes. Each keeps Godot's properties
// as plain values and, once it has an entity, pushes the ones the engine can use into the entity's components.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>A point in space to mark a position (Godot's Marker3D).</summary>
public class Marker3D : Node3D
{
    public float GizmoExtents { get; set; } = 0.25f;
}

/// <summary>Anything drawn in 3D (Godot's VisualInstance3D).</summary>
public class VisualInstance3D : Node3D
{
    public uint Layers { get; set; } = 1;
    public bool GetLayerMaskValue(int layerNumber) => (Layers & (1u << (layerNumber - 1))) != 0;
    public void SetLayerMaskValue(int layerNumber, bool value) => Layers = value ? Layers | (1u << (layerNumber - 1)) : Layers & ~(1u << (layerNumber - 1));
}

public class GeometryInstance3D : VisualInstance3D
{
    public enum ShadowCastingSetting { Off = 0, On = 1, DoubleSided = 2, ShadowsOnly = 3 }

    private Material? _materialOverride;

    /// <summary>Replaces the mesh's own material; its asset name reaches the entity's mesh renderer.</summary>
    public Material? MaterialOverride
    {
        get => _materialOverride;
        set { _materialOverride = value; OnMaterialOverrideChanged(); }
    }

    internal virtual void OnMaterialOverrideChanged() { }

    public Material? MaterialOverlay { get; set; }
    public ShadowCastingSetting CastShadow { get; set; } = ShadowCastingSetting.On;
    public float Transparency { get; set; }
    public float LodBias { get; set; } = 1f;
}

/// <summary>A node that draws a mesh (Godot's MeshInstance3D). On entering the tree it gives its entity a mesh renderer for
/// <see cref="Mesh"/>'s asset path.</summary>
public class MeshInstance3D : GeometryInstance3D
{
    private Mesh? _mesh;

    public Mesh? Mesh
    {
        get => _mesh;
        set { _mesh = value; ApplyMesh(); }
    }

    public int GetSurfaceOverrideMaterialCount() => 1;
    public Material? GetSurfaceOverrideMaterial(int surface) => surface == 0 ? MaterialOverride : null;
    public void SetSurfaceOverrideMaterial(int surface, Material? material) { if (surface == 0) MaterialOverride = material; }

    // "res://meshes/a.ocmesh" and "meshes/a.ocmesh" both name the content asset "meshes/a.ocmesh".
    internal static string AssetPath(string resourcePath) =>
        resourcePath.StartsWith("res://", StringComparison.Ordinal) ? resourcePath["res://".Length..] : resourcePath;

    private void ApplyMesh()
    {
        if (!Self.IsValid || !NodeHost.Current.IsEngine || _mesh == null || _mesh.ResourcePath.Length == 0) return;
        Self.SetMesh(AssetPath(_mesh.ResourcePath));
        if (MaterialOverride is { ResourcePath.Length: > 0 }) Self.SetMaterial(System.IO.Path.GetFileNameWithoutExtension(MaterialOverride.ResourcePath));
    }

    internal override void OnMaterialOverrideChanged() => ApplyMesh();

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        ApplyMesh();
    }

    internal override void OnVisibilityApplied(bool effective)
    {
        if (Self.IsValid && Self.HasComponent(Component.MeshRenderer)) Self.SetVisible(effective);
    }
}

/// <summary>Any light (Godot's Light3D). Colour, energy and shadows reach the entity's light component whenever they change.
/// Godot's energy is unitless and Aver's is lux, so <see cref="LuxPerEnergy"/> is the one calibration constant (the sun, energy 1,
/// is Aver's 100000 lux default); colours are sRGB in Godot and linear in Aver.</summary>
public class Light3D : VisualInstance3D
{
    /// <summary>Aver lux for a Godot energy of 1.</summary>
    public const float LuxPerEnergy = 100000f;

    private Color _color = new(1, 1, 1, 1);
    private float _energy = 1f;
    private bool _shadow;

    public Color LightColor { get => _color; set { _color = value; ApplyLight(); } }
    public float LightEnergy { get => _energy; set { _energy = value; ApplyLight(); } }
    public float LightIndirectEnergy { get; set; } = 1f;
    public float LightSpecular { get; set; } = 0.5f;
    public bool ShadowEnabled { get => _shadow; set { _shadow = value; ApplyLight(); } }
    public uint LightCullMask { get; set; } = 0xFFFFF;

    /// <summary>The light component's kind: 0 point, 1 spot, 2 directional.</summary>
    internal virtual int LightKind => 0;

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        ApplyLight();
    }

    /// <summary>Writes this light's values into the entity's light component (adding it first).</summary>
    internal void ApplyLight()
    {
        if (!Self.IsValid || !NodeHost.Current.IsEngine) return;
        if (!Self.HasComponent(Component.Light) && !Self.AddComponent(Component.Light)) return;
        Self.SetInt("CLight.kind", LightKind);
        Color lin = _color.SrgbToLinear();
        Self.SetLightColour(new Aver.Scene.Vec3(lin.R, lin.G, lin.B));
        Self.SetLightIntensity(_energy * LuxPerEnergy);
        Self.SetLightShadows(_shadow);
        ApplyKindValues();
    }

    // The values only one kind of light has (range, cone).
    internal virtual void ApplyKindValues() { }
}

public class DirectionalLight3D : Light3D
{
    public float DirectionalShadowMaxDistance { get; set; } = 100f;

    internal override int LightKind => 2;
}

public class OmniLight3D : Light3D
{
    private float _range = 5f;

    public float OmniRange { get => _range; set { _range = value; ApplyLight(); } }
    public float OmniAttenuation { get; set; } = 1f;

    internal override int LightKind => 0;
    internal override void ApplyKindValues() => Self.SetLightRange(FabricSpace.ToAverLength(_range));
}

public class SpotLight3D : Light3D
{
    private float _range = 5f, _angle = 45f;

    public float SpotRange { get => _range; set { _range = value; ApplyLight(); } }
    /// <summary>The cone's half-angle in degrees.</summary>
    public float SpotAngle { get => _angle; set { _angle = value; ApplyLight(); } }
    public float SpotAttenuation { get; set; } = 1f;

    internal override int LightKind => 1;

    internal override void ApplyKindValues()
    {
        Self.SetLightRange(FabricSpace.ToAverLength(_range));
        Self.SetLightCone(_angle * 0.8f, _angle);   // Godot has no inner cone; the soft edge is approximated
    }
}

/// <summary>A camera (Godot's Camera3D). The field of view is vertical, in degrees; near and far are in metres.</summary>
public class Camera3D : Node3D
{
    public enum ProjectionType { Perspective = 0, Orthogonal = 1, Frustum = 2 }
    public enum KeepAspectEnum { Width = 0, Height = 1 }

    private float _fov = 75f, _near = 0.05f, _far = 4000f;

    public float Fov { get => _fov; set { _fov = value; ApplyCamera(); } }
    public float Near { get => _near; set { _near = value; ApplyCamera(); } }
    public float Far { get => _far; set { _far = value; ApplyCamera(); } }
    public float Size { get; set; } = 1f;
    public ProjectionType Projection { get; set; } = ProjectionType.Perspective;
    public KeepAspectEnum KeepAspect { get; set; } = KeepAspectEnum.Height;
    public uint CullMask { get; set; } = 0xFFFFF;
    public float HOffset { get; set; }
    public float VOffset { get; set; }
    private bool _current;

    /// <summary>Whether this is the camera that is drawn. Set it to take over from the current one.</summary>
    public bool Current
    {
        get => _current;
        set { if (value) MakeCurrent(); else _current = false; }
    }

    /// <summary>The camera that is drawn: the one made current last.</summary>
    internal static Camera3D? CurrentCamera { get; private set; }

    internal static void ResetCurrent() => CurrentCamera = null;

    public void MakeCurrent()
    {
        if (CurrentCamera is { } old && old != this) old._current = false;
        CurrentCamera = this;
        _current = true;
        if (Self.IsValid && NodeHost.Current.IsEngine) Aver.Framework.Fw.aver_fw_set_view_entity(Self.Handle);
    }

    public void ClearCurrent(bool enableNext = true)
    {
        _current = false;
        if (CurrentCamera == this) CurrentCamera = null;
    }

    public bool IsCurrent() => _current;

    // ---- projection (pure maths: the viewport size and the camera's transform) ----

    // Half the width and height of the view at distance 1 (perspective) or at any distance (orthogonal).
    private (float X, float Y) HalfExtents(Vector2 viewport)
    {
        float aspect = viewport.Y > 0 ? viewport.X / viewport.Y : 1f;
        if (Projection == ProjectionType.Orthogonal)
            return KeepAspect == KeepAspectEnum.Height ? (Size * 0.5f * aspect, Size * 0.5f) : (Size * 0.5f, Size * 0.5f / aspect);
        float half = MathF.Tan(Mathf.DegToRad(_fov) * 0.5f);
        return KeepAspect == KeepAspectEnum.Height ? (half * aspect, half) : (half, half / aspect);
    }

    private Vector2 ViewportSize() => GetViewport() is { } v ? new Vector2(v.Size.X, v.Size.Y) : new Vector2(1280, 720);

    // A screen point as -1..1 across and up.
    private static Vector2 ToNdc(Vector2 screen, Vector2 viewport) => new(screen.X / viewport.X * 2f - 1f, 1f - screen.Y / viewport.Y * 2f);

    /// <summary>Where the ray through a screen point starts, in world space (the camera's position, or on the view plane when orthogonal).</summary>
    public Vector3 ProjectRayOrigin(Vector2 screenPoint)
    {
        Transform3D t = GlobalTransform;
        if (Projection != ProjectionType.Orthogonal) return t.Origin;
        Vector2 vp = ViewportSize();
        Vector2 ndc = ToNdc(screenPoint, vp);
        (float hx, float hy) = HalfExtents(vp);
        return t * new Vector3(ndc.X * hx, ndc.Y * hy, -_near);
    }

    /// <summary>The direction of the ray through a screen point, in world space, normalised.</summary>
    public Vector3 ProjectRayNormal(Vector2 screenPoint)
    {
        Transform3D t = GlobalTransform;
        if (Projection == ProjectionType.Orthogonal) return (t.Basis * new Vector3(0, 0, -1)).Normalized();
        Vector2 vp = ViewportSize();
        Vector2 ndc = ToNdc(screenPoint, vp);
        (float hx, float hy) = HalfExtents(vp);
        return (t.Basis * new Vector3(ndc.X * hx, ndc.Y * hy, -1f)).Normalized();
    }

    /// <summary>The world point at <paramref name="zDepth"/> metres in front of the camera that appears at a screen point.</summary>
    public Vector3 ProjectPosition(Vector2 screenPoint, float zDepth)
    {
        Vector2 vp = ViewportSize();
        Vector2 ndc = ToNdc(screenPoint, vp);
        (float hx, float hy) = HalfExtents(vp);
        float scale = Projection == ProjectionType.Orthogonal ? 1f : zDepth;
        return GlobalTransform * new Vector3(ndc.X * hx * scale, ndc.Y * hy * scale, -zDepth);
    }

    /// <summary>The screen point a world point appears at. A point behind the camera comes out mirrored: check <see cref="IsPositionBehind"/>.</summary>
    public Vector2 UnprojectPosition(Vector3 worldPoint)
    {
        Vector2 vp = ViewportSize();
        Vector3 p = GlobalTransform.AffineInverse() * worldPoint;
        (float hx, float hy) = HalfExtents(vp);
        float ndcX, ndcY;
        if (Projection == ProjectionType.Orthogonal) { ndcX = p.X / hx; ndcY = p.Y / hy; }
        else
        {
            float depth = -p.Z;
            ndcX = p.X / depth / hx;
            ndcY = p.Y / depth / hy;
        }
        return new Vector2((ndcX * 0.5f + 0.5f) * vp.X, (0.5f - ndcY * 0.5f) * vp.Y);
    }

    /// <summary>True when the world point is nearer the camera than its near plane along the view direction.</summary>
    public bool IsPositionBehind(Vector3 worldPoint)
    {
        Transform3D t = GlobalTransform;
        Vector3 forward = (t.Basis * new Vector3(0, 0, -1)).Normalized();
        return forward.Dot(worldPoint - t.Origin) < _near;
    }

    public bool IsPositionInFrustum(Vector3 worldPoint)
    {
        if (IsPositionBehind(worldPoint)) return false;
        Vector2 vp = ViewportSize();
        Vector2 s = UnprojectPosition(worldPoint);
        return s.X >= 0 && s.Y >= 0 && s.X <= vp.X && s.Y <= vp.Y;
    }

    public Transform3D GetCameraTransform() => GlobalTransform.Orthonormalized();

    public Projection GetCameraProjection()
    {
        Vector2 vp = ViewportSize();
        float aspect = vp.Y > 0 ? vp.X / vp.Y : 1f;
        if (Projection == ProjectionType.Orthogonal)
        {
            (float hx, float hy) = HalfExtents(vp);
            return Aver.Fabric.Projection.CreateOrthogonal(-hx, hx, -hy, hy, _near, _far);
        }
        return Aver.Fabric.Projection.CreatePerspective(_fov, aspect, _near, _far, KeepAspect == KeepAspectEnum.Width);
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        ApplyCamera();
    }

    private void ApplyCamera()
    {
        if (!Self.IsValid || !NodeHost.Current.IsEngine) return;
        if (!Self.HasComponent(Component.Camera) && !Self.AddComponent(Component.Camera)) return;
        Self.SetFloat("CCamera.fovYRad", Mathf.DegToRad(_fov));
        Self.SetFloat("CCamera.nearCm", FabricSpace.ToAverLength(_near));
        Self.SetFloat("CCamera.farCm", FabricSpace.ToAverLength(_far));
    }

    internal override void OnReadying()
    {
        base.OnReadying();
        if (_current) MakeCurrent();
    }
}

/// <summary>A shape for the collision node above it (Godot's CollisionShape3D). The parent builds its body from these children
/// when it becomes ready.</summary>
public class CollisionShape3D : Node3D
{
    private Shape3D? _shape;
    private bool _disabled;

    public Shape3D? Shape { get => _shape; set { _shape = value; (GetParent() as CollisionObject3D)?.QueueRebuild(); } }

    /// <summary>A disabled shape is not part of its parent's body. Changing it after the parent is ready makes the body again.</summary>
    public bool Disabled { get => _disabled; set { _disabled = value; (GetParent() as CollisionObject3D)?.QueueRebuild(); } }
}
