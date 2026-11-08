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

    // AverScene2dAnimFrame.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dAnimFrame
    {
        public long texture;
        public float region0, region1, region2, region3;
        public float weight;
        public int cell;
    }

    // AverScene2dAnimState.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dAnimState
    {
        public float speedScale;
        public uint flags;
        public int clip;
        public int autoplay;
        public int frame;
        public float progress;
        public int clipCount;
        public int frameCount;
    }

    // AverScene2dAnimEvent.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dAnimEvent
    {
        public int entity;
        public int kind;
        public int clip;
        public int frame;
    }

    // AverScene2dCamera.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dCamera
    {
        public float zoom0, zoom1;
        public float offset0, offset1;
        public float limit0, limit1, limit2, limit3;
        public float smoothSpeed;
        public float drag0, drag1, drag2, drag3;
        public uint flags;
    }

    // AverScene2dParallax.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AverScene2dParallax
    {
        public float scrollScale0, scrollScale1;
        public float scrollOffset0, scrollOffset1;
        public float repeatSize0, repeatSize1;
        public int repeatTimes;
        public float autoscroll0, autoscroll1;
        public float limitBegin0, limitBegin1;
        public float limitEnd0, limitEnd1;
        public uint flags;
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
    [DllImport(Lib)] internal static extern int aver_scene2d_set_camera(int entity, ref AverScene2dCamera desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_camera(int entity, out AverScene2dCamera outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_set_parallax(int entity, ref AverScene2dParallax desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_parallax(int entity, out AverScene2dParallax outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_camera_make_current(int entity);
    [DllImport(Lib)] internal static extern int aver_scene2d_camera_current();
    [DllImport(Lib)] internal static extern int aver_scene2d_camera_center(int entity, float[] outXY);
    [DllImport(Lib)] internal static extern int aver_scene2d_camera_view(float[] out6);
    [DllImport(Lib)] internal static extern int aver_scene2d_set_anim(int entity, ref AverScene2dAnimState desc);
    [DllImport(Lib)] internal static extern int aver_scene2d_get_anim(int entity, out AverScene2dAnimState outDesc);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_set_clip(int entity, int index, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, float fps, int loop, AverScene2dAnimFrame[]? frames, int count);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_get_clip(int entity, int index, byte[]? outName, float[]? outFps, int[]? outLoop, int[]? outFrameCount);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_get_frames(int entity, int index, AverScene2dAnimFrame[]? outFrames, int max);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_find(int entity, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_play(int entity, [MarshalAs(UnmanagedType.LPUTF8Str)] string? name, int restart);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_stop(int entity);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_pause(int entity);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_set_frame(int entity, int frame, float progress);
    [DllImport(Lib)] internal static extern int aver_scene2d_anim_poll(AverScene2dAnimEvent[] outEvents, int max);
    [DllImport(Lib)] internal static extern void aver_scene2d_tick(float dt);
    [DllImport(Lib)] internal static extern int aver_scene2d_remove(int entity, int which);
    [DllImport(Lib)] internal static extern int aver_scene2d_update_transforms();
    [DllImport(Lib)] internal static extern int aver_scene2d_world_affine(int entity, float[] outAffine6, out int outZ);
    [DllImport(Lib)] internal static extern int aver_scene2d_sprite_bounds(int entity, float[] outRect4);
    [DllImport(Lib)] internal static extern void aver_scene2d_set_canvas(float[]? in6, float viewX, float viewY, float viewW, float viewH);
    [DllImport(Lib)] internal static extern void aver_scene2d_set_canvas_view(float viewX, float viewY, float viewW, float viewH);
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
    /// <summary>The current <see cref="Camera2D"/>'s view applies to this layer (the default canvas, under no layer, always follows it).
    /// A layer without it is fixed to the screen, like a UI layer.</summary>
    FollowViewport = 2,
}

/// <summary>Flags of <see cref="Camera2D"/>. None is an enabled camera centred on its transform that ignores rotation.</summary>
[Flags]
public enum Camera2DFlags : uint
{
    /// <summary>None.</summary>
    None = 0,
    /// <summary>Never the current camera.</summary>
    Disabled = 1,
    /// <summary>Preferred as the current camera; <see cref="Scene2D.MakeCurrent"/> sets it and clears it on the others.</summary>
    Current = 2,
    /// <summary>The view turns with the camera's transform.</summary>
    FollowRotation = 4,
    /// <summary>The centre eases toward its goal at <see cref="Camera2D.SmoothSpeed"/>.</summary>
    Smoothing = 8,
    /// <summary>Horizontal drag margins are active.</summary>
    DragHorizontal = 16,
    /// <summary>Vertical drag margins are active.</summary>
    DragVertical = 32,
    /// <summary>The camera's position is the view's top-left corner, not its centre.</summary>
    AnchorTopLeft = 64,
    /// <summary><see cref="Camera2D.Limit"/>.X applies.</summary>
    LimitLeft = 128,
    /// <summary><see cref="Camera2D.Limit"/>.Y applies.</summary>
    LimitTop = 256,
    /// <summary><see cref="Camera2D.Limit"/>.Z applies.</summary>
    LimitRight = 512,
    /// <summary><see cref="Camera2D.Limit"/>.W applies.</summary>
    LimitBottom = 1024,
}

/// <summary>Flags of <see cref="Parallax2D"/>.</summary>
[Flags]
public enum Parallax2DFlags : uint
{
    /// <summary>None: follows the camera.</summary>
    None = 0,
    /// <summary>Does not follow the camera: the layer stays in screen space.</summary>
    NoFollow = 1,
    /// <summary><see cref="Parallax2D.LimitBegin"/> clamps the camera position the scroll uses.</summary>
    LimitBegin = 2,
    /// <summary><see cref="Parallax2D.LimitEnd"/> clamps the camera position the scroll uses.</summary>
    LimitEnd = 4,
}

/// <summary>What a sprite animation reported.</summary>
public enum SpriteAnimEventKind
{
    /// <summary>The shown frame changed.</summary>
    FrameChanged = 0,
    /// <summary>A clip that does not loop reached its end and stopped.</summary>
    Finished = 1,
    /// <summary>A looping clip wrapped around.</summary>
    Looped = 2,
    /// <summary>A different clip started.</summary>
    AnimationChanged = 3,
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

/// <summary>One frame of a sprite animation.</summary>
public struct SpriteAnimFrame2D
{
    /// <summary>Asset id of the image; 0 keeps the sprite's own texture (a frame that is a cell of its sheet).</summary>
    public long Texture;
    /// <summary>Source rectangle in pixels (x, y, width, height); a zero width or height means the whole image.</summary>
    public Vector4 Region;
    /// <summary>Relative length, 0 reads as 1: the frame lasts Weight / (fps * speed scale) seconds.</summary>
    public float Weight;
    /// <summary>The sprite's <see cref="Sprite2D.HFrames"/> x <see cref="Sprite2D.VFrames"/> cell while this frame shows.</summary>
    public int Cell;
}

/// <summary>Playback settings and state of the sprite animation on an entity. The clips are defined with <see cref="Scene2D.SetClip"/>.</summary>
public struct SpriteAnim2D
{
    /// <summary>Playback speed; 0 reads as 1, negative plays backwards.</summary>
    public float SpeedScale;
    /// <summary>Clip index + 1 that starts playing on the first frame; 0 for none.</summary>
    public int Autoplay;
    /// <summary>Read only: the clip is advancing.</summary>
    public bool Playing;
    /// <summary>Read only: index of the current clip, -1 when there are none.</summary>
    public int Clip;
    /// <summary>Read only: frame within the current clip.</summary>
    public int Frame;
    /// <summary>Read only: 0 to 1 through that frame.</summary>
    public float Progress;
    /// <summary>Read only: clips defined.</summary>
    public int ClipCount;
    /// <summary>Read only: frames defined across all the clips.</summary>
    public int FrameCount;
}

/// <summary>A sprite animation event: <paramref name="Entity"/> had <paramref name="Kind"/> happen at <paramref name="Clip"/>, <paramref name="Frame"/>.</summary>
public readonly record struct SpriteAnimEvent2D(Entity Entity, SpriteAnimEventKind Kind, int Clip, int Frame);

/// <summary>A 2D camera on an entity that has a <see cref="Transform2D"/>. One is current at a time. Zero in a field is its default.</summary>
public struct Camera2D
{
    /// <summary>Zoom; 0 reads as 1, larger shows less.</summary>
    public Vector2 Zoom;
    /// <summary>Added to the entity's position to get the view centre.</summary>
    public Vector2 Offset;
    /// <summary>Left, top, right, bottom in world units; each applies only with its limit flag.</summary>
    public Vector4 Limit;
    /// <summary>Easing rate with <see cref="Camera2DFlags.Smoothing"/>; 0 reads as 5.</summary>
    public float SmoothSpeed;
    /// <summary>Left, top, right, bottom drag margins as fractions of the half view; used with the drag flags.</summary>
    public Vector4 Drag;
    /// <summary>Behaviour bits.</summary>
    public Camera2DFlags Flags;
}

/// <summary>Parallax scrolling for the sprites below an entity (Godot's Parallax2D). Unlike the other components a zero scroll scale
/// pins the layer to the screen: <see cref="ScrollScale"/> is stored as given, so start from <see cref="Scene2D.DefaultParallax"/>.</summary>
public struct Parallax2D
{
    /// <summary>1 scrolls with the world, 0 stays on screen, between is farther away.</summary>
    public Vector2 ScrollScale;
    /// <summary>Added to the scroll.</summary>
    public Vector2 ScrollOffset;
    /// <summary>Tile size per axis; 0 means no tiling on that axis.</summary>
    public Vector2 RepeatSize;
    /// <summary>Tiles on each side of the original; 0 reads as 1.</summary>
    public int RepeatTimes;
    /// <summary>World units per second the layer drifts.</summary>
    public Vector2 Autoscroll;
    /// <summary>With <see cref="Parallax2DFlags.LimitBegin"/>: the camera position the scroll uses is not lower than this.</summary>
    public Vector2 LimitBegin;
    /// <summary>With <see cref="Parallax2DFlags.LimitEnd"/>: the camera position the scroll uses is not higher than this.</summary>
    public Vector2 LimitEnd;
    /// <summary>Behaviour bits.</summary>
    public Parallax2DFlags Flags;
}

/// <summary>Which of the six components.</summary>
public enum Scene2DComponent
{
    /// <summary><see cref="Transform2D"/>.</summary>
    Transform = 0,
    /// <summary><see cref="Sprite2D"/>.</summary>
    Sprite = 1,
    /// <summary><see cref="Layer2D"/>.</summary>
    Layer = 2,
    /// <summary><see cref="SpriteAnim2D"/> and its clips.</summary>
    SpriteAnim = 3,
    /// <summary><see cref="Camera2D"/>.</summary>
    Camera = 4,
    /// <summary><see cref="Parallax2D"/>.</summary>
    Parallax = 5,
}

/// <summary>Resolves a sprite's asset id to the texture the UI draw list knows and its size in pixels. False skips the sprite this frame.</summary>
public delegate bool Scene2DTextureResolver(long assetId, out ulong texture, out int width, out int height);

/// <summary>2D scenes: sprites, 2D transforms and layers as entity components, the stretch transform and the ordered canvas that
/// sprites and external draws (a UI control's) share. Zero in a field means its default throughout.</summary>
public static class Scene2D
{
    /// <summary>The ABI version this binding was written against.</summary>
    public const int AbiVersion = 3;

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
                        && Scene2DNative.aver_scene2d_struct_size(2) == Marshal.SizeOf<Scene2DNative.AverScene2dLayer>()
                        && Scene2DNative.aver_scene2d_struct_size(3) == Marshal.SizeOf<Scene2DNative.AverScene2dAnimState>()
                        && Scene2DNative.aver_scene2d_struct_size(4) == Marshal.SizeOf<Scene2DNative.AverScene2dCamera>()
                        && Scene2DNative.aver_scene2d_struct_size(5) == Marshal.SizeOf<Scene2DNative.AverScene2dParallax>()
                        && Scene2DNative.aver_scene2d_struct_size(6) == Marshal.SizeOf<Scene2DNative.AverScene2dAnimFrame>()
                        && Scene2DNative.aver_scene2d_struct_size(7) == Marshal.SizeOf<Scene2DNative.AverScene2dAnimEvent>() ? 1 : -1;
                }
                catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { s_state = -1; }
            }
            return s_state > 0;
        }
    }

    /// <summary>Registers the six components with the scene (idempotent). Call once before attaching.</summary>
    public static bool Register() => Available && Scene2DNative.aver_scene2d_register() != 0;

    /// <summary>The scene component id of one of the six, for the generic field API; 0 when not registered.</summary>
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

    /// <summary>Parallax settings that scroll with the world, as Godot's defaults: scroll scale 1, no tiling.</summary>
    public static Parallax2D DefaultParallax => new() { ScrollScale = Vector2.One };

    /// <summary>Adds the component if the entity lacks it and writes the state. False for a stale entity or a non-finite value.</summary>
    public static bool Set(Entity e, in Camera2D c)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dCamera
        {
            zoom0 = c.Zoom.X, zoom1 = c.Zoom.Y, offset0 = c.Offset.X, offset1 = c.Offset.Y,
            limit0 = c.Limit.X, limit1 = c.Limit.Y, limit2 = c.Limit.Z, limit3 = c.Limit.W, smoothSpeed = c.SmoothSpeed,
            drag0 = c.Drag.X, drag1 = c.Drag.Y, drag2 = c.Drag.Z, drag3 = c.Drag.W, flags = (uint)c.Flags,
        };
        return Scene2DNative.aver_scene2d_set_camera(e.Handle, ref n) != 0;
    }

    /// <summary>Reads the camera. False when the entity has none.</summary>
    public static bool TryGet(Entity e, out Camera2D c)
    {
        c = default;
        if (!Available || Scene2DNative.aver_scene2d_get_camera(e.Handle, out var n) == 0) return false;
        c = new Camera2D
        {
            Zoom = new Vector2(n.zoom0, n.zoom1), Offset = new Vector2(n.offset0, n.offset1),
            Limit = new Vector4(n.limit0, n.limit1, n.limit2, n.limit3), SmoothSpeed = n.smoothSpeed,
            Drag = new Vector4(n.drag0, n.drag1, n.drag2, n.drag3), Flags = (Camera2DFlags)n.flags,
        };
        return true;
    }

    /// <summary>Adds the component if the entity lacks it and writes the state. False for a stale entity, a non-finite value or a negative repeat.</summary>
    public static bool Set(Entity e, in Parallax2D p)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dParallax
        {
            scrollScale0 = p.ScrollScale.X, scrollScale1 = p.ScrollScale.Y, scrollOffset0 = p.ScrollOffset.X, scrollOffset1 = p.ScrollOffset.Y,
            repeatSize0 = p.RepeatSize.X, repeatSize1 = p.RepeatSize.Y, repeatTimes = p.RepeatTimes,
            autoscroll0 = p.Autoscroll.X, autoscroll1 = p.Autoscroll.Y, limitBegin0 = p.LimitBegin.X, limitBegin1 = p.LimitBegin.Y,
            limitEnd0 = p.LimitEnd.X, limitEnd1 = p.LimitEnd.Y, flags = (uint)p.Flags,
        };
        return Scene2DNative.aver_scene2d_set_parallax(e.Handle, ref n) != 0;
    }

    /// <summary>Reads the parallax. False when the entity has none.</summary>
    public static bool TryGet(Entity e, out Parallax2D p)
    {
        p = default;
        if (!Available || Scene2DNative.aver_scene2d_get_parallax(e.Handle, out var n) == 0) return false;
        p = new Parallax2D
        {
            ScrollScale = new Vector2(n.scrollScale0, n.scrollScale1), ScrollOffset = new Vector2(n.scrollOffset0, n.scrollOffset1),
            RepeatSize = new Vector2(n.repeatSize0, n.repeatSize1), RepeatTimes = n.repeatTimes,
            Autoscroll = new Vector2(n.autoscroll0, n.autoscroll1), LimitBegin = new Vector2(n.limitBegin0, n.limitBegin1),
            LimitEnd = new Vector2(n.limitEnd0, n.limitEnd1), Flags = (Parallax2DFlags)n.flags,
        };
        return true;
    }

    /// <summary>Makes the entity's camera the current one (clearing the others). False when it has no camera.</summary>
    public static bool MakeCurrent(Entity e) => Available && Scene2DNative.aver_scene2d_camera_make_current(e.Handle) != 0;

    /// <summary>The entity of the camera that applies, or <see cref="Entity.None"/>.</summary>
    public static Entity CurrentCamera => Available ? new Entity(Scene2DNative.aver_scene2d_camera_current()) : default;

    /// <summary>The view centre of the entity's camera in world units after easing and limits. False when it has no camera.</summary>
    public static bool TryGetCameraCenter(Entity e, out Vector2 center)
    {
        center = default;
        var a = new float[2];
        if (!Available || Scene2DNative.aver_scene2d_camera_center(e.Handle, a) == 0) return false;
        center = new Vector2(a[0], a[1]);
        return true;
    }

    /// <summary>The current camera's world to content-units transform. False (identity) with no camera or no canvas.</summary>
    public static bool TryGetCameraView(out Matrix3x2 view)
    {
        view = Matrix3x2.Identity;
        var a = new float[6];
        if (!Available) return false;
        int ok = Scene2DNative.aver_scene2d_camera_view(a);
        view = new Matrix3x2(a[0], a[1], a[2], a[3], a[4], a[5]);
        return ok != 0;
    }

    /// <summary>Writes the playback settings (speed scale and autoplay), adding the component if needed. The clips come from <see cref="SetClip"/>.</summary>
    public static bool Set(Entity e, in SpriteAnim2D a)
    {
        if (!Available) return false;
        var n = new Scene2DNative.AverScene2dAnimState { speedScale = a.SpeedScale, autoplay = a.Autoplay };
        return Scene2DNative.aver_scene2d_set_anim(e.Handle, ref n) != 0;
    }

    /// <summary>Reads the playback settings and state. False when the entity has no sprite animation.</summary>
    public static bool TryGet(Entity e, out SpriteAnim2D a)
    {
        a = default;
        if (!Available || Scene2DNative.aver_scene2d_get_anim(e.Handle, out var n) == 0) return false;
        a = new SpriteAnim2D
        {
            SpeedScale = n.speedScale, Autoplay = n.autoplay, Playing = (n.flags & 1u) != 0, Clip = n.clip, Frame = n.frame,
            Progress = n.progress, ClipCount = n.clipCount, FrameCount = n.frameCount,
        };
        return true;
    }

    /// <summary>The most clips one entity can hold.</summary>
    public const int MaxClips = 16;
    /// <summary>The most frames one entity can hold, across its clips.</summary>
    public const int MaxClipFrames = 128;

    /// <summary>Defines clip <paramref name="index"/> (an index equal to the clip count appends), adding the component if needed. A clip has a
    /// unique name of under 24 UTF-8 bytes; <paramref name="fps"/> 0 reads as 5. False for a bad index or name, a non-finite number, or too many clips or frames.</summary>
    public static bool SetClip(Entity e, int index, string name, float fps, bool loop, ReadOnlySpan<SpriteAnimFrame2D> frames)
    {
        if (!Available) return false;
        var native = new Scene2DNative.AverScene2dAnimFrame[frames.Length];
        for (int i = 0; i < frames.Length; i++)
        {
            ref readonly var f = ref frames[i];
            native[i] = new Scene2DNative.AverScene2dAnimFrame
            {
                texture = f.Texture, region0 = f.Region.X, region1 = f.Region.Y, region2 = f.Region.Z, region3 = f.Region.W, weight = f.Weight, cell = f.Cell,
            };
        }
        return Scene2DNative.aver_scene2d_anim_set_clip(e.Handle, index, name, fps, loop ? 1 : 0, native, native.Length) != 0;
    }

    /// <summary>Reads clip <paramref name="index"/>. False past the last clip.</summary>
    public static bool TryGetClip(Entity e, int index, out string name, out float fps, out bool loop, out SpriteAnimFrame2D[] frames)
    {
        name = string.Empty;
        fps = 0;
        loop = true;
        frames = Array.Empty<SpriteAnimFrame2D>();
        var nameBytes = new byte[24];
        var f = new float[1];
        var l = new int[1];
        var c = new int[1];
        if (!Available || Scene2DNative.aver_scene2d_anim_get_clip(e.Handle, index, nameBytes, f, l, c) == 0) return false;
        int len = Array.IndexOf(nameBytes, (byte)0);
        name = System.Text.Encoding.UTF8.GetString(nameBytes, 0, len < 0 ? nameBytes.Length : len);
        fps = f[0];
        loop = l[0] != 0;
        var native = new Scene2DNative.AverScene2dAnimFrame[c[0]];
        int got = Scene2DNative.aver_scene2d_anim_get_frames(e.Handle, index, native, native.Length);
        frames = new SpriteAnimFrame2D[Math.Max(got, 0)];
        for (int i = 0; i < frames.Length; i++)
        {
            ref var n = ref native[i];
            frames[i] = new SpriteAnimFrame2D { Texture = n.texture, Region = new Vector4(n.region0, n.region1, n.region2, n.region3), Weight = n.weight, Cell = n.cell };
        }
        return true;
    }

    /// <summary>The index of the clip named <paramref name="name"/>, or -1.</summary>
    public static int FindClip(Entity e, string name) => Available ? Scene2DNative.aver_scene2d_anim_find(e.Handle, name) : -1;

    /// <summary>Plays the clip <paramref name="name"/> (null or empty continues the current one); <paramref name="restart"/> rewinds one that is already playing.
    /// False for an unknown or empty clip.</summary>
    public static bool Play(Entity e, string? name = null, bool restart = false) => Available && Scene2DNative.aver_scene2d_anim_play(e.Handle, name, restart ? 1 : 0) != 0;

    /// <summary>Stops the animation and rewinds it to its first frame.</summary>
    public static bool Stop(Entity e) => Available && Scene2DNative.aver_scene2d_anim_stop(e.Handle) != 0;

    /// <summary>Stops the animation where it is.</summary>
    public static bool Pause(Entity e) => Available && Scene2DNative.aver_scene2d_anim_pause(e.Handle) != 0;

    /// <summary>Shows frame <paramref name="frame"/> of the current clip (clamped), <paramref name="progress"/> of the way through it.</summary>
    public static bool SetFrame(Entity e, int frame, float progress = 0f) => Available && Scene2DNative.aver_scene2d_anim_set_frame(e.Handle, frame, progress) != 0;

    /// <summary>Drains the queued animation events (frame changed, finished, looped, clip changed), oldest first.</summary>
    public static SpriteAnimEvent2D[] PollAnimationEvents(int max = 256)
    {
        if (!Available || max <= 0) return Array.Empty<SpriteAnimEvent2D>();
        var buf = new Scene2DNative.AverScene2dAnimEvent[max];
        int n = Scene2DNative.aver_scene2d_anim_poll(buf, max);
        var list = new SpriteAnimEvent2D[Math.Max(n, 0)];
        for (int i = 0; i < list.Length; i++) list[i] = new SpriteAnimEvent2D(new Entity(buf[i].entity), (SpriteAnimEventKind)buf[i].kind, buf[i].clip, buf[i].frame);
        return list;
    }

    /// <summary>One step of everything time-driven: transforms, animations (and their events), parallax autoscroll, camera drag and easing.
    /// The engine hosts call it every frame; a script calls it only when it drives the 2D scene itself.</summary>
    public static void Tick(float dt)
    {
        if (Available) Scene2DNative.aver_scene2d_tick(dt);
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

    /// <summary>Moves the window rectangle (x, y, width, height) the game is drawn in and keeps the stretch rules. Hosts call it each frame with the real viewport.</summary>
    public static void SetCanvasView(Vector4 view)
    {
        if (Available) Scene2DNative.aver_scene2d_set_canvas_view(view.X, view.Y, view.Z, view.W);
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
