// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SubViewport, ViewportTexture and SubViewportContainer: render-to-texture views of the world (G4_RENDER.md R5). A SubViewport asks the engine for a view named by its
// key; the engine draws it with the first Camera3D below the node (or the one made current) into a texture that materials (the converter bakes the reference into
// the .ocmat) and the UI show. The view is the cheap raster configuration: see the contract for what it does not draw.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>The calls a SubViewport makes on the engine. The engine's is <see cref="SubViews"/>; a test installs a recording one.</summary>
internal interface ISubViewBackend
{
    int Create(string key, int width, int height);
    bool Destroy(int id);
    bool SetSize(int id, int width, int height);
    bool SetUpdate(int id, SubViewUpdate mode);
    bool SetClear(int id, SubViewClear mode);
    bool SetTransparent(int id, bool on);
    bool SetOwnWorld(int id, bool on);
    bool SetCamera(int id, int entity);
    bool SetScale(int id, float scale, int msaa);
    bool SetParentVisible(int id, bool on);
    bool SetClearColor(int id, float r, float g, float b, float a);
    bool AddMaterialConsumer(int id);
    bool RemoveMaterialConsumer(int id);
    bool AddUiConsumer(int id);
    bool RemoveUiConsumer(int id);
    bool RequestRender(int id);
    bool SetScreenRect(int id, float x, float y, float width, float height, bool visible);
    bool TryGetInfo(int id, out SubViewInfo info);
    ulong Texture(int id);
}

internal sealed class NativeSubViewBackend : ISubViewBackend
{
    public int Create(string key, int width, int height) => SubViews.Create(key, width, height);
    public bool Destroy(int id) => SubViews.Destroy(id);
    public bool SetSize(int id, int width, int height) => SubViews.SetSize(id, width, height);
    public bool SetUpdate(int id, SubViewUpdate mode) => SubViews.SetUpdate(id, mode);
    public bool SetClear(int id, SubViewClear mode) => SubViews.SetClear(id, mode);
    public bool SetTransparent(int id, bool on) => SubViews.SetTransparent(id, on);
    public bool SetOwnWorld(int id, bool on) => SubViews.SetOwnWorld(id, on);
    public bool SetCamera(int id, int entity) => SubViews.SetCamera(id, entity);
    public bool SetScale(int id, float scale, int msaa) => SubViews.SetScale(id, scale, msaa);
    public bool SetParentVisible(int id, bool on) => SubViews.SetParentVisible(id, on);
    public bool SetClearColor(int id, float r, float g, float b, float a) => SubViews.SetClearColor(id, r, g, b, a);
    public bool AddMaterialConsumer(int id) => SubViews.AddMaterialConsumer(id);
    public bool RemoveMaterialConsumer(int id) => SubViews.RemoveMaterialConsumer(id);
    public bool AddUiConsumer(int id) => SubViews.AddUiConsumer(id);
    public bool RemoveUiConsumer(int id) => SubViews.RemoveUiConsumer(id);
    public bool RequestRender(int id) => SubViews.RequestRender(id);
    public bool SetScreenRect(int id, float x, float y, float width, float height, bool visible) => SubViews.SetScreenRect(id, x, y, width, height, visible);
    public bool TryGetInfo(int id, out SubViewInfo info) => SubViews.TryGetInfo(id, out info);
    public ulong Texture(int id) => SubViews.Texture(id);
}

internal static class SubViewBackend
{
    private static readonly NativeSubViewBackend s_native = new();

    /// <summary>A test's recording backend, or null for the engine's.</summary>
    internal static ISubViewBackend? Override;

    /// <summary>Where calls go: the override, else the engine when nodes live in it, else nowhere (a headless tree makes no views).</summary>
    internal static ISubViewBackend? Current => Override ?? (NodeHost.Current.IsEngine ? s_native : null);

