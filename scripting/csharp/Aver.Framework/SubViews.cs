// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Render-to-texture views a script or a converted level asks for. Design: R5 of https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md.

namespace Aver.Framework;

/// <summary>When a view is drawn. The numbers are the C ABI's <c>AVER_FW_SUBVIEW_UPDATE_*</c>.</summary>
public enum SubViewUpdate
{
    /// <summary>Never (a render asked for with <see cref="SubViews.RequestRender"/> still happens once).</summary>
    Disabled = 0,
    /// <summary>One picture, then the view reads as <see cref="Disabled"/>.</summary>
    Once = 1,
    /// <summary>Only while something shows the texture (a material, the UI, the screen).</summary>
    WhenVisible = 2,
    /// <summary>While the view's parent is visible (<see cref="SubViews.SetParentVisible"/>).</summary>
    WhenParentVisible = 3,
    /// <summary>Every frame.</summary>
    Always = 4,
}

/// <summary>Whether the target is cleared before a picture. The numbers are the C ABI's <c>AVER_FW_SUBVIEW_CLEAR_*</c>.</summary>
public enum SubViewClear
{
    Always = 0,
    Never = 1,
    Once = 2,
}

/// <summary>What the host knows about a view: its modes and the size it is actually drawn at.</summary>
public readonly struct SubViewInfo
{
    public SubViewUpdate Update { get; init; }
    public SubViewClear Clear { get; init; }
    public bool Transparent { get; init; }
    /// <summary>The camera entity handle, 0 for none.</summary>
    public int CameraEntity { get; init; }
    /// <summary>The size drawn: the requested size times the 3D scale, fitted to the pixel budget.</summary>
    public int Width { get; init; }
    public int Height { get; init; }
    /// <summary>Pictures drawn so far.</summary>
    public int Renders { get; init; }
    /// <summary>Changes when the texture is rebuilt; a consumer holding the old <see cref="SubViews.Texture"/> must read it again.</summary>
    public int Generation { get; init; }
}

/// <summary>
/// Sub-viewports: extra views of the world drawn into textures that materials and the UI show.
/// </summary>
/// <remarks>
/// <para>A view is named by a KEY, a string that is the same on every load (the node path of the viewport in its scene is what an importer
/// writes), so a material baked with <c>@rt:viewport.&lt;key&gt;</c> finds its texture. Creating a key that exists returns its id. The engine keeps a small
/// budget (4 views alive, 2 drawn per frame unless the project's <c>RENDER.SUBVIEWPORTS</c> / <c>RENDER.SUBVIEWRENDERS</c> say otherwise); a refused
/// create returns 0. A view is drawn by the cheap raster configuration, not the main renderer: no global illumination, shadows or post chain, flat
/// material colours. Calls return false instead of throwing, and without a host they do nothing.</para>
/// </remarks>
public static class SubViews
{
    public const int DefaultMaxViews = 4;
    public const int DefaultRendersPerFrame = 2;

    private const int ConsumerMaterial = 0, ConsumerUi = 1;

    /// <summary>Makes the view named <paramref name="key"/> or returns the one that exists. 0 when refused.</summary>
    public static int Create(string key, int width, int height) => string.IsNullOrEmpty(key) ? 0 : FwSubView.aver_fw_subview_create(key, width, height);

    public static bool Destroy(int id) => id > 0 && FwSubView.aver_fw_subview_destroy(id) != 0;

    /// <summary>The id of the view with this key, or 0.</summary>
    public static int Find(string key) => string.IsNullOrEmpty(key) ? 0 : FwSubView.aver_fw_subview_find(key);

    public static bool SetSize(int id, int width, int height) => FwSubView.aver_fw_subview_set_size(id, width, height) != 0;

    /// <summary>Setting <see cref="SubViewUpdate.Once"/> asks for one render now.</summary>
    public static bool SetUpdate(int id, SubViewUpdate mode) => FwSubView.aver_fw_subview_set_update(id, (int)mode) != 0;

    public static bool SetClear(int id, SubViewClear mode) => FwSubView.aver_fw_subview_set_clear(id, (int)mode) != 0;

    /// <summary>A transparent view clears to alpha 0.</summary>
    public static bool SetTransparent(int id, bool on) => FwSubView.aver_fw_subview_set_transparent(id, on ? 1 : 0) != 0;

    /// <summary>Carried only: the engine has one world.</summary>
    public static bool SetOwnWorld(int id, bool on) => FwSubView.aver_fw_subview_set_own_world(id, on ? 1 : 0) != 0;

    /// <summary>The entity whose camera component and world transform draw the view; 0 clears it (a view with no camera is not drawn).</summary>
    public static bool SetCamera(int id, int entityHandle) => FwSubView.aver_fw_subview_set_camera(id, entityHandle) != 0;

    /// <summary>Multiplies the drawn size (0.25 to 8); the multisample count is stored, not used.</summary>
    public static bool SetScale(int id, float scale, int msaa = 0) => FwSubView.aver_fw_subview_set_scale(id, scale, msaa) != 0;

    public static bool SetParentVisible(int id, bool visible) => FwSubView.aver_fw_subview_set_parent_visible(id, visible ? 1 : 0) != 0;

    /// <summary>The colour the target is cleared to (display values 0 to 1).</summary>
    public static bool SetClearColor(int id, float r, float g, float b, float a = 1f) => FwSubView.aver_fw_subview_set_clear_color(id, r, g, b, a) != 0;

    /// <summary>Counts a material that shows the view's texture; a <see cref="SubViewUpdate.WhenVisible"/> view is drawn only while there is one.</summary>
    public static bool AddMaterialConsumer(int id) => FwSubView.aver_fw_subview_consumer(id, ConsumerMaterial, +1) != 0;
    public static bool RemoveMaterialConsumer(int id) => FwSubView.aver_fw_subview_consumer(id, ConsumerMaterial, -1) != 0;

    /// <summary>Counts a piece of UI that shows the view's texture.</summary>
    public static bool AddUiConsumer(int id) => FwSubView.aver_fw_subview_consumer(id, ConsumerUi, +1) != 0;
    public static bool RemoveUiConsumer(int id) => FwSubView.aver_fw_subview_consumer(id, ConsumerUi, -1) != 0;

    /// <summary>Asks for one more picture whatever the update mode says.</summary>
    public static bool RequestRender(int id) => FwSubView.aver_fw_subview_request_render(id) != 0;

    /// <summary>Shows the view over the window, a rectangle in the UI's own pixels (a SubViewportContainer); <paramref name="visible"/> false hides it.</summary>
    public static bool SetScreenRect(int id, float x, float y, float width, float height, bool visible) =>
        FwSubView.aver_fw_subview_set_screen_rect(id, x, y, width, height, visible ? 1 : 0) != 0;

    /// <summary>False for an unknown id.</summary>
    public static bool TryGetInfo(int id, out SubViewInfo info)
    {
        var v = new int[8];
        if (id <= 0 || FwSubView.aver_fw_subview_info(id, v) == 0) { info = default; return false; }
        info = new SubViewInfo
        {
            Update = (SubViewUpdate)v[0], Clear = (SubViewClear)v[1], Transparent = v[2] != 0, CameraEntity = v[3],
            Width = v[4], Height = v[5], Renders = v[6], Generation = v[7],
        };
        return true;
    }

    /// <summary>The host's texture (an engine texture handle) for the view, 0 until a picture has been drawn. Hand it to a UI image widget.</summary>
    public static ulong Texture(int id) => id > 0 ? FwSubView.aver_fw_subview_texture(id) : 0;
}
