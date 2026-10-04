# Play in New Window

Play options > **New Window** runs the normal Play-in-Editor session (same world, GameMode, pawn and
tick as Selected Viewport) but shows the game in a separate OS window with no editor UI. Esc in that
window, closing it, or the toolbar's Stop ends play and returns to the editor. It is the same
process, not the Standalone runtime.

## How it works

- **One device, one render.** Nothing renders twice. The scene renders as it always does, in the
  scene rect, which `updatePlayWindow` sets each frame to the top-left of the present image at the
  play window's client size (clamped to the editor window, which the present images are sized from).
- **Mirror output (D3D12).** `IDevice::setMirrorWindow(hwnd, w, h)` creates a second flip-model
  swapchain on the same present queue. After the post chain and overlays (HUD) and before the editor
  UI, `presentPass` copies that rect into `mirrorImages_[image]`. The present thread, which already
  presents every image (real and NeuraFI-generated) at its moment, copies and presents the mirror
  image right after the main one, under the same fence. The mirror presents with sync 0 so it never
  waits on vblank, and a mirror failure never fails the main present.
- **Input.** The play window's events feed the same `InputState` as the editor window. While it is
  the foreground window, input ownership ignores ImGui (which only sees the editor window), so the
  keyboard and mouse go to the game. Mouse capture anchors to the play window. Esc, Shift+F1
  (release the mouse) and the click that takes it back are read from `InputState`. The spectator
  fly keys accept `InputState` keys too.
- **Window.** A second `Window` with `WindowDesc::quitOnDestroy = false`. Without it, `WM_DESTROY`
  posts `WM_QUIT` and closing the play window would close the editor.

## Limits

- D3D12 only. On Vulkan `setMirrorWindow` returns false and play continues in the viewport.
- The play window can be larger than the editor window, but the game then renders at the editor
  window's size and is stretched.
- F8 (eject) and the other editor chords still work from the editor window, not from the play
  window.
