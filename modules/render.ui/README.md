# Aver.Render.UI

The GPU half of the retained game UI. `Aver.UI` produces a draw list; this turns one into draw
calls. That is the whole of its responsibility.

## Why it is a separate module

`Aver.UI` depends on `Aver.Core` and nothing else, which is what makes a widget tree testable with
no GPU and no device. Giving it the RHI would end that property permanently. So the split runs where
`Aver.Render.Voxi` puts its own: settings and logic on one side, the device on the other.

It buys three things:

- the widget tree is testable headless (`tests/ui`, 26 assertions, no device);
- a second backend is a second file **here** and no change **there**;
- there is exactly one place in the engine that knows how a UI vertex reaches a rasteriser.

The link line enforces it: this module names `Aver.RHI`, the generic interface, and never
`Aver.RHI.D3D12`.

## Where it runs in the frame

`UiRenderer` is an `rhi::IRenderFeature` and implements exactly one hook — `overlayPass`, which the
backend calls after the camera post chain with the backbuffer bound.

That is not an implementation detail. A HUD is authored in display colours and must not be
tonemapped, exposed or bloomed along with the world behind it. Drawn into the scene target, a white
panel would be the brightest thing in the frame and would stop the eye adaptation down over the
entire image — the UI would decide the exposure of the game.

Consequences of drawing after the tonemap:

- the backbuffer is `RGBA8_UNORM` and already gamma-encoded, so a colour authored as an sRGB byte
  arrives on screen as that byte;
- the blend is therefore in display space. That is the wrong space for compositing light and exactly
  the space a designer picking 50% alpha expects;
- there is no depth buffer and no depth test. Compositing is submission order, which is what the
  layers in `UiDrawList` define.

## What it needed from the RHI

The RHI could not express caller-owned geometry at all: every draw was addressed by `MeshHandle`,
which names an immutable upload of the engine's 32-byte `MeshVertex`. A UI vertex is 20 bytes with
no normal and no third coordinate, and it is rebuilt every frame.

Four additions, each the minimum:

| Addition | Why |
|---|---|
| `GraphicsPipelineDesc::vertexLayout` | A feature that owns its geometry owns its vertex format. Left empty, the backend still uses `MeshVertex`, so no existing pipeline changed. |
| `IResourceFactory::writeBuffer` | Upload buffers were already mapped for life; nothing exposed the write. Rejected on any other kind, so a hidden staging copy cannot masquerade as a memcpy. |
| `setVertexBuffer` / `setIndexBuffer` / `drawIndexed` | Draw a range of caller-owned geometry. |
| `IRenderFeature::overlayPass` | The only hook downstream of the tonemap. |

## Buffering

Vertex and index buffers are rotated three deep against a backend that keeps two frames in flight.
The rotation happens in `overlayPass`, not `submit`, because `overlayPass` is what the backend calls
once per frame — rotating on submit would let an app that submits twice (a HUD and a debug overlay
built separately) advance the ring twice and overwrite a buffer the GPU is still reading.

## Textures

`UiDrawCmd::texture` is a `u64` in `Aver.UI` precisely so that module never learns the RHI's
vocabulary. The meaning of the number is therefore **this** module's: it is an `rhi::TextureHandle`,
widened, and `0` means the built-in 1×1 white texel. An untextured rect is a textured rect sampling
that, which keeps one pipeline and one code path for a solid quad and a glyph alike.

Both the vertex colour and any bound texture are required to be **premultiplied**. That is stated
rather than detected: a straight-alpha texture multiplied by a premultiplied colour produces a halo
that looks like a filtering artefact and is not one.

## Status

Not verified on screen at the time of writing. The path compiles, all suites pass, and the sandbox
carries a hand-written demo draw list behind **Window → Game UI Demo** — off by default, because the
gates compare backbuffer pixels and a HUD over the viewport would move every one of them.

There is no text and no widget tree yet. Text is blocked on a font decision (vendor
`stb_truetype.h`, or bitmap fonts); the widget tree is the next thing above this.
