# 2D scenes

Engine 2D is sprites and 2D transforms as engine components. Native games can use them, and Fabric maps Godot's Node2D, Sprite2D and CanvasLayer onto them. The design aims to be useful, not to clone Godot.

## Decisions (owner, 2026-11-07)

- The engine owns 2D. Fabric does not draw 2D nodes itself.
- Sprites and UI Controls keep Godot's draw order through a **shared sort key**: (canvas layer, z, tree order). The engine sorts its sprites by it, and Fabric tags each Control draw with the same key so the two interleave. An example is a background sprite under its child buttons.

## Shape

- **Components.** These are registered dynamically, as `CControlRig` is, so no built-in id moves. Zero means unset throughout, because the pool zero-fills.
  - `C2DTransform`: position, rotation, scale and z-index (plus z-as-relative). Its derived world affine is recomposed by a 2D pass keyed on revisions, apart from the 3D pass.
  - `CSprite2D`: texture id, region (zero means the whole image), flip bits, modulate, a "not centered" bit, offset, and hframes/vframes/frame.
  - `C2DLayer`: layer index, visibility, follow-viewport, offset/rotation/scale.
  - `CSpriteAnim2D`, `CCamera2D` and `CParallax2D`: the animation, camera and parallax (see the last status section).
  - 2D entities live in the scene World and use `CHierarchy` and `CName`.
- **Drawing.**
  - Through the UI draw list (`modules/ui.abi`), which runs in both the editor's Play and the packaged runtime. It has samplers, blending, clips and transforms, and it draws over the 3D scene, as Godot's canvas does. "Behind 3D" is not a goal.
  - Visible sprites are gathered and sorted by the shared key. Fabric's Control replay hands its items to the same ordered submit, so one sorted list is emitted.
- **Stretch.** The engine owns the canvas transform (viewport stretch and aspect, using `uiComputeStretch`). Fabric's Control path moves onto it, with a parity test that the two agree.
- **Formats.** `COMPONENT C2DTransform` and `COMPONENT CSprite2D` records on a placement with no mesh.
- **ABI.** A versioned `scene2d_abi.h` (create, transform, sprite, layer, bounds), plus thin C# wrappers in Aver.Framework.
- **Fabric.**
  - The converter maps Node2D, Sprite2D and CanvasLayer to the components. Other 2D classes stay unsupported, with their reason.
  - The runtime's Node2D and Sprite2D drive the components, and the Control replay submits with the shared key.
  - AnimatedSprite2D, Camera2D and Parallax2D map onto `CSpriteAnim2D`, `CCamera2D` and `CParallax2D` (last status section).

## Steps

