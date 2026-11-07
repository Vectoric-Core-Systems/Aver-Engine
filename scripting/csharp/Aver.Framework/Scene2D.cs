// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// 2D scenes: sprites, 2D transforms and canvas layers as entity components, and the ordered canvas (scene2d_abi.h).

using System.Numerics;
using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The 2D scene C ABI (scene2d_abi.h), in Aver.Scene.dll. Names and parameter names are copied from the header character
/// for character (tests/abi checks them); the struct layouts are pinned by Scene2dAbiTest.</summary>
internal static class Scene2DNative
{
    private const string Lib = "Aver.Scene";

    // AverScene2dTransform, field for field. Vector members are spelled as scalars so the managed layout cannot differ from the C struct.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dTransform
    {
        public float position0, position1;
        public float rotation;
        public float scale0, scale1;
        public int zIndex;
        public uint flags;
        public uint treeOrder;
    }

    // AverScene2dSprite.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dSprite
    {
        public long texture;
        public float region0, region1, region2, region3;
        public float offset0, offset1;
        public uint modulate;
        public uint flags;
        public int hframes;
        public int vframes;
        public int frame;
        public int blend;
    }

    // AverScene2dLayer.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dLayer
    {
        public int index;
        public uint flags;
        public float offset0, offset1;
        public float rotation;
        public float scale0, scale1;
        public uint reserved;
    }

