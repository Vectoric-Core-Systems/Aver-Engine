I have everything needed. Here is the extracted spec.

---

# GPU Vehicle-Body Deformation Compute Pipeline — Port Spec

Faithful extraction from the authoritative sources. Purpose: re-express the per-frame GPU cage-skin/deform as a compute pass on a new RHI (DX12/DX11/Vulkan), independent of Unreal.

## 1. What the pipeline does (one sentence)

One compute thread per body vertex blends the vertex's `K` bound cage-node displacements (pre-normalized inverse-distance weights), maps the blended displacement from cage space to actor space (Y-mirror), applies a per-section gain and a length clamp, adds it to the rest position, and writes the deformed position into a buffer that is simultaneously bound as the mesh's position vertex stream — eliminating the per-frame CPU re-upload of the whole mesh during crash damage (`GpuVehicleCageComponent.h:9-20`).

## 2. Coordinate system, units, layout conventions

| Property | Value | Source |
|---|---|---|
| Coordinate system | UE left-handed, Z-up, cm units (positions are "actor-space") | `VehicleDeform.usf:15`; `VehicleCageComponent.h:450` |
| Displacement space | CAGE space; converted to actor space by flipping Y when `bMirrorY` | `GpuVehicleCageComponent.cpp:566`, `.usf:69`; `VehicleCageComponent.cpp:503-504` |
| Cage→actor mirror | reflection across Y (`disp.y = -disp.y`) | `.usf:69` |
| Numeric type | 32-bit float / 32-bit signed int; host is little-endian Win64 | `VehicleDeformShader.h:21-25` |
| Vertex position packing | flat `float` array, x,y,z interleaved: index `vi*3 + {0,1,2}` | `.usf:36`, `.usf:77-79` |
| Weight/index packing | flat arrays, `K` slots per vertex, interleaved: `vi*K + k` | `.usf:61,64` |
| Node displacement packing | flat `float` array, `ni*3 + {0,1,2}` | `.usf:65` |
| Buffer kind | **typed buffers** (`Buffer<float>`/`Buffer<int>`/`RWBuffer<float>`), element stride 4 B — NOT `StructuredBuffer<float3>` | `.usf:15-19`; `.cpp:197,206,211,213` |
| Weight normalization | PRE-normalized at bake (`BindToCage` already `/wsum`); shader **sums, never re-divides** | `.usf:9-10,57`; `GpuVehicleCageComponent.cpp:515` |
| Unused bind slot sentinel | node index `< 0` (`INDEX_NONE == -1`); slot skipped, weight 0 | `.usf:10,61-62`; `GpuVehicleCageComponent.cpp:514` |
| Weight endian/versioning/magic | none — raw GPU buffers, no header/magic/version | — |

## 3. Compute shader I/O (the buffer contract)

Shader entry: `MainCS` in `VehicleDeform.usf`; C++ binding `FVehicleDeformCS` in `VehicleDeformShader.h`.

### Resource bindings (per body section)

| HLSL name | Binding | RHI element format | Element count | Contents | Lifetime | Source |
|---|---|---|---|---|---|---|
| `RestPositions` | SRV `Buffer<float>` | `PF_R32_FLOAT` | `NumVerts*3` | actor-space rest verts (x,y,z) | static (per section) | `.usf:15`; `.cpp:206` |
| `BindNodeIdx` | SRV `Buffer<int>` | `PF_R32_SINT` | `NumVerts*K` | cage node index per slot; `<0`=unused | static; re-uploaded on tear | `.usf:16`; `.cpp:211,334` |
| `BindNodeWt` | SRV `Buffer<float>` | `PF_R32_FLOAT` | `NumVerts*K` | pre-normalized inverse-distance weights | static; re-uploaded on tear | `.usf:17`; `.cpp:213,337,380` |
| `NodeDisp` | SRV `Buffer<float>` | `PF_R32_FLOAT` | `NumNodes*3` | per-frame cage displacement (cage space) | **recreated every frame**, shared by all sections | `.usf:18`; `.cpp:285-287` |
| `OutPositions` | UAV `RWBuffer<float>` | `PF_R32_FLOAT` | `NumVerts*3` | deformed actor-space verts; **also bound as the position vertex stream** | static; written every dispatch | `.usf:19`; `.cpp:197-198,298` |

