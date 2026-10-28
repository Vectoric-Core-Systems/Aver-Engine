// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// SubViewportTests: SubViewport, ViewportTexture and SubViewportContainer (G4 R5). The classes under Godot's names and defaults, what a SubViewport asks the engine
// for (against a recording backend: nothing here touches the native framework), which camera draws it, the texture object and the material reference it
// gives, and the container's rectangle and visibility rules.

using Aver.Fabric;
using Aver.Framework;

static class SubViewportTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("sub-viewport: the classes and Godot's defaults (G4 R5)", Classes);
        T.Run("sub-viewport: what a view asks the engine for (G4 R5)", Registration);
        T.Run("sub-viewport: properties pushed after load, and the key (G4 R5)", Properties);
        T.Run("sub-viewport: the camera, current and the first one below (G4 R5)", Cameras);
        T.Run("sub-viewport: ViewportTexture and the material reference (G4 R5)", Texture);
        T.Run("sub-viewport: SubViewportContainer shows the view (G4 R5)", Container);
        T.Run("sub-viewport: a container laid out by anchors and offsets (G4 R5)", ContainerAnchors);
        T.Run("sub-viewport: no engine, no view (G4 R5)", NoEngine);
    });

    // A backend that records what it is asked, keyed like the engine's (the same key is the same id).
    private sealed class Fake : ISubViewBackend
    {
        public readonly List<string> Calls = new();
        public readonly Dictionary<string, int> Ids = new();
        public readonly Dictionary<int, string> Keys = new();
        public readonly Dictionary<int, SubViewInfo> Infos = new();
        public readonly Dictionary<int, ulong> Textures = new();
        public readonly Dictionary<int, (float X, float Y, float W, float H, bool On)> Screens = new();
        public readonly Dictionary<int, int> MaterialUsers = new(), UiUsers = new();
        public int RefuseAfter = 99;
        public int NextId = 1;

        public int Create(string key, int w, int h)
        {
            Calls.Add($"create {key} {w}x{h}");
            if (Ids.TryGetValue(key, out int existing)) return existing;
            if (Ids.Count >= RefuseAfter) return 0;
            int id = NextId++;
            Ids[key] = id;
            Keys[id] = key;
            Infos[id] = new SubViewInfo { Update = SubViewUpdate.WhenVisible, Width = w, Height = h };
            return id;
        }
        public bool Destroy(int id) { Calls.Add($"destroy {id}"); if (Keys.Remove(id, out string? k)) Ids.Remove(k); return true; }
        public bool SetSize(int id, int w, int h) { Calls.Add($"size {id} {w}x{h}"); return true; }
        public bool SetUpdate(int id, SubViewUpdate m) { Calls.Add($"update {id} {m}"); return true; }
        public bool SetClear(int id, SubViewClear m) { Calls.Add($"clear {id} {m}"); return true; }
        public bool SetTransparent(int id, bool on) { Calls.Add($"transparent {id} {on}"); return true; }
        public bool SetOwnWorld(int id, bool on) { Calls.Add($"ownworld {id} {on}"); return true; }
        public bool SetCamera(int id, int e) { Calls.Add($"camera {id} {e}"); return true; }
        public bool SetScale(int id, float s, int msaa) { Calls.Add($"scale {id} {s} msaa{msaa}"); return true; }
        public bool SetParentVisible(int id, bool on) { Calls.Add($"parentvisible {id} {on}"); return true; }
        public bool SetClearColor(int id, float r, float g, float b, float a) { Calls.Add($"clearcolor {id} {r:0.##} {g:0.##} {b:0.##} {a:0.##}"); return true; }
        public bool AddMaterialConsumer(int id) { MaterialUsers[id] = MaterialUsers.GetValueOrDefault(id) + 1; return true; }
        public bool RemoveMaterialConsumer(int id) { MaterialUsers[id] = Math.Max(0, MaterialUsers.GetValueOrDefault(id) - 1); return true; }
        public bool AddUiConsumer(int id) { UiUsers[id] = UiUsers.GetValueOrDefault(id) + 1; return true; }
        public bool RemoveUiConsumer(int id) { UiUsers[id] = Math.Max(0, UiUsers.GetValueOrDefault(id) - 1); return true; }
        public bool RequestRender(int id) { Calls.Add($"request {id}"); return true; }
        public bool SetScreenRect(int id, float x, float y, float w, float h, bool visible) { Screens[id] = (x, y, w, h, visible); return true; }
        public bool TryGetInfo(int id, out SubViewInfo info) => Infos.TryGetValue(id, out info);
        public ulong Texture(int id) => Textures.GetValueOrDefault(id);
        public int Count(string prefix) => Calls.Count(c => c.StartsWith(prefix, StringComparison.Ordinal));
    }

    private static Fake Install()
    {
        var f = new Fake();
        SubViewBackend.Override = f;
        // A node's "entity" is its position in creation order + 100, so the camera handles are checkable without an engine.
        var made = new Dictionary<Node, int>();
        SubViewBackend.EntityOf = n => { if (!made.TryGetValue(n, out int e)) made[n] = e = 100 + made.Count; return e; };
        return f;
    }

    private static void Done()
    {
        SubViewBackend.Override = null;
        SubViewBackend.EntityOf = n => n.Self.IsValid ? n.Self.Handle : 0;
    }

    private static void Classes()
    {
        foreach (string name in new[] { "SubViewport", "SubViewportContainer", "ViewportTexture", "Viewport", "Texture2D" })
            T.Check($"ClassDB knows {name}", ClassDB.ClassExists(name));
        T.Check("the tree: SubViewport is a Viewport is a Node; ViewportTexture a Texture2D a Resource; the container a Control",
            ClassDB.IsParentClass("SubViewport", "Viewport") && ClassDB.IsParentClass("Viewport", "Node") && ClassDB.IsParentClass("ViewportTexture", "Texture2D") &&
            ClassDB.IsParentClass("Texture2D", "Resource") && ClassDB.IsParentClass("SubViewportContainer", "Control"));
        var v = new SubViewport();
        T.Check("Godot's defaults: 512 by 512, update when visible, clear always, opaque, scale 1, no MSAA, own world off",
            v.Size == new Vector2I(512, 512) && v.RenderTargetUpdateMode == SubViewport.UpdateMode.WhenVisible && v.RenderTargetClearMode == SubViewport.ClearMode.Always &&
            !v.TransparentBg && v.Scaling3DScale == 1f && v.Msaa3D == SubViewport.MsaaMode.Disabled && !v.OwnWorld3D && !v.Disable3D);
        T.Check("the enum numbers are Godot's (and the engine's)",
            (int)SubViewport.UpdateMode.Disabled == 0 && (int)SubViewport.UpdateMode.Once == 1 && (int)SubViewport.UpdateMode.WhenVisible == 2 &&
            (int)SubViewport.UpdateMode.WhenParentVisible == 3 && (int)SubViewport.UpdateMode.Always == 4 &&
            (int)SubViewport.ClearMode.Always == 0 && (int)SubViewport.ClearMode.Never == 1 && (int)SubViewport.ClearMode.Once == 2 &&
            (int)SubViewUpdate.WhenParentVisible == 3 && (int)SubViewClear.Once == 2);
        var c = new SubViewportContainer();
        T.Check("the container's defaults: no stretch, shrink 1", !c.Stretch && c.StretchShrink == 1);
        T.Check("the window's viewport has no texture", SceneTree.Instance.Root.GetTexture() == null);
        T.Check("GetViewport() of a node under a SubViewport is that SubViewport, not the window",
            Build(out SubViewport sv, out Node3D under) && under.GetViewport() == sv);
        T.Check("and a node outside any is the window", ReferenceEquals(new Node3D().GetViewport(), null) && ReferenceEquals(Outside(), SceneTree.Instance.Root));

        static bool Build(out SubViewport sv, out Node3D under)
        {
            sv = new SubViewport();
            under = new Node3D();
            sv.AddChild(under);
            SceneTree.Instance.Root.AddChild(sv);
            return true;
        }
        static Viewport? Outside()
        {
            var n = new Node3D();
            SceneTree.Instance.Root.AddChild(n);
            return n.GetViewport();
        }
    }

    private static void Registration()
    {
        Fake f = Install();
        try
        {
            var sv = new SubViewport { Name = "Screen", ViewportKey = "Main:Main/Screen", Size = new Vector2I(640, 360), RenderTargetUpdateMode = SubViewport.UpdateMode.Always,
                                       RenderTargetClearMode = SubViewport.ClearMode.Never, TransparentBg = true, OwnWorld3D = true, Scaling3DScale = 0.5f };
            T.Check("a view not in the tree has made nothing", f.Calls.Count == 0 && sv.ViewId == 0);
            var root = new Node { Name = "Main" };
            root.AddChild(sv);
            SceneTree.Instance.Root.AddChild(root);
            T.Check("entering the tree creates it under the converter's key at its size", f.Calls.Count > 0 && f.Calls[0] == "create Main:Main/Screen 640x360" && sv.ViewId == 1);
            T.Check("and pushes every property the node holds",
                f.Calls.Contains("update 1 Always") && f.Calls.Contains("clear 1 Never") && f.Calls.Contains("transparent 1 True") && f.Calls.Contains("ownworld 1 True") &&
                f.Calls.Contains("scale 1 0.5 msaa0") && f.Calls.Any(c => c.StartsWith("clearcolor 1 0.3 0.3 0.3 1")));
            T.Check("with no camera below, none is given", f.Calls.Contains("camera 1 0"));
            T.Check("a node's size, update mode, clear mode and so on are pushed when set later",
                Set(() => sv.Size = new Vector2I(320, 180), f, "size 1 320x180") &&
                Set(() => sv.RenderTargetUpdateMode = SubViewport.UpdateMode.Once, f, "update 1 Once") &&
                Set(() => sv.RenderTargetClearMode = SubViewport.ClearMode.Once, f, "clear 1 Once") &&
                Set(() => sv.TransparentBg = false, f, "transparent 1 False") &&
                Set(() => sv.Msaa3D = SubViewport.MsaaMode.Msaa4X, f, "scale 1 0.5 msaa4"));
            T.Check("a sub-pixel size never reaches the engine as zero", Set(() => sv.Size = new Vector2I(0, 0), f, "size 1 1x1"));

            sv.RequestRender();
            T.Check("request_render passes through", f.Count("request 1") == 1);

            root.RemoveChild(sv);
            T.Check("leaving the tree destroys the view", f.Calls.Last() == "destroy 1" && sv.ViewId == 0);
            root.AddChild(sv);
            T.Check("and entering again makes it again (a new id)", f.Count("create Main:Main/Screen") == 2 && sv.ViewId == 2);
            root.QueueFree();
            T.Frame();

            // The engine refuses (the project's budget): a warning, no id, and later setters do not crash or push.
            f.RefuseAfter = 0;
            f.Ids.Clear();
            T.Logs.Clear();
            var refused = new SubViewport { Name = "Refused" };
            SceneTree.Instance.Root.AddChild(refused);
            T.Check("a refused view has no id and says why", refused.ViewId == 0 && T.Logs.Any(l => l.Message.Contains("refused")));
            int before = f.Calls.Count;
            refused.Size = new Vector2I(100, 100);
            refused.RenderTargetUpdateMode = SubViewport.UpdateMode.Always;
            T.Check("and setting its properties pushes nothing", f.Calls.Count == before);

            // Without a key the tree path names the view.
            f.RefuseAfter = 99;
            var anon = new SubViewport { Name = "Anon" };
            var holder = new Node { Name = "Holder" };
            holder.AddChild(anon);
            SceneTree.Instance.Root.AddChild(holder);
            T.Check("a view made by script is named by its tree path", f.Calls.Any(c => c.StartsWith("create /root/Holder/Anon ", StringComparison.Ordinal)), string.Join("; ", f.Calls.Where(c => c.StartsWith("create"))));
        }
        finally { Done(); }
    }

    private static bool Set(Action act, Fake f, string expected)
    {
        int n = f.Calls.Count;
        act();
        return f.Calls.Count > n && f.Calls[^1] == expected;
    }

    private static void Properties()
    {
        Fake f = Install();
        try
        {
            // The level's Props arrive under Godot's names; the converter's private ones under theirs.
            var sv = new SubViewport();
            sv.Set("_viewport_key", "Level:Level/TV/View");
            sv.Set("_material_consumers", 2);
            sv.Set("size", new Vector2I(256, 128));
            sv.Set("render_target_update_mode", 4);
            sv.Set("render_target_clear_mode", 2);
            sv.Set("transparent_bg", true);
            sv.Set("scaling_3d_scale", 0.75f);
            T.Check("Props set the key, the consumer count and the Godot properties by name",
                sv.ViewportKey == "Level:Level/TV/View" && sv.MaterialConsumers == 2 && sv.Size == new Vector2I(256, 128) &&
                sv.RenderTargetUpdateMode == SubViewport.UpdateMode.Always && sv.RenderTargetClearMode == SubViewport.ClearMode.Once && sv.TransparentBg && sv.Scaling3DScale == 0.75f);
            SceneTree.Instance.Root.AddChild(sv);
            T.Check("two material consumers are registered with the view", f.MaterialUsers.GetValueOrDefault(1) == 2);
            sv.MaterialConsumers = 1;
            T.Check("lowering the count removes one", f.MaterialUsers.GetValueOrDefault(1) == 1);
            sv.MaterialConsumers = 4;
            T.Check("raising it adds", f.MaterialUsers.GetValueOrDefault(1) == 4);
            sv.ClearColor = new Color(0.1f, 0.2f, 0.3f, 1f);
            T.Check("the clear colour is pushed", f.Calls.Last() == "clearcolor 1 0.1 0.2 0.3 1");

            sv.Disable3D = true;
            T.Check("disable_3d takes the camera away (a view with no camera is not drawn)", f.Calls.Last() == "camera 1 0");
        }
        finally { Done(); }
    }

    private static void Cameras()
    {
        Fake f = Install();
        try
        {
            var sv = new SubViewport { ViewportKey = "L:V" };
            var camA = new Camera3D { Name = "A" };
            var pivot = new Node3D { Name = "Pivot" };
            var camB = new Camera3D { Name = "B" };
            sv.AddChild(camA);
            sv.AddChild(pivot);
            pivot.AddChild(camB);
            SceneTree.Instance.Root.AddChild(sv);
            T.Check("with none made current the first Camera3D below (tree order) draws the view", sv.ActiveCamera == camA && f.Calls.Last() == "camera 1 100", string.Join("; ", f.Calls));
            T.Check("a camera inside the view never takes the window's view", Camera3D.CurrentCamera != camA && Camera3D.CurrentCamera != camB && SceneTree.Instance.Root.GetCamera3D() == null);

            camB.MakeCurrent();
            T.Check("make_current on another camera under it takes the view", sv.ActiveCamera == camB && camB.IsCurrent() && !camA.IsCurrent() && f.Calls.Last() == "camera 1 101");
            T.Check("and still not the window's", Camera3D.CurrentCamera == null && sv.GetCamera3D() == camB);

            camB.ClearCurrent();
            T.Check("clearing it hands the view to the first camera again", sv.ActiveCamera == camA && f.Calls.Last() == "camera 1 100");

            sv.RemoveChild(camA);
            T.Check("a camera that leaves the view is no longer its camera (the next one is)", sv.ActiveCamera == camB);
            sv.RemoveChild(pivot);
            T.Check("none left, none given", sv.ActiveCamera == null && f.Calls.Last() == "camera 1 0");

            // The converter's private form of 'current': applied before the node is in the tree, honoured when it is ready.
            var sv2 = new SubViewport { ViewportKey = "L:V2" };
            var first = new Camera3D { Name = "First" };
            var chosen = new Camera3D { Name = "Chosen" };
            chosen.Set("_viewport_current", true);
            sv2.AddChild(first);
            sv2.AddChild(chosen);
            SceneTree.Instance.Root.AddChild(sv2);
            T.Check("a camera the scene marked current wins over the first one", sv2.ActiveCamera == chosen && Camera3D.CurrentCamera == null);

            // A camera outside any SubViewport is untouched: it is the window's.
            var main = new Camera3D { Name = "Main" };
            SceneTree.Instance.Root.AddChild(main);
            main.MakeCurrent();
            T.Check("a camera outside any SubViewport still takes the window's view", Camera3D.CurrentCamera == main && main.OwningSubViewport() == null);
        }
        finally { Done(); }
    }

    private static void Texture()
    {
        Fake f = Install();
        try
        {
            var sv = new SubViewport { ViewportKey = "Main:Main/Screen", Size = new Vector2I(256, 128) };
            ViewportTexture? t = sv.GetTexture();
            T.Check("GetTexture gives a ViewportTexture, the same one every time", t != null && ReferenceEquals(t, sv.GetTexture()) && ReferenceEquals(t.GetViewport(), sv));
            T.Check("it is a Texture2D with a size (the requested size before the view exists)", t is Texture2D && t.GetWidth() == 256 && t.GetHeight() == 128);
            T.Check("no engine texture before the view is drawn", t!.NativeHandle == 0 && t.Generation == 0);
            T.Check("the material reference is the runtime texture the converter bakes", t.MaterialReference == "@rt:viewport.Main:Main/Screen");

            SceneTree.Instance.Root.AddChild(sv);
            f.Textures[sv.ViewId] = 0xABCD;
            f.Infos[sv.ViewId] = new SubViewInfo { Width = 128, Height = 64, Generation = 3 };
            T.Check("once the engine draws it, the handle, the drawn size and the generation come from the engine",
                t.NativeHandle == 0xABCD && t.GetSize() == new Vector2(128, 64) && t.Generation == 3);

            // ViewportPath is what a scene stores; Object(...) text builds one.
            Variant v = VariantText.Parse("Object(ViewportTexture, \"viewport_path\": NodePath(\"SubViewport\"))");
            var parsed = (ViewportTexture)v.AsGodotObject()!;
            T.Check("a ViewportTexture written as Object(...) keeps its viewport_path", parsed.ViewportPath.ToString() == "SubViewport" && parsed.GetViewport() == null && parsed.NativeHandle == 0);
        }
        finally { Done(); }
    }

    private static void Container()
    {
        Fake f = Install();
        try
        {
            var outer = new Control { Name = "Outer", Position = new Vector2(100, 50), Size = new Vector2(1000, 800) };
            var box = new SubViewportContainer { Name = "Box", Position = new Vector2(10, 20), Size = new Vector2(640, 360), Stretch = true };
            var sv = new SubViewport { ViewportKey = "L:Box/View", Size = new Vector2I(512, 512) };
            box.AddChild(sv);
            outer.AddChild(box);
            SceneTree.Instance.Root.AddChild(outer);

            T.Check("the container's global rectangle adds its Control parents' positions", box.GetGlobalRect().Position == new Vector2(110, 70) && box.GetGlobalRect().Size == new Vector2(640, 360));
            T.Check("a stretched container resizes its view to its own size", sv.Size == new Vector2I(640, 360) && f.Calls.Contains("size 1 640x360"));
            T.Check("and shows it over the window at its rectangle", f.Screens.TryGetValue(1, out var s) && s.On && s.X == 110 && s.Y == 70 && s.W == 640 && s.H == 360);
            T.Check("the view counts one UI consumer, so a when-visible view is drawn", f.UiUsers.GetValueOrDefault(1) == 1);

            box.Size = new Vector2(320, 180);
            T.Check("a new size follows: the view and the rectangle", sv.Size == new Vector2I(320, 180) && f.Screens[1].W == 320 && f.Screens[1].H == 180);
            box.StretchShrink = 2;
            T.Check("stretch_shrink draws the view at a fraction of the container (the rectangle keeps filling it)", sv.Size == new Vector2I(160, 90) && f.Screens[1].W == 320);

            box.Visible = false;
            T.Check("hiding the container hides the view and drops the UI consumer", !f.Screens[1].On && f.UiUsers.GetValueOrDefault(1) == 0);
            box.Visible = true;
            T.Check("showing it again restores both", f.Screens[1].On && f.UiUsers.GetValueOrDefault(1) == 1);

            outer.Visible = false;
            box.Refresh();
            T.Check("a hidden parent hides it too", !f.Screens[1].On);
            outer.Visible = true;
            box.Refresh();

            box.Stretch = false;
            T.Check("without stretch the view keeps its own size and the rectangle follows it", f.Screens[1].W == sv.Size.X && f.Screens[1].H == sv.Size.Y);

            outer.RemoveChild(box);
            T.Check("leaving the tree destroys the view with the container (children leave first)", f.Calls.Contains("destroy 1") && sv.ViewId == 0);
            outer.AddChild(box);
            T.Check("entering again makes a new view and counts the container as its UI consumer again", sv.ViewId == 2 && f.UiUsers.GetValueOrDefault(2) == 1 &&
                f.Screens.TryGetValue(2, out var again) && again.On && again.W == sv.Size.X && again.H == sv.Size.Y,
                $"id={sv.ViewId} ui2={f.UiUsers.GetValueOrDefault(2)} screen2={(f.Screens.TryGetValue(2, out var s2) ? $"{s2.X},{s2.Y},{s2.W},{s2.H},{s2.On}" : "none")} size={sv.Size} inTree={box.IsInsideTree()} vis={box.IsVisibleInTree()} calls={string.Join("; ", f.Calls.TakeLast(6))}");
            f.Screens.Remove(2);
        }
        finally { Done(); }
    }

    private static void ContainerAnchors()
    {
        Fake f = Install();
        try
        {
            // A scene stores anchors and offsets, not a size: the rectangle is worked out against the window (1280 by 720 here).
            var full = new SubViewportContainer { Name = "Full", AnchorRight = 1f, AnchorBottom = 1f, OffsetLeft = 20f, OffsetTop = 30f, OffsetRight = -20f, OffsetBottom = -30f, Stretch = true };
            var sv = new SubViewport { ViewportKey = "L:Full/V" };
            full.AddChild(sv);
            SceneTree.Instance.Root.AddChild(full);
            Rect2 r = full.GetGlobalRect();
            T.Check("anchors 0..1 with offsets fill the window minus the offsets", r.Position == new Vector2(20, 30) && r.Size == new Vector2(1240, 660), $"{r.Position} {r.Size}");
            T.Check("and a stretched view takes that size and is shown there", sv.Size == new Vector2I(1240, 660) && f.Screens[1].On && f.Screens[1].X == 20 && f.Screens[1].W == 1240);

            // A container inside a Control is laid out against it.
            var panel = new Control { Name = "Panel", Position = new Vector2(100, 100), Size = new Vector2(400, 200) };
            var half = new SubViewportContainer { Name = "Half", AnchorLeft = 0.5f, AnchorRight = 1f, AnchorBottom = 1f };
            panel.AddChild(half);
            SceneTree.Instance.Root.AddChild(panel);
            Rect2 h = half.GetGlobalRect();
            T.Check("against a parent Control, anchors are fractions of its size (and its position is added)", h.Position == new Vector2(300, 100) && h.Size == new Vector2(200, 200), $"{h.Position} {h.Size}");
        }
        finally { Done(); }
    }

    private static void NoEngine()
    {
        // The managed host and no override: a headless tree makes no views and nothing throws.
        SubViewBackend.Override = null;
        var sv = new SubViewport { ViewportKey = "X" };
        var cam = new Camera3D();
        sv.AddChild(cam);
        SceneTree.Instance.Root.AddChild(sv);
        sv.Size = new Vector2I(100, 100);
        sv.RequestRender();
        T.Check("without a host no view is made and the setters are harmless", sv.ViewId == 0 && sv.GetTexture()!.NativeHandle == 0 && sv.ActiveCamera == cam);
        SceneTree.Instance.Root.RemoveChild(sv);
        T.Check("leaving the tree is harmless too", sv.ViewId == 0);
    }
}