    [DllImport(Lib)] internal static extern int aver_scene2d_abi_version();
    [DllImport(Lib)] internal static extern int aver_scene2d_register();
    [DllImport(Lib)] internal static extern int aver_scene2d_component_id(int which);
    [DllImport(Lib)] internal static extern int aver_scene2d_struct_size(int which);
    [DllImport(Lib)] internal static extern int aver_scene2d_set_transform(int entity, ref AverScene2dTransform desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_set_sprite(int entity, ref AverScene2dSprite desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_set_layer(int entity, ref AverScene2dLayer desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_transform(int entity, out AverScene2dTransform outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_sprite(int entity, out AverScene2dSprite outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_layer(int entity, out AverScene2dLayer outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_remove(int entity, int which);
    [DllImport(Lib)] internal static extern int aver_scene2d_update_transforms();
    [DllImport(Lib)] internal static extern int aver_scene2d_world_affine(int entity, float[] outAffine6, out int outZ);
    [DllImport(Lib)] internal static extern int aver_scene2d_sprite_bounds(int entity, float[] outRect4);
    [DllImport(Lib)] internal static extern void aver_scene2d_set_canvas(float[]? in6, float viewX, float viewY, float viewW, float viewH);
    [DllImport(Lib)] internal static extern int aver_scene2d_canvas_transform(float[] out6, float[]? out11);
    [DllImport(Lib)] internal static extern void aver_scene2d_set_texture_resolver(IntPtr fn, IntPtr user);
    [DllImport(Lib)] internal static extern int aver_scene2d_submit_external(int layer, int z, long order, IntPtr fn, IntPtr user, uint flags);
    [DllImport(Lib)] internal static extern int aver_scene2d_pending();
    [DllImport(Lib)] internal static extern void aver_scene2d_flush(IntPtr drawList);
    [DllImport(Lib)] internal static extern int aver_scene2d_frame(IntPtr drawList);
}

/// <summary>Flags of <see cref="Transform2D"/>.</summary>
[Flags]
public enum Transform2DFlags : uint
{
    /// <summary>None: z is an offset from the parent's, the parent's transform applies, the entity draws.</summary>
    None = 0,
    /// <summary>The z-index is the final z, not an offset from the parent's.</summary>
    ZAbsolute = 1,
    /// <summary>Ignore the parent's transform (the entity keeps its place in the hierarchy).</summary>
    TopLevel = 2,
    /// <summary>This entity and everything below it draws nothing.</summary>
    Hidden = 4,
}

/// <summary>Flags of <see cref="Sprite2D"/>.</summary>
[Flags]
public enum Sprite2DFlags : uint
{
    /// <summary>None: centered, linear sampling, visible, not flipped.</summary>
    None = 0,
    /// <summary>Mirror left to right.</summary>
    FlipH = 1,
    /// <summary>Mirror top to bottom.</summary>
    FlipV = 2,
    /// <summary>The offset is the top-left corner instead of the centre.</summary>
    NotCentered = 4,
    /// <summary>Draw nothing.</summary>
    Hidden = 8,
    /// <summary>Nearest-neighbour sampling instead of linear.</summary>
    Nearest = 16,
}

/// <summary>How a sprite combines with what is behind it.</summary>
public enum Sprite2DBlend
{
    /// <summary>Alpha blend.</summary>
    Mix = 0,
    /// <summary>Added.</summary>
    Add = 1,
    /// <summary>Subtracted.</summary>
    Sub = 2,
    /// <summary>Multiplied.</summary>
    Mul = 3,
}

/// <summary>Flags of <see cref="Layer2D"/>.</summary>
[Flags]
public enum Layer2DFlags : uint
{
    /// <summary>None: visible.</summary>
    None = 0,
    /// <summary>Everything in the layer draws nothing.</summary>
    Hidden = 1,
    /// <summary>Stored for the camera that comes later; changes nothing yet.</summary>
    FollowViewport = 2,
}

/// <summary>The viewport stretch mode of the canvas.</summary>
public enum CanvasStretchMode
{
    /// <summary>Content is the window's size.</summary>
    Disabled = 0,
    /// <summary>Content keeps its base size and is drawn scaled.</summary>
    CanvasItems = 1,
    /// <summary>Content is rendered at the base size and scaled up.</summary>
    Viewport = 2,
}

/// <summary>What the canvas does when the window's shape differs from the base size's.</summary>
public enum CanvasStretchAspect
{
    /// <summary>Stretch each axis on its own.</summary>
    Ignore = 0,
    /// <summary>Keep the shape and add bars.</summary>
    Keep = 1,
    /// <summary>Keep the width, grow the height.</summary>
    KeepWidth = 2,
    /// <summary>Keep the height, grow the width.</summary>
    KeepHeight = 3,
    /// <summary>Grow whichever axis is needed.</summary>
    Expand = 4,
}

/// <summary>Whether the stretch scale may be fractional.</summary>
public enum CanvasScaleMode
{
    /// <summary>Any scale.</summary>
    Fractional = 0,
    /// <summary>Whole multiples of the base size.</summary>
    Integer = 1,
}

/// <summary>Position, rotation, scale and z-index of a 2D entity. A zero scale reads as 1.</summary>
public struct Transform2D
{
    /// <summary>Local position, in canvas units.</summary>
    public Vector2 Position;
    /// <summary>Local rotation in radians, clockwise on screen (y points down).</summary>
    public float Rotation;
    /// <summary>Local scale; 0 reads as 1.</summary>
    public Vector2 Scale;
    /// <summary>Draw order within the canvas layer, -4096 to 4096; relative to the parent's unless <see cref="Transform2DFlags.ZAbsolute"/>.</summary>
    public int ZIndex;
    /// <summary>Behaviour bits.</summary>
    public Transform2DFlags Flags;
    /// <summary>0 derives the tree order from the hierarchy; otherwise the caller's own tree index (the shared sort key's last part).</summary>
    public uint TreeOrder;
}

/// <summary>A sprite: a texture, or a region or frame of it, drawn at the entity's transform.</summary>
public struct Sprite2D
{
    /// <summary>Asset id of the image (<c>Assets.ObjectIdOf</c> of its content path); 0 draws nothing.</summary>
    public long Texture;
    /// <summary>Source rectangle in pixels (x, y, width, height); a zero width or height means the whole image.</summary>
    public Vector4 Region;
    /// <summary>Offset of the image from the entity's origin.</summary>
    public Vector2 Offset;
    /// <summary>Straight RGBA multiplier, 0xAABBGGRR; 0 means white.</summary>
    public uint Modulate;
    /// <summary>Behaviour bits.</summary>
    public Sprite2DFlags Flags;
    /// <summary>Columns of the frame grid inside the region; 0 means 1.</summary>
    public int HFrames;
    /// <summary>Rows of the frame grid; 0 means 1.</summary>
    public int VFrames;
    /// <summary>Which cell of the grid to draw; wraps.</summary>
    public int Frame;
    /// <summary>How it combines with what is behind it.</summary>
    public Sprite2DBlend Blend;
}

/// <summary>A canvas layer: everything below it (the nearest ancestor-or-self layer) sorts by <see cref="Index"/> and takes its transform.</summary>
public struct Layer2D
{
    /// <summary>The layer index; higher draws over lower. Entities under no layer are in layer 0.</summary>
    public int Index;
    /// <summary>Behaviour bits.</summary>
    public Layer2DFlags Flags;
    /// <summary>Translation of everything in the layer.</summary>
    public Vector2 Offset;
    /// <summary>Rotation in radians.</summary>
    public float Rotation;
    /// <summary>Scale; 0 reads as 1.</summary>
    public Vector2 Scale;
}

/// <summary>Which of the three components.</summary>
public enum Scene2DComponent
{
    /// <summary><see cref="Transform2D"/>.</summary>
    Transform = 0,
    /// <summary><see cref="Sprite2D"/>.</summary>
    Sprite = 1,
    /// <summary><see cref="Layer2D"/>.</summary>
    Layer = 2,
}

/// <summary>Resolves a sprite's asset id to the texture the UI draw list knows and its size in pixels. False skips the sprite this frame.</summary>
public delegate bool Scene2DTextureResolver(long assetId, out ulong texture, out int width, out int height);

/// <summary>2D scenes: sprites, 2D transforms and layers as entity components, the stretch transform and the ordered canvas that
/// sprites and external draws (a UI control's) share. Zero in a field means its default throughout.</summary>
public static class Scene2D
{
    /// <summary>The ABI version this binding was written against.</summary>
    public const int AbiVersion = 1;

    private static int s_state;   // 0 unknown, 1 usable, -1 not (an older DLL or a layout mismatch)

    /// <summary>True when the native library is the version this binding speaks and its structs match the managed ones.</summary>
    public static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try
                {
                    s_state = Scene2DNative.aver_scene2d_abi_version() == AbiVersion
                        && Scene2DNative.aver_scene2d_struct_size(0) == Marshal.SizeOf<Scene2DNative.AverScene2dTransform>()
                        && Scene2DNative.aver_scene2d_struct_size(1) == Marshal.SizeOf<Scene2DNative.AverScene2dSprite>()
                        && Scene2DNative.aver_scene2d_struct_size(2) == Marshal.SizeOf<Scene2DNative.AverScene2dLayer>() ? 1 : -1;
                }
                catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { s_state = -1; }
            }
            return s_state > 0;
        }
    }

    /// <summary>Registers the three components with the scene (idempotent). Call once before attaching.</summary>
    public static bool Register() => Available && Scene2DNative.aver_scene2d_register() != 0;

    /// <summary>The scene component id of one of the three, for the generic field API; 0 when not registered.</summary>
    public static int ComponentId(Scene2DComponent which) => Available ? Scene2DNative.aver_scene2d_component_id((int)which) : 0;

    /// <summary>Adds the component if the entity lacks it and writes the state. False for a stale entity or a non-finite value.</summary>
    public static bool Set(Entity e, in Transform2D t)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dTransform
        {
            position0 = t.Position.X, position1 = t.Position.Y, rotation = t.Rotation, scale0 = t.Scale.X, scale1 = t.Scale.Y,
            zIndex = t.ZIndex, flags = (uint)t.Flags, treeOrder = t.TreeOrder,
        };
        return Scene2DNative.aver_scene2d_set_transform(e.Handle, ref n) != 0;
    }

    /// <summary>Adds the component if the entity lacks it and writes the state. False for a stale entity or a non-finite value.</summary>
    public static bool Set(Entity e, in Sprite2D s)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dSprite
        {
            texture = s.Texture, region0 = s.Region.X, region1 = s.Region.Y, region2 = s.Region.Z, region3 = s.Region.W,
            offset0 = s.Offset.X, offset1 = s.Offset.Y, modulate = s.Modulate, flags = (uint)s.Flags,
            hframes = s.HFrames, vframes = s.VFrames, frame = s.Frame, blend = (int)s.Blend,
        };
        return Scene2DNative.aver_scene2d_set_sprite(e.Handle, ref n) != 0;
    }

    /// <summary>Adds the component if the entity lacks it and writes the state. False for a stale entity or a non-finite value.</summary>
    public static bool Set(Entity e, in Layer2D l)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dLayer
        {
            index = l.Index, flags = (uint)l.Flags, offset0 = l.Offset.X, offset1 = l.Offset.Y, rotation = l.Rotation,
            scale0 = l.Scale.X, scale1 = l.Scale.Y,
        };
        return Scene2DNative.aver_scene2d_set_layer(e.Handle, ref n) != 0;
    }