### Loose/scalar parameters (UE packs these into a param struct; port to a `cbuffer`)

| HLSL name | Type | Meaning | Set from | Source |
|---|---|---|---|---|
| `NumVerts` | `uint` | vertices in this section (thread bound) | `S->NumVerts` | `.usf:21`; `.cpp:299` |
| `NumNodes` | `uint` | cage nodes this frame (`NodeDisp.Num/3`) | `NodeDispFloats.Num()/3` | `.usf:22`; `.cpp:265,300` |
| `K` | `uint` | bind slots per vertex (clamped 1..8, default 4) | `SkinK` clamp | `.usf:23`; `.cpp:301,492,599` |
| `bMirrorY` | `uint` | 1 ⇒ flip disp.y (cage→actor) | `bBuiltMirrorY` | `.usf:24`; `.cpp:302,506` |
| `Gain` | `float` | crumple scale; **already folds in per-panel `DeformStiffness`** | `Damage->DeformGain * Skin.DeformStiffness` | `.usf:25`; `.cpp:303,510` |
| `MaxD` | `float` | length clamp (cm); 0 ⇒ no clamp | `Damage->MaxBoneDisplacement` (fallback 40) | `.usf:26`; `.cpp:304,511` |
| `bIdentity` | `uint` | 1 ⇒ `Out = Rest` (pipeline-proof debug; set 0 in prod) | constant `0u` | `.usf:27`; `.cpp:305` |
| `DebugOffsetZ` | `float` | ≠0 ⇒ `Out = Rest + (0,0,z)` (debug only) | `oc.GpuDeform.DebugOffsetZ` cvar | `.usf:28`; `.cpp:268,306` |

Note: `DeformStiffness` is folded into `Gain` on the CPU (`.cpp:510`) — the shader does **not** multiply by stiffness a second time. Default `MaxBoneDisplacement` is 60 in the damage class (`VehicleDamage.h:438`) but the proxy's no-`Damage` fallback and the struct default are 40 (`.cpp:511`, `.h:60,98`).

## 4. The deform math (exact, must match CPU byte-for-byte)

Parity contract, `.usf:8-11` and `.cpp:57-79`, mirrored by CPU `DeformedVert` (`VehicleCageComponent.cpp:1215-1238`):

```
disp = 0
for k in 0..K:
    ni = BindNodeIdx[vi*K + k]
    if 0 <= ni < NumNodes:                 # CPU: Disp.IsValidIndex(ni)
        disp += NodeDisp[ni] * BindNodeWt[vi*K + k]     # SUM, no re-divide
if bMirrorY: disp.y = -disp.y              # cage -> actor
disp *= Gain                               # Gain already includes DeformStiffness
if MaxD > 0 and length(disp) > MaxD:
    disp *= MaxD / length(disp)            # == FVector::GetClampedToMaxSize(MaxD)
OutPositions[vi] = RestPositions[vi] + disp
```

Order is load-bearing: blend → mirror → gain → clamp → add. `MaxD` clamp scales the **whole vector** by `MaxD/len` (not per-component). (`.usf:57-79`, `.cpp:1219-1230`.)

Scope limitation to carry over: the GPU shader implements **only** the cage-crumple blend. The CPU `DeformedVert` additionally composes a per-section rigid pose and articulated linear-blend bone skin (`VehicleCageComponent.cpp:1232-1259`); those are **not** in the GPU path — skinned sections are excluded from GPU eligibility and fall back to the CPU renderer (`.cpp:499`).

## 5. Dispatch / threadgroup layout

