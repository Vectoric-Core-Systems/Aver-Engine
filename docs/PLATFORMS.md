# Linux and macOS

Aver builds and runs on three platforms. Windows is unchanged by the port: the same Win32 code, D3D12
as the default backend, and the same presets and CI job. Linux and macOS run on the Vulkan backend,
with SDL 3 for the window, input, gamepads, audio and file dialogs.

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | MSVC | GCC 13+, Clang 17+ | AppleClang (Xcode 16+), macOS 14+ |
| GPU backends | D3D12 (default), D3D11, Vulkan | Vulkan | Vulkan over MoltenVK |
| Window and input | Win32 | SDL 3 (X11 and Wayland) | SDL 3 (Cocoa) |
| Audio device | WASAPI (`Aver.Audio.Wasapi`) | SDL 3 (`Aver.Audio.Sdl`) | SDL 3 (`Aver.Audio.Sdl`) |
| Shader compiler | `third_party/dxc-spirv/dxcompiler.dll` | `libdxcompiler.so` | `libdxcompiler.dylib` (Vulkan SDK) |
| C# scripting | .NET 10 | .NET 10 | .NET 10 |

## Status

Checked on 2026-10-12 against `0.7-main-nrdneuraa`.

- **Linux, verified.** Ubuntu 24.04 (Pop!_OS on COSMIC), AMD Radeon RX 9070 XT on RADV, GCC 13. The
  whole CTest run passes: 300 tests, the `.vulkan` variants and the managed suites included, 4 of them
  skipped because they test Windows-only pieces (Media Foundation video encoding, two D3D12 GPU tests,
  the D3D12 run of `LineMeshTest`). The tree also builds with Clang 18 and libc++ 18, the nearest thing
  to AppleClang on Linux; that suite passed too, apart from `TemplateScaffoldTest`, which finds
  `templates/` by walking up from `bin/` and so needs the build directory inside the checkout (that
  one was not). The runtime plays a project; the editor opens one, draws
  the viewport, and runs the C# host in-process, on X11 (through XWayland) and on native Wayland.
- **macOS, built in CI only so far.** The `macos-15` job in `.github/workflows/ci.yml` builds and runs
  the suites. Nobody has run the editor on a Mac yet; the last section of this page is the list for
  doing that.
- **Windows, unchanged.** The existing `windows-2022` job is the check that nothing shared broke.

## Building on Linux

Ubuntu 24.04 or anything as recent. SDL's build dependencies, Ninja, and a Vulkan driver:

```sh
sudo apt install build-essential cmake ninja-build \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdecor-0-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev libvulkan1 mesa-vulkan-drivers
```

Missing X11 extension headers are not fatal: `cmake/AvSdl.cmake` builds SDL without the ones it can't
find and says which. C# scripting needs the .NET 10 SDK on `PATH`; without it everything else still
builds and the scripting host declines at run time.

```sh
cmake --preset linux-ninja            # or linux-ninja-release
cmake --build build
ctest --test-dir build -j 8           # or ./scripts/test.ps1 under pwsh
./build/bin/Sandbox                   # the editor
./build/bin/AverEngineRuntime --project path/to/Game.ocproject
```

SDL picks X11 or Wayland itself, and on some compositors it prefers XWayland. `SDL_VIDEO_DRIVER=wayland`
selects native Wayland; on COSMIC that was noticeably faster.

## Building on macOS

Xcode 16 or its command-line tools, Homebrew, and the LunarG Vulkan SDK. The SDK is where MoltenVK, the
Vulkan loader and a SPIR-V-capable DXC come from: Microsoft publish no macOS DXC and Homebrew does not
package one. Install the SDK with its "System Global Installation" component (that puts the loader
where the engine looks for it) and point `VULKAN_SDK` at its `macOS` folder.

```sh
brew install cmake ninja sdl3
export VULKAN_SDK=$HOME/VulkanSDK/<version>/macOS
cmake --preset macos-ninja
cmake --build build
ctest --test-dir build -j 8
./build/bin/Sandbox
```

MoltenVK has no ray tracing and no mesh shaders, so the renderer runs its non-RT paths there.

## Where the dependencies come from

Neither new dependency is vendored. Each is an installed package if there is one, and otherwise a
pinned release that CMake downloads once and checks against its SHA-256.

| Dependency | Licence | Package looked for | Otherwise |
|---|---|---|---|
| SDL 3.4.18 | zlib | `find_package(SDL3)`, e.g. Homebrew's `sdl3` | `FetchContent`, built as a shared library |
| DXC (`libdxcompiler`) | MIT + LLVM | `AVER_DXC_LIBRARY`, then `$VULKAN_SDK/lib`, then the system | Linux x86_64: Microsoft's v1.9.2609 release. macOS: none |