    /// <summary>Reads the transform. False when the entity has none.</summary>
    public static bool TryGet(Entity e, out Transform2D t)
    {
        t = default;
        if (!Available || Scene2DNative.aver_scene2d_get_transform(e.Handle, out var n) == 0) return false;
        t = new Transform2D
        {
            Position = new Vector2(n.position0, n.position1), Rotation = n.rotation, Scale = new Vector2(n.scale0, n.scale1),
            ZIndex = n.zIndex, Flags = (Transform2DFlags)n.flags, TreeOrder = n.treeOrder,
        };
        return true;
    }

    /// <summary>Reads the sprite. False when the entity has none.</summary>
    public static bool TryGet(Entity e, out Sprite2D s)
    {
        s = default;
        if (!Available || Scene2DNative.aver_scene2d_get_sprite(e.Handle, out var n) == 0) return false;
        s = new Sprite2D
        {
            Texture = n.texture, Region = new Vector4(n.region0, n.region1, n.region2, n.region3), Offset = new Vector2(n.offset0, n.offset1),
            Modulate = n.modulate, Flags = (Sprite2DFlags)n.flags, HFrames = n.hframes, VFrames = n.vframes, Frame = n.frame,
            Blend = (Sprite2DBlend)n.blend,
        };
        return true;
    }