| Property | Value | Source |
|---|---|---|
| Threadgroup size | `[numthreads(64,1,1)]` | `.usf:30` |
| Thread index | `SV_DispatchThreadID.x == vi` | `.usf:31-33` |
| Bounds guard | `if (vi >= NumVerts) return;` | `.usf:34` |
| Groups dispatched | `X = ceil(NumVerts/64)`, `Y=1`, `Z=1` | `.cpp:314` (`DivideAndRoundUp(NumVerts,64)`) |
| Granularity | one dispatch **per section**, per frame | `.cpp:289-316` |
| Shared per frame | one `NodeDisp` buffer for all sections of the same cage | `.cpp:284-287` |

## 6. Per-frame CPU→GPU cage-state upload

Game thread → render thread flow, once per tick when the cage moved:

1. **Gate** (skip when nothing changed): `UpdateGpuDeform` requires `Damage && bBound`; early-outs when `GetDeformVersion()` is unchanged and no bone is active (a settled dent persists in `OutPositions`) (`.cpp:554-563`).
2. **Gather displacements**: `Damage->GetCageDisplacements(Disp)` = `Nodes[i].Position - Nodes[i].LocalRest` for every node, in **cage space, cm** (`.cpp:566`; `VehicleDamage.cpp:1498-1502`).
3. **Flatten** to `float[NumNodes*3]` (x,y,z interleaved) (`.cpp:569-576`).
4. **Marshal** to render thread by value via `ENQUEUE_RENDER_COMMAND` (a moved copy of the float array; no shared mutable state crosses threads) (`.cpp:588-590`).
5. **Upload + dispatch** in `EnqueueDeform_RenderThread` (`.cpp:263-317`):
   - Release last frame's `NodeDisp` buffer, **recreate** it from the upload view and dispatch — deliberately *not* a lock/rename on a dynamic buffer (a rename-on-lock would dangle the SRV) (`.cpp:284-287`).
   - For each visible, bound, non-empty section: fill params, transition `OutPositions` to `UAVCompute`, dispatch, transition back to the read state.

Static per-vertex bind (`RestPositions`, `BindNodeIdx`, `BindNodeWt`) is uploaded **once** — either at proxy creation if `BindToCage` already ran (`.cpp:208-215`), or later via `GpuUploadBind` → `UploadBind_RenderThread`, which builds the index/weight SRVs and flips `bHasBind` to start dispatching (`.cpp:322-344,595-619`). `bHasBind==false` ⇒ section renders rest only, no dispatch (`.cpp:291`).

## 7. Deformed vertices → renderer

The single most important integration detail (`.cpp:69-83,225-234`):

- `OutPositions` is created as a **UAV + SRV + vertex buffer** (dual-use). `FRWBuffer::Initialize` sets `BUF_VertexBuffer` so the same RHI buffer can be a vertex stream.
- It is bound as the **position vertex STREAM** (`Data.PositionComponent`, `VET_Float3`, stride 12, offset 0), not merely as an SRV — because every raster pass (base/GBuffer, depth, shadow, velocity) reads position from the bound stream, while the SRV path is only manual-vertex-fetch/ray-tracing. A tiny `FVertexBuffer` wrapper aliases `OutPositions.Buffer` (one allocation, no copy). The SRV is *also* set (`Data.PositionComponentSRV`) for the MVF/triangle-sort paths (`.cpp:229-232`).
- Static streams (tangents+UVs interleaved, color, indices) come from CPU-filled buffers exactly like a normal static mesh; the rest-position CPU buffer is kept only as the source of the initial upload and is **not** bound to the factory (`.cpp:161-181,221-223`).
- On the port: bind the compute UAV's underlying buffer as the position vertex input (VB slot 0, R32G32B32_FLOAT, stride 12). Ensure a UAV→vertex-buffer barrier between dispatch and draw.

## 8. RHI state / barriers (must reproduce)

