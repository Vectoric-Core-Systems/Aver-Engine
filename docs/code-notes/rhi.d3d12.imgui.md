# Code notes: rhi.d3d12.imgui

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp

- `UiSrvPool::kCount = 512`: Originally 16 slots, found insufficient through measurement. Permanent residents (icon sheets, splash logo, compile-status icon, viewport, asset preview, thumbnail cache) account for ~8 before ImGui atlases; remaining pool exhausted on Content Browser thumbnail generation (ThumbnailCache advertises cap of 64). Changed to 512 to shift constraint back to ThumbnailCache's VRAM budget rather than descriptor heap.

- Layout persistence (`editor-layout.ini`): Changed from always rebuilding default layout on startup. Previously, io.IniFilename = nullptr meant every window size, dock arrangement, table column width, and collapsing-header state was thrown away on exit. Resizing the Outliner and restarting put it straight back — intentional per old docs/EDITOR.md, but wrong: startup cost paid by everyone forever. Now persists per-user per-machine in user data dir (not next to executable, which fails on Program Files). View > Reset Layout clears dockBuilt_ and rebuilds default as safety valve.