    /// <summary>Reads the layer. False when the entity has none.</summary>
    public static bool TryGet(Entity e, out Layer2D l)
    {
        l = default;
        if (!Available || Scene2DNative.aver_scene2d_get_layer(e.Handle, out var n) == 0) return false;
        l = new Layer2D
        {
            Index = n.index, Flags = (Layer2DFlags)n.flags, Offset = new Vector2(n.offset0, n.offset1), Rotation = n.rotation,
            Scale = new Vector2(n.scale0, n.scale1),
        };
        return true;
    }

    /// <summary>Removes one of the components. True when it was there.</summary>
    public static bool Remove(Entity e, Scene2DComponent which) => Available && Scene2DNative.aver_scene2d_remove(e.Handle, (int)which) != 0;

    /// <summary>Recomposes the 2D transforms now (the canvas frame does it too). Returns how many entities changed.</summary>
    public static int UpdateTransforms() => Available ? Scene2DNative.aver_scene2d_update_transforms() : 0;

    /// <summary>The composed world transform (canvas units; x' = M11 x + M21 y + M31) and effective z of a 2D entity.</summary>
    public static bool TryGetWorld(Entity e, out Matrix3x2 world, out int z)
    {
        world = Matrix3x2.Identity;
        z = 0;
        var a = new float[6];
        if (!Available || Scene2DNative.aver_scene2d_world_affine(e.Handle, a, out z) == 0) return false;
        world = new Matrix3x2(a[0], a[1], a[2], a[3], a[4], a[5]);
        return true;
    }

    /// <summary>The sprite's axis-aligned bounds (x, y, width, height) in world 2D space. False when it has no sprite or its image size is unknown.</summary>
    public static bool TryGetSpriteBounds(Entity e, out Vector4 bounds)
    {
        bounds = default;
        var r = new float[4];
        if (!Available || Scene2DNative.aver_scene2d_sprite_bounds(e.Handle, r) == 0) return false;
        bounds = new Vector4(r[0], r[1], r[2], r[3]);
        return true;
    }

    /// <summary>Sets the viewport stretch and the window rectangle (x, y, width, height) the game is drawn in. A rectangle with no size clears the canvas transform.</summary>
    public static void SetCanvas(CanvasStretchMode mode, CanvasStretchAspect aspect, CanvasScaleMode scaleMode, float baseWidth, float baseHeight,
                                 float scale, Vector4 view)
    {
        if (!Available) return;
        Scene2DNative.aver_scene2d_set_canvas(new float[] { (float)mode, (float)aspect, (float)scaleMode, baseWidth, baseHeight, scale },
                                              view.X, view.Y, view.Z, view.W);
    }

