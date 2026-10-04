# Vulkan-Headers

| | |
|---|---|
| Upstream | https://github.com/KhronosGroup/Vulkan-Headers |
| Version | v1.3.296 (`VK_HEADER_VERSION 296`) |
| Licence | Apache License 2.0 (see `LICENSE.md`) |
| Vendored | 2026-08-09 |

Headers only — `include/vulkan/**` and `include/vk_video/**`. No source, no binaries, no build
system: the upstream CMake, tests and registry XML are deliberately not copied.

## Why the headers and not the LunarG SDK

The full LunarG SDK is an installer that wants ~1 GB, admin rights and a `VULKAN_SDK` environment
variable, and it exists to provide four things: the headers, the validation layers, a shader
compiler, and the loader. This tree needs one of those from a package:

- **Headers** — vendored here. Apache-2.0, which is on this repository's accepted list
  (`docs/ASSET_IMPORT.md`: MIT/BSD/zlib/Apache-2.0/public-domain only).
- **Loader** — already present. `C:\Windows\System32\vulkan-1.dll` ships with the GPU driver, not
  with the SDK, and `vulkaninfo.exe` is on PATH on this machine. A Vulkan application links the
  loader by name and finds the ICD through the registry; nothing from the SDK is involved at run
  time.
- **Shader compiler** — the engine already has DXC, and DXC emits SPIR-V with `-spirv`. Adding
  glslang or shaderc to compile the same HLSL a second way would be a second source of truth for
  what a shader means.
- **Validation layers** — genuinely only in the SDK, and genuinely useful. They are a DEBUGGING
  aid, loaded by name at instance creation and absent otherwise; a backend built without them
  still runs, it just fails less informatively. If validation is wanted later, install the SDK
  then; nothing here has to change, because layers are discovered at run time.

So the SDK is not a build dependency of this backend. Installing it remains the right move for
anyone actually debugging the Vulkan path.

## Updating

Replace `include/` wholesale from a tagged upstream release and update the version line above.
Nothing in this directory is modified from upstream, so a diff against the tag should be empty.