    /// <summary>The entity handle a node's camera component lives on, 0 for none. A test replaces it.</summary>
    internal static Func<Node, int> EntityOf = n => n.Self.IsValid ? n.Self.Handle : 0;
}

/// <summary>
/// The picture a <see cref="SubViewport"/> draws (Godot's ViewportTexture). It is a handle to the view, not a copy of its pixels: the engine keeps the target,
/// a material the converter baked shows it through <see cref="MaterialReference"/>, and <see cref="NativeHandle"/> is what a UI image widget is given.
/// </summary>
public class ViewportTexture : Texture2D
{
    /// <summary>The viewport's path as the scene stored it, relative to the scene's root; empty for a texture made by <see cref="SubViewport.GetTexture"/>.</summary>
    public NodePath ViewportPath { get; set; } = new NodePath("");

    internal SubViewport? Source { get; set; }

    /// <summary>The viewport this texture shows, null until it is in the tree.</summary>
    public SubViewport? GetViewport() => Source;

    public override Vector2 GetSize() => Source != null ? new Vector2(Source.DrawnSize.X, Source.DrawnSize.Y) : Vector2.Zero;

    internal override ulong EngineHandle => NativeHandle;

    /// <summary>The engine's texture for the view (an engine texture handle), 0 until the view has been drawn once. It changes when the view is resized;
    /// <see cref="Generation"/> says when.</summary>
    public ulong NativeHandle => Source?.NativeTexture ?? 0;

    /// <summary>Changes whenever <see cref="NativeHandle"/> does.</summary>
    public int Generation => Source?.TextureGeneration ?? 0;

    /// <summary>The texture reference a material names this view by: <c>@rt:viewport.&lt;key&gt;</c>. Empty until the viewport has a key.</summary>
    public string MaterialReference => Source != null ? "@rt:viewport." + Source.EffectiveKey : "";
}

/// <summary>
/// A view of the world drawn into a texture (Godot's SubViewport). The camera is the first <see cref="Camera3D"/> below it, or the one made current. Update mode,
/// clear mode, transparent background and size are honoured; <c>own_world_3d</c>, MSAA and the 3D scaling mode are carried and not used. The picture is drawn
/// by a cheap raster configuration (one sun, flat material colours, no shadows or global illumination, no particles, foliage, terrain or water).
/// </summary>
public class SubViewport : Viewport
{
    public enum UpdateMode { Disabled = 0, Once = 1, WhenVisible = 2, WhenParentVisible = 3, Always = 4 }
    public enum ClearMode { Always = 0, Never = 1, Once = 2 }
    public enum MsaaMode { Disabled = 0, Msaa2X = 1, Msaa4X = 2, Msaa8X = 3 }
    public enum Scaling3DModeType { Bilinear = 0, Fsr = 1, Fsr2 = 2, Max = 3 }

    private Vector2I _size = new(512, 512);
    private UpdateMode _update = UpdateMode.WhenVisible;
    private ClearMode _clear = ClearMode.Always;
    private bool _transparent, _ownWorld, _disable3D;
    private MsaaMode _msaa = MsaaMode.Disabled;
    private Scaling3DModeType _scalingMode = Scaling3DModeType.Bilinear;
    private float _scale = 1f;
    private Color _clearColor = new(0.3f, 0.3f, 0.3f, 1f);
    private Camera3D? _camera;
    private int _id;
    private int _materialConsumers;
    private string _key = "";
    private ViewportTexture? _texture;
    private bool _parentVisiblePushed = true;

    /// <summary>The size in pixels (Godot's default is 512 by 512). The engine draws it at this size times <see cref="Scaling3DScale"/>, fitted to the project's
    /// pixel budget; a change rebuilds the target, which waits for the GPU, so set it when the view is made and not every frame.</summary>
    public override Vector2I Size { get => _size; set { _size = value; if (_id != 0) Backend?.SetSize(_id, Math.Max(1, value.X), Math.Max(1, value.Y)); } }

    public Vector2I Size2DOverride { get; set; }