    /// <summary>The canvas transform, content units to window pixels. False (identity) until a canvas with a size is set.</summary>
    public static bool TryGetCanvasTransform(out Matrix3x2 toWindow)
    {
        toWindow = Matrix3x2.Identity;
        var a = new float[6];
        if (!Available) return false;
        int ok = Scene2DNative.aver_scene2d_canvas_transform(a, null);
        toWindow = new Matrix3x2(a[0], a[1], a[2], a[3], a[4], a[5]);
        return ok != 0;
    }

    // The native side keeps raw function pointers, so the delegates behind them live in static fields.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int NativeTextureFn(long assetId, out ulong texture, IntPtr widthHeight, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void NativeDrawFn(IntPtr drawList, IntPtr user);

    private static NativeTextureFn? s_textureThunk;
    private static Scene2DTextureResolver? s_resolver;
    private static readonly NativeDrawFn s_drawThunk = DrawThunk;
    private static readonly Dictionary<long, Action> s_pending = new();
    private static long s_nextToken;

    /// <summary>Installs the host's image lookup (null clears it, and sprites draw nothing). The engine hosts install their own.</summary>
    public static void SetTextureResolver(Scene2DTextureResolver? resolver)
    {
        if (!Available) return;
        s_resolver = resolver;
        if (resolver is null)
        {
            Scene2DNative.aver_scene2d_set_texture_resolver(IntPtr.Zero, IntPtr.Zero);
            s_textureThunk = null;
            return;
        }
        s_textureThunk = (long id, out ulong texture, IntPtr wh, IntPtr user) =>
        {
            texture = 0;
            if (s_resolver is null || !s_resolver(id, out texture, out int w, out int h)) return 0;
            Marshal.WriteInt32(wh, 0, w);
            Marshal.WriteInt32(wh, 4, h);
            return 1;
        };
        Scene2DNative.aver_scene2d_set_texture_resolver(Marshal.GetFunctionPointerForDelegate(s_textureThunk), IntPtr.Zero);
    }

    private static void DrawThunk(IntPtr drawList, IntPtr user)
    {
        Action? draw;
        lock (s_pending) { if (!s_pending.Remove((long)user, out draw)) return; }
        draw();
    }

    /// <summary>Queues an external draw into the canvas's ordered list. Sprites and external draws sort together by
    /// (<paramref name="layer"/>, <paramref name="z"/>, <paramref name="order"/>), lower first, submission order breaking ties.
    /// <paramref name="draw"/> runs at its turn during the flush and draws through the UI draw calls; with
    /// <paramref name="inCanvas"/> it draws in content units under the stretch transform.</summary>
    public static bool SubmitExternal(int layer, int z, long order, Action draw, bool inCanvas = false)
    {
        ArgumentNullException.ThrowIfNull(draw);
        if (!Available) return false;
        long token;
        lock (s_pending) { token = ++s_nextToken; s_pending[token] = draw; }
        if (Scene2DNative.aver_scene2d_submit_external(layer, z, order, Marshal.GetFunctionPointerForDelegate(s_drawThunk), (IntPtr)token, inCanvas ? 1u : 0u) != 0)
            return true;
        lock (s_pending) s_pending.Remove(token);
        return false;
    }

    /// <summary>Items waiting for the next flush.</summary>
    public static int Pending => Available ? Scene2DNative.aver_scene2d_pending() : 0;

    /// <summary>Sorts the queue and draws it into the UI draw list (the pointer <c>aver_ui_draw_list</c> returns); a zero pointer drops the queue.</summary>
    public static void Flush(IntPtr drawList)
    {
        if (!Available) return;
        Scene2DNative.aver_scene2d_flush(drawList);
        if (drawList == IntPtr.Zero) lock (s_pending) s_pending.Clear();
    }

    /// <summary>Recomposes the 2D transforms, queues every visible sprite and flushes. The host calls this once a frame; returns the sprites drawn.</summary>
    public static int Frame(IntPtr drawList) => Available ? Scene2DNative.aver_scene2d_frame(drawList) : 0;
}