`OutPositions` is created in the dual read state `VertexOrIndexBuffer | SRVMask` (so the first pre-dispatch barrier's old-state matches the stream read) (`.cpp:195-198`). Each frame per section (`.cpp:312-315`):

```
barrier OutPositions: (VertexOrIndexBuffer | SRVMask) -> UAVCompute
Dispatch(ceil(NumVerts/64),1,1)
barrier OutPositions: UAVCompute -> (VertexOrIndexBuffer | SRVMask)
```

Port mapping: DX12 = `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` ↔ `VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE` (transition + UAV barrier); Vulkan = `VK_ACCESS_SHADER_WRITE_BIT` ↔ `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | SHADER_READ` with matching pipeline stages.

## 9. In-place topology updates (avoid full rebuild)

These are cheap render-thread mutations that avoid reallocating all sections (the crash-frame lag spike). Reproduce as targeted buffer swaps:

| Op | Effect | Buffers touched | Source |
|---|---|---|---|
| Hide section | `bVisible=false` flag; skipped by draw + dispatch loops (opens a hole for a shed panel) | none | `.cpp:349-358,626-632` |
| Retear section | swap the section's **index buffer** (reduced tri set) + **re-upload weight SRV** (torn nodes zeroed & survivors renormalized on CPU, GPU must match) | index + `BindNodeWt` only | `.cpp:364-384,634-645` |
| Upload bind | build index/weight SRVs post-`BindToCage`, set `bHasBind` | `BindNodeIdx` + `BindNodeWt` | `.cpp:322-344` |

## 10. Eligibility gate (when the GPU path runs)

`TryCreateGpuDeformProxy` returns the GPU proxy only when: `DriveMode==GpuCompute` AND `bRenderBody` AND SM5+ feature level AND there is ≥1 non-broken, **non-skinned** section with verts+triangles; otherwise falls back to the CPU (ProceduralMesh) renderer (`.cpp:478-546`). Per section it skips `bBroken` (shed → hole) and `bSkinned` (rigged → CPU) (`.cpp:498-499`). Port this as a capability check + a CPU fallback path.

## 11. Shader registration / include resolution

- Virtual path: the C++ references the shader as `/OCShaders/Private/VehicleDeform.usf`, mapped to `<plugin>/Shaders` on disk at module load (`OCShaders.cpp:13-20`; `VehicleDeformShader.cpp:7`).
- Entry point `MainCS`, stage compute (`SF_Compute`) (`VehicleDeformShader.cpp:7`).
- Only compiled for SM5+ (`VehicleDeformShader.h:36-39`).
- Port: replace the virtual-path shader registry with your own include resolver; compile `VehicleDeform.usf`→HLSL with DXC to DXIL/SPIR-V.

## 12. UE dependencies to strip/replace in the port

| UE type / API | Role | Replace with |
|---|---|---|
| `FGlobalShader`, `DECLARE_EXPORTED_GLOBAL_SHADER`, `IMPLEMENT_GLOBAL_SHADER`, `SHADER_USE_PARAMETER_STRUCT` | shader class + registration | your PSO/shader object + a compute pipeline |
| `BEGIN_SHADER_PARAMETER_STRUCT` / `SHADER_PARAMETER_SRV` / `_UAV` / `_(scalar)` | auto-reflected bindings | explicit root signature / descriptor set layout + a constant buffer for scalars |
| `FComputeShaderUtils::Dispatch` | dispatch helper | `ID3D12GraphicsCommandList::Dispatch` / `vkCmdDispatch` |
| `TShaderMapRef<>`, `GetGlobalShaderMap` | shader lookup | your compiled-shader/PSO cache |
| `FRWBuffer` (UAV+SRV+VB), `FReadBuffer` (SRV) | typed buffer wrappers | RHI buffer create with UAV+SRV(+VB) / SRV views, `R32_FLOAT` & `R32_SINT` |
| `FResourceArrayUploadArrayView` | create-with-initial-data | upload heap / staging buffer at buffer creation |
| `ERHIAccess`, `FRHITransitionInfo`, `RHICmdList.Transition` | barriers | D3D12 resource barriers / Vulkan pipeline barriers (§8) |
| `FLocalVertexFactory`, `FVertexStreamComponent`, `FStaticMeshVertexBuffers`, `FDynamicMeshIndexBuffer32` | vertex input + static streams | your input-layout binding the UAV buffer as position VB + normal static VBs/IB |
| `FPrimitiveSceneProxy`, `GetDynamicMeshElements`, `FMeshBatch`, `FMaterialRenderProxy` | draw submission | your renderer's mesh/draw record |
| `ENQUEUE_RENDER_COMMAND`, `FRHICommandListImmediate` | game→render marshaling | your render command queue |
| `AddShaderSourceDirectoryMapping`, `/OCShaders/...` virtual path | shader include resolution | your shader include system |
| `IsFeatureLevelSupported(..., SM5)` | capability gate | your RHI feature/SM query |
| `#include "/Engine/Public/Platform.ush"` | UE shader prelude | remove for plain-HLSL/DXC (§13) |
| `FDynamicMeshVertex`, `FProcMeshTangent`, `PF_R32_*` | vertex/format types | your vertex struct + DXGI/VK formats (`R32_FLOAT`, `R32_SINT`) |
| `TAutoConsoleVariable` (`oc.GpuDeform.DebugOffsetZ`) | debug cvar | optional debug toggle |

## 13. HLSL-in-USF → plain HLSL for DXC

The `.usf` is nearly plain HLSL; changes needed:

1. **Remove** `#include "/Engine/Public/Platform.ush"` (`.usf:13`) — UE-only prelude. Not needed for standard HLSL under DXC.
2. **Wrap scalars in a `cbuffer`.** The globals `NumVerts, NumNodes, K, bMirrorY, Gain, MaxD, bIdentity, DebugOffsetZ` (`.usf:21-28`) are loose in USF because UE's reflection auto-packs them. For DXC + an explicit root signature, put them in `cbuffer Params : register(b0) { ... }`.
3. **Add explicit register bindings.** The typed buffers have no `register()` in USF (UE assigns via reflection). Add `Buffer<float> RestPositions : register(t0);`, `Buffer<int> BindNodeIdx : register(t1);`, `Buffer<float> BindNodeWt : register(t2);`, `Buffer<float> NodeDisp : register(t3);`, `RWBuffer<float> OutPositions : register(u0);` (any consistent order matching the root signature).
4. **Everything else is standard HLSL** and compiles under DXC unchanged: `Buffer<float>`/`Buffer<int>`/`RWBuffer<float>` (typed SRV/UAV, NOT structured), `[numthreads(64,1,1)]`, `SV_DispatchThreadID`, `[loop]`, `length()`, `float3` swizzles. Keep the flat `*3`/`*K` addressing.
5. Optionally drop the `bIdentity`/`DebugOffsetZ` branches (`.usf:38-55`) — debug-only pipeline proofs.

## 14. Source file map

| Concern | File |
|---|---|
| Compute shader (authoritative math) | `Plugins/OCShaders/Shaders/Private/VehicleDeform.usf` |
| Shader C++ binding / param struct | `Plugins/OCShaders/Source/OCShaders/Public/VehicleDeformShader.h` |
| Shader registration | `Plugins/OCShaders/Source/OCShaders/Private/VehicleDeformShader.cpp` |
| Virtual-path module | `Plugins/OCShaders/Source/OCShaders/Private/OCShaders.cpp` |
| Scene proxy, buffers, dispatch, upload, tear/hide | `Source/OpenConstructor27/Private/GpuVehicleCageComponent.cpp` |
| Bind/vert data (`FPartSkin`) | `Source/OpenConstructor27/Public/VehicleCageComponent.h:446-506` |
| CPU parity (`DeformedVert`) | `Source/OpenConstructor27/Private/VehicleCageComponent.cpp:1215-1261` |
| Displacement source (`GetCageDisplacements`) | `Source/OpenConstructor27/Private/VehicleDamage.cpp:1498-1502` |