    public UpdateMode RenderTargetUpdateMode
    {
        get => _update;
        set { _update = value; if (_id != 0) Backend?.SetUpdate(_id, (SubViewUpdate)(int)value); }
    }

    public ClearMode RenderTargetClearMode
    {
        get => _clear;
        set { _clear = value; if (_id != 0) Backend?.SetClear(_id, (SubViewClear)(int)value); }
    }

    public bool TransparentBg
    {
        get => _transparent;
        set { _transparent = value; if (_id != 0) Backend?.SetTransparent(_id, value); }
    }

    /// <summary>Carried only: the engine has one world, so the view always shows it.</summary>
    public bool OwnWorld3D
    {
        get => _ownWorld;
        set { _ownWorld = value; if (_id != 0) Backend?.SetOwnWorld(_id, value); }
    }

    /// <summary>Carried only: the view is drawn at one sample.</summary>
    public MsaaMode Msaa3D
    {
        get => _msaa;
        set { _msaa = value; PushScale(); }
    }

    /// <summary>Carried only: any non-bilinear mode is drawn as a plain scale.</summary>
    public Scaling3DModeType Scaling3DMode { get => _scalingMode; set => _scalingMode = value; }

    /// <summary>Multiplies the size the view is drawn at (0.25 to 8).</summary>
    public float Scaling3DScale
    {
        get => _scale;
        set { _scale = value; PushScale(); }
    }

    /// <summary>A view with 3D disabled draws nothing: the engine is given no camera for it.</summary>
    public bool Disable3D
    {
        get => _disable3D;
        set { _disable3D = value; PushCamera(); }
    }

    /// <summary>The colour the target is cleared to when the background is not transparent (Godot takes it from the environment; the engine's views clear to
    /// a flat colour, Godot's default clear colour unless set). Not a Godot property.</summary>
    public Color ClearColor
    {
        get => _clearColor;
        set { _clearColor = value; if (_id != 0) Backend?.SetClearColor(_id, value.R, value.G, value.B, value.A); }
    }

    /// <summary>The name the engine knows this view by: the converter writes it (<c>_viewport_key</c>) as <c>&lt;level&gt;:&lt;path from the level's root&gt;</c>, so a baked
    /// material finds it. A view made by script is named by its tree path.</summary>
    internal string ViewportKey { get => _key; set => _key = value ?? ""; }

    /// <summary>How many materials the converter found that show this view (<c>_material_consumers</c>); a WhenVisible view is drawn while there is one.</summary>
    internal int MaterialConsumers
    {
        get => _materialConsumers;
        set
        {
            int next = Math.Max(0, value);
            if (_id != 0 && Backend is { } b)
            {
                for (int i = _materialConsumers; i < next; i++) b.AddMaterialConsumer(_id);
                for (int i = _materialConsumers; i > next; i--) b.RemoveMaterialConsumer(_id);
            }
            _materialConsumers = next;
        }
    }

    private static ISubViewBackend? Backend => SubViewBackend.Current;

    /// <summary>The key the engine has this view under.</summary>
    internal string EffectiveKey => _key.Length > 0 ? _key : (IsInsideTree() ? GetPath().ToString() : Name.ToString());

    /// <summary>The engine's id for the view, 0 when it has none (not in the tree, no host, or the budget refused it).</summary>
    internal int ViewId => _id;

    /// <summary>The camera the view is drawn with.</summary>
    internal Camera3D? ActiveCamera => _camera;

    /// <summary>The size the engine draws at, after the 3D scale and the pixel budget; the requested size before the view exists.</summary>
    internal Vector2I DrawnSize => _id != 0 && Backend is { } b && b.TryGetInfo(_id, out SubViewInfo i) && i.Width > 0 ? new Vector2I(i.Width, i.Height) : _size;

    internal ulong NativeTexture => _id != 0 && Backend is { } b ? b.Texture(_id) : 0;

