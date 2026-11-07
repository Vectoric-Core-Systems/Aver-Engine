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
  - `C2DCamera` comes later.
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
  - AnimatedSprite2D, Camera2D and Parallax2D come later.

## Steps

1. Components and the 2D transform pass. Headless tests: parent chain, rotation, scale, revisions.
2. Ordered submit to the draw list: sort by key, emit quads (flip, region UVs, modulate, clips). Headless, against the draw list.
3. Canvas transform and stretch ABI, with a parity test against `uiComputeStretch` and Fabric's Control stretch.
4. Shared-key submit for external items (Fabric's Controls). Test interleaving: a sprite under, between and over Controls by layer and z.
5. Scripting ABI and C# wrappers, with ABI-version and layout tests.
6. `.ocworld` round trip of the 2D components (golden file).
7. Fabric converter and runtime mapping. Convert a neutral fixture and assert the records and the runtime state, then convert the real menu scene.
8. Headed check: a Node2D menu's background behind its buttons in editor Play and in the packaged runtime, at two aspect ratios.
