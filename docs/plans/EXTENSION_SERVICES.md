# Engine seams as extension services

**Why.** The extension contract (`docs/EXTENSIONS.md`) offers an extension versioned services through `host->getService(name, version)`,
but only one exists (`aver.asset.import-model`). A managed extension that needs the engine's navigation, animation, audio, network
or UI seams P/Invokes their DLLs by name. Those calls are outside the contract, and most seams carry no version. When the engine
changed `aver_nav_region_set_mesh` (a9778ba0, 2026-11-14), the Fabric Connector built against the old signature crashed (0xC0000005)
instead of being refused.

**What changes.** Every seam (`docs/abi/index.md`) is offered as a service, `aver.<seam>` at a version, and an extension reaches
the engine only through what it is handed.

## Engine

1. **`scripts/abi-services.lock`**: for each service, its name, version, DLL and its functions' C signatures in table order.
   `scripts/abi-services.py --check` (ctest `AbiServicesCheck`) compares the headers with the lock:
   - a new function in a header must be appended (`--update`). The table grows at the end and the version stays.
   - a changed or removed signature, or a new order, needs a new version (`--update --bump aver.<seam>`). Code built
     against the old version is then refused, never handed a table whose slots mean something else.
2. **The table**: `{ uint32_t size; uint32_t count; const void* fn[count]; }`, the functions in lock order. `size` is in bytes,
   so a reader built against fewer slots reads only those.
3. **Registration** (`ext::registerSeamServices`, called by `ScriptHost` and AverAssetC before extensions load): for each service in
   the lock (compiled in as `SeamServices.gen.cpp`), find its already-loaded DLL and resolve every name. A DLL that is absent
   (the module is off in this edition) or missing a name means the service is not offered, which is logged once.
4. **Managed reach**: `ExtensionContext.GetService(name, version)` returns the table (`IntPtr.Zero` when not offered). The bridge
   receives the host's lookup the way it receives the extension list.
5. **Manifest**: `"managed": { "services": { "aver.nav": 1, ... } }`. `ScriptHost` refuses to load the managed half when the
   engine does not offer a listed service at that version, with one log line saying which one and what is offered.

## Fabric Connector

6. Its `*Native.cs` declarations become `static partial` methods whose bodies a generator in that repository (seam-bindings.py) writes. The body calls
   through the table, marshalling arrays, strings and `out`/`ref` arguments. The generator matches every C# parameter against the
   lock's C type and fails on a mismatch, so ABI drift stops the Fabric build instead of crashing a game.
7. No `DllImport` of an engine library remains, only `Aver.Fabric.Native` (its own). The manifest lists the services it needs.
8. Tests that run without the editor's host resolve the same tables directly from the DLLs, by the lock's names.

## Not changing

Fabric's converter links the engine's format writers (`Aver.Formats`) at build time. Its interface with the engine at run time is the
files it writes, which are versioned (`docs/formats/FORMAT_SPECS.md`).