    internal int TextureGeneration => _id != 0 && Backend is { } b && b.TryGetInfo(_id, out SubViewInfo i) ? i.Generation : 0;

    /// <summary>The picture this viewport draws. The same object every time.</summary>
    public override ViewportTexture? GetTexture() => _texture ??= new ViewportTexture { Source = this };

    public override Camera3D? GetCamera3D() => _camera;

    /// <summary>Asks for one more picture whatever the update mode says.</summary>
    public void RequestRender()
    {
        if (_id != 0) Backend?.RequestRender(_id);
    }

    // ---- the camera ----

    // A Camera3D inside this viewport that was made current. Another one made current replaces it.
    internal void SetActiveCamera(Camera3D camera)
    {
        if (_camera != null && _camera != camera) _camera.ClearActiveFlag();
        _camera = camera;
        PushCamera();
    }

    internal void ReleaseCamera(Camera3D camera)
    {
        if (_camera != camera) return;
        // Another camera below takes over, as Godot makes the next one in the tree current. The leaving one is still in the tree while it exits.
        _camera = FirstCameraBelow(camera);
        PushCamera();
    }

    private Camera3D? FirstCameraBelow(Camera3D? except = null)
    {
        foreach (Node n in NodeHost.PreOrder(this))
        {
            if (n == this || n == except || n is not Camera3D c || !c.IsInsideTree()) continue;
            if (c.OwningSubViewport() == this) return c;
        }
        return null;
    }

    private void PushCamera()
    {
        if (_id == 0 || Backend is not { } b) return;
        int entity = !_disable3D && _camera != null ? SubViewBackend.EntityOf(_camera) : 0;
        b.SetCamera(_id, entity);
    }

    private void PushScale()
    {
        if (_id != 0) Backend?.SetScale(_id, _scale, _msaa == MsaaMode.Disabled ? 0 : 1 << (int)_msaa);
    }

    // WhenParentVisible: a hidden ancestor (a CanvasItem or a Node3D) stops the view.
    internal bool ParentVisible()
    {
        for (Node? n = GetParent(); n != null; n = n.GetParent())
        {
            if (n is CanvasItem ci && !ci.Visible) return false;
            if (n is Node3D n3 && !n3.Visible) return false;
        }
        return true;
    }

    internal void RefreshParentVisible()
    {
        bool visible = ParentVisible();
        if (_id == 0 || visible == _parentVisiblePushed) return;
        _parentVisiblePushed = visible;
        Backend?.SetParentVisible(_id, visible);
    }

    // ---- the engine's view ----

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Register();
    }

    internal override void OnReadying()
    {
        base.OnReadying();
        // The children are ready: a camera made current claimed the view in its own OnReadying; otherwise the first one below is it.
        _camera ??= FirstCameraBelow();
        PushCamera();
        _parentVisiblePushed = true;
        RefreshParentVisible();
    }

    internal override void OnExitingTree()
    {
        Unregister();
        base.OnExitingTree();
    }

    private void Register()
    {
        if (_id != 0 || Backend is not { } b) return;
        _id = b.Create(EffectiveKey, Math.Max(1, _size.X), Math.Max(1, _size.Y));
        if (_id == 0)
        {
            GD.PushWarning($"SubViewport '{Name}': the engine refused the view '{EffectiveKey}' (RENDER.SUBVIEWPORTS is the budget; 0 turns sub-viewports off).");
            return;
        }
        b.SetUpdate(_id, (SubViewUpdate)(int)_update);
        b.SetClear(_id, (SubViewClear)(int)_clear);
        b.SetTransparent(_id, _transparent);
        b.SetOwnWorld(_id, _ownWorld);
        b.SetClearColor(_id, _clearColor.R, _clearColor.G, _clearColor.B, _clearColor.A);
        PushScale();
        for (int i = 0; i < _materialConsumers; i++) b.AddMaterialConsumer(_id);
        PushCamera();
        (GetParent() as SubViewportContainer)?.Refresh();   // _Ready runs once, so a view made on a later entry tells its container itself
    }

    private void Unregister()
    {
        if (_id == 0) return;
        Backend?.Destroy(_id);
        _id = 0;
        _parentVisiblePushed = true;
    }
}