An offline machine can configure with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` and the sources supplied
through `FETCHCONTENT_SOURCE_DIR_SDL3` and `FETCHCONTENT_SOURCE_DIR_AVER_DXC`. Only `libdxcompiler` is
copied into `bin/`; the release's `libdxil` is not used. DXC's headers (`dxcapi.h` and the
`WinAdapter.h` COM shim) are vendored in `third_party/dxc-spirv/include/dxc`, so the Vulkan backend
builds without the library and reports "no shader compiler" at run time if it's missing, as it does on
a Windows machine without `dxcompiler.dll`.

SDL is shared on purpose: a static copy linked into the executable and into the shared engine modules
would be two SDLs in one process.

## What is different from Windows

- **The window is SDL's** (`modules/platform/src/sdl`). SDL scancodes are translated to Win32
  virtual-key codes in one table (`SdlKeys.cpp`, tested by `SdlKeyMapTest`), so every key binding, the
  framework's input ABI and the editor's shortcuts read the same numbers everywhere. SDL has one event
  queue for every window, so `Window::pumpEvents()` routes each event to the window it belongs to and
  offers all of them to the UI first, Dear ImGui's secondary viewports included.
- **Multi-viewport** (panels dragged out of the main window) works on X11 and macOS. Wayland doesn't
  let a client place its own windows, and ImGui's SDL backend turns the feature off there.
- **Mouse capture** for a game is SDL's relative mouse mode, the only kind Wayland allows.
- **The file watcher** is inotify on Linux and FSEvents on macOS, behind the same `IWatchBackend` the
  Win32 one implements; `WatcherTest` runs on all three.
- **The file dialog** is the desktop's own, through SDL: the XDG portal on Linux, `NSOpenPanel` on macOS.
- **Move to trash** follows the freedesktop.org Trash specification on Linux (`ShellTrashTest`) and
  uses the system Trash on macOS. Reveal-in-file-manager uses `org.freedesktop.FileManager1` or
  `xdg-open`, and `open -R` on macOS.
- **External tools** (git, `dotnet build`, VS Code, Rider, Play in Standalone) are started through
  `/bin/sh`; see `sandbox/src/ProcessRun.hpp`.
- **The user data directory** is `$XDG_DATA_HOME/AverEngine` (default `~/.local/share/AverEngine`) on
  Linux and `~/Library/Application Support/AverEngine` on macOS.

### Not available off Windows yet

- The D3D12-only render features, which is every one `docs/VULKAN.md` lists as D3D12-only.
- Compressed audio import and video encoding, which use Media Foundation. WAV imports everywhere.
- The crash reporter: a crash there is the system's own, a core dump or the macOS crash report.
- Single-instance forwarding: every launch opens its own editor.
- The MCP control channel (`modules/mcp`, `"platforms": ["windows"]`). Its sockets port easily, but the
  editor side injects input as Win32 messages and needs a platform hook first.
- Packaging a game (`scripts/stage-game.ps1` and the launcher).
- `GraphManagedTests` is registered on Windows only; see `tests/managed/CMakeLists.txt` for why.

## Bugs the port found that are not about Linux

These are shared code, so the fixes apply on Windows too.

- **The Vulkan swapchain lost its present semaphores on every resize.** `VulkanDevice::resize`
  destroyed them and never created new ones, so the next submit signalled a destroyed handle. RADV
  rejects that and the device is lost on the first resize; a driver that tolerates a dead semaphore
  only hid it.
- **The Vulkan editor UI received no input on Windows.** `VulkanDevice::uiInit` never registered the UI
  backend's window-message handler, which D3D12's does.
- **Inserting a Sequencer key returned a garbage index.** `keys.insert(...) - keys.begin()` has
  unsequenced operands, and a compiler that evaluates `begin()` first subtracts a stale iterator
  (`SequenceCurveOps.cpp`, `SequenceTrackOps.cpp`, `SequenceEditor.cpp`).

## Writing code that stays portable

- **Paths:** build them with `AVER_PATH_SEP` (`aver/core/Types.hpp`), which joins into the literal next
  to it at compile time: `dir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "Crashes"`. On Windows the result is
  exactly the backslash path the tree always built, so prefix checks against paths the Windows shell
  returns keep matching. `toNativeSeparators()` (`aver/platform/FileSystem.hpp`) is for a path that is
  canonicalised before it is used as a key. `kExecutableSuffix` names a sibling program.
  `PathSeparatorTest` fails on a hand-written backslash joined into a path in any code that builds
  off Windows. It runs on Windows too, so the author of the slip sees it first.
- **Floating-point text:** use `aver::fromChars` (`aver/core/CharConv.hpp`) for `float` and `double`.
  libc++ before LLVM 20, which is every current Xcode's, has no floating `std::from_chars`.
  `CharConvTest` compares the fallback with the real thing wherever both exist.
- **Shared libraries:** load them through `aver/core/DynamicLibrary.hpp`.
- **A module that exists on some platforms only** says so in its `module.json` with
  `"platforms": ["windows"]` (or `"linux"`, `"macos"`). It is then treated as absent elsewhere by
  configure and by `scripts/module-catalog.py`, and nothing may depend on it there. The D3D12 and
  D3D11 backends are marked this way, so a plain `cmake -S . -B build` works on every platform.
- **Headers:** include what you use. MSVC's standard headers include each other far more than
  libstdc++'s and libc++'s do, and most of the first Linux build's errors were exactly that.

## The first session on a Mac

On an Apple Silicon Mac with the tools from "Building on macOS" installed:

```sh
git clone https://github.com/Vectoric-Core-Systems/Aver-Engine && cd Aver-Engine
git checkout <this branch>
export VULKAN_SDK=$HOME/VulkanSDK/<version>/macOS
cmake --preset macos-ninja-release && cmake --build build-release
ctest --test-dir build-release -j 8
./build-release/bin/AverEngineRuntime --project test-content/MaterialGraph/Game.ocproject --frames 120 --screenshot /tmp/runtime.png
./build-release/bin/Sandbox test-content/MaterialGraph/Game.ocproject --open-legacy
```

What to look at: the screenshot shows a blue sky, a grey floor and two cubes; the editor opens with its
panels, the viewport draws the same scene, the mouse and keyboard work in both the panels and the
viewport, a panel can be dragged out into its own window, and the window title reads
"Aver Engine — Editor".