1. Components and the 2D transform pass. Headless tests: parent chain, rotation, scale, revisions.
2. Ordered submit to the draw list: sort by key, emit quads (flip, region UVs, modulate, clips). Headless, against the draw list.
3. Canvas transform and stretch ABI, with a parity test against `uiComputeStretch` and Fabric's Control stretch.
4. Shared-key submit for external items (Fabric's Controls). Test interleaving: a sprite under, between and over Controls by layer and z.
5. Scripting ABI and C# wrappers, with ABI-version and layout tests.
6. `.ocworld` round trip of the 2D components (golden file).
7. Fabric converter and runtime mapping. Convert a neutral fixture and assert the records and the runtime state, then convert the real menu scene.
8. Headed check: a Node2D menu's background behind its buttons in editor Play and in the packaged runtime, at two aspect ratios.

## Status (2026-11-08, steps 1-6)

Done in the engine, headless-tested: the three components and the 2D transform pass (`Scene2D.hpp`), the ordered canvas and its shared-key submit (`Canvas2D.hpp`), the canvas transform against `uiComputeStretch`, `scene2d_abi.h` v1 with the C# wrappers (`Aver.Framework/Scene2D.cs`), and the `COMPONENT C2DTransform` / `CSprite2D` / `C2DLayer` records with a golden file (`tests/scene2d/data/menu2d.ocworld`). All of it lives in `Aver.Scene.dll` (which now links `Aver.UI` privately); the tests are `Scene2d*Test` under `tests/scene2d`.

Decisions taken while building it, for the owner to veto:

- `C2DLayer` also has an `.ocworld` record (the plan named two): a saved level would lose its canvas layers otherwise.
- The tree order of a sprite is derived from the hierarchy unless `C2DTransform::treeOrder` is set. Fabric should set it to its own node-tree index, so its Controls and the sprites share one number space.
- Frames: the region (or the whole image) is divided by `hframes` x `vframes` and `frame` picks a cell, wrapping. Godot ignores the frame when a region is set; here they combine.
- Anything drawn into the UI draw list directly (not submitted) lands before the flushed canvas, so it is under every sprite.
- `Viewport` stretch mode scales like `canvas_items` (the draw list cannot render to a lower-resolution target).

## Status (2026-11-08, host wiring)

Both hosts now flush the canvas. `Runtime/include/aver/game/GameScene2D.hpp` is the shared helper (header-only, like `GameUiImages.hpp`): `scene2dBind` installs the sprite texture resolver (asset id -> content path through the project's content index -> `aver_ui_image_use`), and `scene2dSubmit` sets the canvas view rectangle from `aver_ui_viewport` (`setCanvasViewRect`, ABI v2 `aver_scene2d_set_canvas_view`; the stretch rules a script set stay) and flushes sprites and queued external draws into the UI draw list. The packaged runtime calls it from `GameApp::submitGameUi`. The editor calls it from `SandboxApp::submitGameUi` only while a Play session is active, and drops the queue otherwise, so edit mode draws no sprites, as it draws no game UI. Both run it before the UI renderer takes the list, so anything drawn straight into the list sits under every sprite.

A class placement (Fabric's Node2D, Sprite2D, CanvasLayer) takes its `COMPONENT C2DTransform` / `CSprite2D` / `C2DLayer` records as its actor spawns (`applyPlacedComponents2D`); the level pass skips class placements. Tested by `Scene2dHostTest` and `Scene2dLevelTest`.

## Status (2026-11-09, animation, camera, parallax)

Three more components, all in `Scene2D.hpp`, registered with the others (six now) and in `.ocworld` as `COMPONENT CSpriteAnim2D` / `CCamera2D` / `CParallax2D` (FORMAT_SPECS, golden file `test-content/Scene2D/menu2d.ocworld`). `scene2d_abi.h` is version 3 with C# wrappers in `Scene2D.cs`. Tests: `Scene2dAnimTest`, `Scene2dCameraTest`, plus the ABI, level, host and transform tests extended.

- **Sprite animation (`CSpriteAnim2D`).** Up to 16 named clips and 128 frames in all, in the component itself (fixed storage, so the field table and the record stay plain). A frame is a texture (0 keeps the sprite's own), a region (0 size = the whole image) and a `cell` (the sprite's `frame`); its length is `weight / (fps * speed scale)` seconds, which is how Godot's SpriteFrames frame durations work. Loop, autoplay (once), play / stop / pause / set frame, a negative speed plays backwards. While a clip is current its frames own the sprite's texture, region and frame. Events (frame changed, finished, looped, animation changed) go to a queue that scripts drain (`aver_scene2d_anim_poll`); they are never saved. Stepped by `tick` (below).
- **Camera (`CCamera2D`).** On an entity with a `C2DTransform`: zoom, offset, limits (left / top / right / bottom, each a flag), smoothing (the centre eases by `min(1, speed * dt)` toward its goal), drag margins, a top-left anchor, an optional follow-rotation (the default ignores it). One camera applies: the first enabled one flagged current, else the first enabled one; `makeCameraCurrent` moves the flag, and the new camera's view starts on it with no easing. The camera's view is world -> content units and goes on last, so it composes with the canvas stretch; it applies to the default canvas (sprites under no `C2DLayer`) and to layers with `kLayerFollowViewport`, and not to other layers (UI layers stay fixed). The limits clamp the view so its edge stops at the limit; when the limits are closer together than the view the left / top limit wins, as in Godot.
- **Parallax (`CParallax2D`).** Godot 4.3 Parallax2D semantics for the sprites below the entity (`pin` stores `1 - scroll_scale` so zero is the default): shift `camera * pin + scroll offset + autoscroll * time`, tiling by `repeat size` with `repeat times` copies each side (the shift wraps by whole tiles relative to the camera, so tiles follow the view), limit begin / end clamp the camera position the scroll uses, and `kParallaxNoFollow` leaves the layer in screen space.
- **`tick(world, dt)`** is the one time step: transforms, animations, parallax autoscroll, camera drag and smoothing. The hosts call it from `scene2dFrame` with the wall-clock time since the last frame (at most 0.1 s); headless tests pass their own dt, and `dt = 0` steps no time but still snaps a new camera.

Decisions for the owner to veto:

- Animation storage is fixed (16 clips, 128 frames per entity) rather than a shared resource. A converted game over that is reported by Fabric. A shared `SpriteFrames` asset would fix it but needs a format of its own.
- Time comes from the wall clock inside `scene2dSubmit`, not from the game's delta, so a paused Play still animates (the same as sprites drawing while paused).
- The parallax limits clamp the camera position the scroll uses; Godot's limits clip the scrolling range. They agree for a camera that stays inside the limits.
- Camera limits apply to the unrotated view, also with follow-rotation.
