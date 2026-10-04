# DirectX Shader Compiler — the SPIR-V-capable build

`dxcompiler.dll`, from the official Microsoft release
[`v1.9.2607`](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607)
("DX Compiler Release for July 2026", asset `dxc_2026_07_29.zip`, 41,625,275 bytes).
Only the x64 compiler DLL is kept; see *What is deliberately NOT here* below.

## Why this exists at all

`modules/rhi.vulkan` compiles the engine's HLSL to SPIR-V by handing `-spirv` to DXC. The
engine already had a DXC — `modules/rhi.d3d12/CMakeLists.txt` finds `dxcompiler.dll` in the
**Windows SDK** and copies it beside the executable — and the Vulkan backend was designed on the
assumption that this was the same compiler:

> the engine already has DXC, which emits SPIR-V with `-spirv`, so no glslang/shaderc either

**That assumption is false**, and it cost the backend its entire life: Microsoft build the Windows
SDK redistributable **without** the SPIR-V backend compiled in. `-spirv` is accepted as an argument
and then refused at code generation:

```
VSMain (vs_6_0): SPIR-V CodeGen not available. Please recompile with -DENABLE_SPIRV_CODEGEN=ON.
```

Only the releases from the DirectXShaderCompiler GitHub project (and the LunarG Vulkan SDK) ship
one built with `ENABLE_SPIRV_CODEGEN=ON`. This is that DLL.

The alternatives were both already ruled out on purpose and stay ruled out:
vendoring **glslang/shaderc** would be a second source of truth for the same HLSL, and taking the
**LunarG SDK** as a build dependency is what `third_party/vulkan-headers/README.md` exists to
avoid. Vendoring the compiler keeps both decisions intact.

## Licence

The DXC release archive carries **three** licence files, and the split matters:

| File | Covers | Kept here |
|---|---|---|
| `LICENCE-MIT.txt` | Microsoft's own DXC source | ✅ |
| `LICENSE-LLVM.txt` | the LLVM base (University of Illinois / NCSA) | ✅ |
| `LICENSE-MS.txt` | **`dxil.dll`** — a proprietary Microsoft EULA | ❌ not applicable, see below |

`dxcompiler.dll` is built from the open-source DirectXShaderCompiler tree, which is MIT (Microsoft's
contributions) over NCSA (the LLVM base). Both are permissive and both are on this repository's
accepted list in `docs/ASSET_IMPORT.md` — MIT explicitly, and NCSA as a BSD-style licence whose
terms are the attribution-and-disclaimer set that list is drawn around.

### What is deliberately NOT here

**`dxil.dll` is not vendored, and must not be.** It is Microsoft's DXIL *signing* library, shipped
as a binary under `LICENSE-MS.txt` — a proprietary EULA with data-collection terms and distribution
restrictions, i.e. exactly the kind of licence this repository is permissive-only in order to avoid.

Nothing here needs it. DXIL signing is a **D3D12** concern (drivers reject unsigned DXIL), and the
D3D12 backend keeps getting `dxil.dll` from the Windows SDK exactly as before. SPIR-V is not signed,
so the Vulkan path never calls into it.

Also dropped from the archive: the arm64 and x86 builds, the PDBs, and the HLSL header set — none
of which any part of this engine loads.

## How it is used

`modules/rhi.d3d12/CMakeLists.txt` owns the copy-to-`bin/` step for both backends (the Vulkan module
adds no copy of its own). It now prefers **this** DLL over the SDK's when it is present, so a single
`dxcompiler.dll` beside the executable serves both: DXIL for D3D12, SPIR-V for Vulkan. That is the
whole point of there being one — two DXCs in one directory is a coin toss over which one loads.

## Updating

Download a newer Windows release asset, keep `bin/x64/dxcompiler.dll` (renamed to `dxcompiler.dll`
here -- NOT in a `bin/` subdirectory, because the repository's `.gitignore` excludes `[Bb]in/` and
would silently drop it, which it did once already) and the two open-source licence files, and re-verify the thing that actually matters:

```
Sandbox.exe --backend vulkan --frames 20 --headless
```

A working compiler gets past shader compilation; a Windows SDK one fails on `SPIR-V CodeGen not
available` at the very first shader. That single line is the whole acceptance test for this
directory.