/// <summary>
/// Shows a <see cref="SubViewport"/> child on the window (Godot's SubViewportContainer): split screen, a picture-in-picture, a minimap. With <see cref="Stretch"/> the
/// view is drawn at the container's size (divided by <see cref="StretchShrink"/>); without it, at its own size. The rectangle is the container's position and size in the UI's
/// pixels, so a container in a hidden parent is hidden too. Position and size are read as set; anchors and container layout are not computed.
/// </summary>
public class SubViewportContainer : Control
{
    private bool _stretch;
    private int _shrink = 1;
    private int _uiId;   // the view this container counts as a UI consumer of, 0 for none

    public bool Stretch { get => _stretch; set { _stretch = value; Refresh(); } }
    public int StretchShrink { get => _shrink; set { _shrink = Math.Max(1, value); Refresh(); } }

    /// <summary>The SubViewport children this container shows, in order.</summary>
    internal IEnumerable<SubViewport> Viewports()
    {
        foreach (Node c in ChildrenSnapshot())
            if (c is SubViewport sv) yield return sv;
    }

    // The container's rectangle is the Control's: anchors and offsets against the parent (G5 U1 gave Control the real layout), GetGlobalRect() from it.

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        Refresh();
    }

    internal override void OnReadying()
    {
        base.OnReadying();
        Refresh();
    }

    internal override void OnExitingTree()
    {
        TakeOffScreen();
        base.OnExitingTree();
    }

    internal override void OnRectChanged()
    {
        base.OnRectChanged();
        Refresh();
    }

    internal override void OnShownChanged()
    {
        base.OnShownChanged();
        Refresh();
    }

    // Puts the first viewport child on screen (or takes it off) and sizes it. Safe to call at any time.
    internal void Refresh()
    {
        if (SubViewBackend.Current is not { } b) return;
        bool show = IsInsideTree() && IsVisibleInTree();
        foreach (SubViewport sv in Viewports())
        {
            sv.RefreshParentVisible();
            int id = sv.ViewId;
            if (id == 0) continue;
            Rect2 r = GetGlobalRect();
            int w = Math.Max(1, (int)(Stretch ? r.Size.X / _shrink : sv.Size.X)), h = Math.Max(1, (int)(Stretch ? r.Size.Y / _shrink : sv.Size.Y));
            if (Stretch && r.Size.X >= 1f && r.Size.Y >= 1f && (sv.Size.X != w || sv.Size.Y != h)) sv.Size = new Vector2I(w, h);
            float drawW = Stretch ? r.Size.X : sv.Size.X, drawH = Stretch ? r.Size.Y : sv.Size.Y;
            bool ok = show && drawW >= 1f && drawH >= 1f;
            b.SetScreenRect(id, r.Position.X, r.Position.Y, drawW, drawH, ok);
            if (ok && _uiId != id) { b.AddUiConsumer(id); _uiId = id; }   // a view made again after leaving the tree has a new id and no consumers
            else if (!ok && _uiId == id) { b.RemoveUiConsumer(id); _uiId = 0; }
            break;   // one view per container, as in Godot's usual use
        }
    }

    private void TakeOffScreen()
    {
        int held = _uiId;
        _uiId = 0;   // the view below is usually destroyed in the same exit (children leave first); a later entry counts again from nothing
        if (SubViewBackend.Current is not { } b) return;
        foreach (SubViewport sv in Viewports())
        {
            int id = sv.ViewId;
            if (id == 0) continue;
            b.SetScreenRect(id, 0, 0, 0, 0, false);
            if (held == id) b.RemoveUiConsumer(id);
            break;
        }
    }
}
