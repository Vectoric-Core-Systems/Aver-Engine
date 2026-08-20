# Aver Engine — Asset Format System Design (`AVR1` container + `.oc*` family)

Status: authoritative design spec, v1. Target repo: `C:/Users/User/Documents/Aver Engine`. Scope: on-disk formats only (container, legacy preservation, seven new native formats, versioning, cook/DDC, migration, C ABI loader surface). All numeric widths are exact and little-endian unless stated. This document is implementation-ready: a C++ writer/reader, a Rust cooker, and a C# importer can be built directly from the byte tables below.

---

## 0. Design principles (why this shape)

1. **Two-tier: preserve-as-text, add-as-container.** The four legacy OpenConstructor formats (`.ocbeam`, `.ocaero`, `.ocmap`, `.scene`) are line-based UTF-8 text that must load *byte-identically*. They are **not** wrapped, re-encoded, or containerized. New bulk asset types get a single unified binary container (`AVR1`). New graph/authoring types stay human-readable text in the same lexical dialect as the OC family (`#` comments, `KEY{ ... }` sections, comma rows, optional trailing `;`).
2. **Pay-for-what-you-use / modular.** Everything is a chunked container: a reader loads only the chunks it needs; unknown chunks are skipped, not fatal. No monolithic "everything asset." This is the anti-Unreal-bloat contract at the format layer.
3. **Author-once / consume-by-both.** Formats carry no engine-runtime pointers, no reflection blobs. The same file feeds the C++ renderer, the C# editor, and the Rust cooker across the C ABI. This mirrors the existing `OCNetTypes`/`oc_sim` philosophy (plain data crosses every seam).
4. **One coordinate contract, everywhere** (from recon `arch §1`): **cm, +Z up, +X forward, +Y right, left-handed.** Import from glTF/Blender applies the fixed conversion `(x,y,z) → (x,-z,y)` with `SCALE = 0.01` (m→cm), plus the render/physics `bMirrorY` correction. These conversions happen **at import**, never at runtime load — every native binary file is already in engine space.
5. **Permissively-licensed only.** Container hashing = xxHash3 / BLAKE3 (BSD-2 / CC0-Apache). Compression = zstd + LZ4 (both BSD). Texture codecs = BCn (unencumbered DX standard), ASTC (Khronos permissively-licensed), Basis Universal / KTX2 (Apache-2.0). Image decode = stb_image (public domain). glTF/JSON parse = self-contained (the OCCompiler already ships one). No GPL, no patent-bearing tech.
6. **GPU-driven-ready.** `.ocmesh` ships meshlets + cull cones so a mesh-shader / GPU-culling path (UE5-Nanite-class, Source-2-class) is possible on DX12/Vulkan, while the position stream stays a standalone `R32G32B32_FLOAT` buffer so the existing GPU cage-deform compute pass (recon `gpudeform §7`) can alias it as a UAV with zero copy.

---

## 1. Format taxonomy

| Ext | Kind | On-disk | Container | Purpose | Status |
|---|---|---|---|---|---|
| `.ocbeam` | Soft-body cage source | **text** | none | Vehicle mass-spring cage + embedded glTF + rig + collision + aero directives | **PRESERVE (unchanged)** |
| `.ocaero` | Baked aero table | **text** | none | Wind-tunnel force/CoP grid over yaw×pitch×ride | **PRESERVE (unchanged)** |
| `.ocmap` | Compiled world | **text** | none | Object-reference placements + env table | **PRESERVE (unchanged)** |
| `.scene` | World authoring input | **text** | none | Human-authored placement list (→ `.ocmap`/`.ocworld`) | **PRESERVE (unchanged)** |
| `.octrack` | Track variant of `.ocmap` | **text** | none | Same grammar as `.ocmap` (kept for content compat) | **PRESERVE (unchanged)** |
| `.ocmesh` | Static mesh | **binary** | `AVR1`/`MESH` | Vertex streams, indices, submeshes, LODs, meshlets, bounds, optional collision + cage-bind | **NEW** |
| `.octex` | Texture | **binary** | `AVR1`/`TEX ` | Mip chain over BCn/ASTC/Basis/raw, color space, sampler | **NEW** |
| `.ocmat` | Material | **text** (cooked → `MATL` chunk) | none / `AVR1` | PBR metallic-roughness params + texture bindings + optional node graph | **NEW** |
| `.ocskel` | Skeletal mesh + skeleton | **binary** | `AVR1`/`SKEL` | Bones + skin binding + embedded `.ocmesh` geometry | **NEW** |
| `.ocanim` | Animation clip | **binary** | `AVR1`/`ANIM` | Full-fidelity keyframe TRS tracks (fixes 30fps/cubic loss) | **NEW** |
| `.ocprefab` | Prefab | **text** | none | Reusable node tree + component list + asset refs + overrides | **NEW** |
| `.ocgraph` | Visual scripting graph (Aver Node) | **text** | none | Nodes/pins/links for dataflow or event-driven behaviour; optionally declares itself a spawnable actor class | **NEW** |
| `.ocworld` | Native scene/world | **text** (cooked → `WRLD` chunk) | none / `AVR1` | **Superset of `.ocmap`** — scene graph, streaming, lighting, terrain, prefab instances, class-instance placement | **NEW** |
| `.ocpak` | Cooked package | **binary** | `AVR1`/`PAK ` | Bundle of cooked assets for shipping (DDC output) | **NEW** |
| `.ocparticle` | Particle effect | **text** | none | Emitter shape/rate, lifetime, direction+spread, speed, gravity, damping, size- and colour-over-life, blend mode, GI opt-in | **NEW** |

Rationale for text vs binary: **graphs and small authoring data stay text** (materials, prefabs, worlds, the legacy family) — diffable, hand-editable, mergeable in VCS, matching OC ergonomics. **Bulk geometry/pixel/track data is binary** (mesh, texture, skeletal, animation) — mmap-able, GPU-uploadable, compact. Text formats have a *cooked* binary projection (a chunk inside `.ocpak`) for shipping; the text form remains the source of truth.

---

## 2. Common conventions (shared by all native binary formats)

### 2.1 Coordinate system, units, handedness
Identical to recon `arch §1`: **length = cm (f32); +Z up; vehicle-local +X fwd / +Y right; left-handed.** Rotations stored as quaternions `(x,y,z,w)`, identity `(0,0,0,1)`. Matrices, where stored, are **row-major 4×4** in engine convention (import transposes glTF's column-major). All spatial data in a native binary file is already engine-space (conversions done at import).

### 2.2 Endianness, alignment, padding
- **Endianness: little-endian** for every multi-byte field (x64/ARM64 native; matches all recon wire/asset code).
- **Base alignment: 16 bytes.** Every chunk's file offset is a multiple of 16.
- **GPU-upload alignment: 256 bytes.** Chunks flagged `GPU_UPLOADABLE` (vertex/index/meshlet/texture-mip data) are aligned to 256 so they can be `memcpy`'d straight into a D3D12/Vulkan upload heap without a re-pack.
- Padding bytes are zero. Reserved fields are zero. Readers must tolerate (skip) nonzero reserved bytes for forward compat unless a flag says otherwise.

### 2.3 Primitive encodings (used throughout)
| Name | Layout | Notes |
|---|---|---|
| `u8/u16/u32/u64`, `i8..i64` | LE integers | — |
| `f16` | IEEE half (16-bit) | UV / HDR quantization |
| `f32/f64` | IEEE float/double | positions f32; DDC timestamps f64 |
| `FourCC` | 4 ASCII bytes as `u32` LE | e.g. `'MESH'` = `0x4853454D` |
| `GUID` | 16 bytes | 128-bit asset identity (see §2.5) |
| `StringRef` | `u32` | byte offset into the file's `STRT` chunk; `0xFFFFFFFF` = null |
| `STRT entry` | `u16 len` + `len` UTF-8 bytes | at the referenced offset (matches net-wire `[u16 len]` string convention) |
| `vec3f` | 3×`f32` (12 B) | position/normal |
| `quat` | 4×`f32` (16 B) or QTangent (§5.4) | rotation |
| `AABB` | 6×`f32` (24 B) | minXYZ, maxXYZ |
| `Sphere` | 4×`f32` (16 B) | centerXYZ, radius |

**Quantized position decode** (when `PositionQuantized`): stored `R16G16B16A16_UNORM` `q`; `pos = AABB.min + (q/65535) * (AABB.max - AABB.min)`.
**Octahedral normal** (alt normal format): standard oct decode of `RG16_SNORM`.
**QTangent decode:** see §5.4.

### 2.4 Hashing & compression (permissively-licensed)
- **Chunk & content hashes: xxHash3-64** (BSD-2). Fast integrity + change detection.
- **Content-addressed DDC / Merkle roots: BLAKE3-256** (CC0/Apache-2.0). Ties into the net map-parity `mapRoot` (32 B) and `HashAlgo` enum (`Blake3=2`).
- **Legacy compatibility hash: FNV-1a-64** retained exactly (recon `ocmap §3.1.1`) for `.ocmap`/`.ocworld` `ID` content-id so existing worlds keep the same id.
- **Compression per chunk:** `0=none`, `1=zstd`, `2=lz4`. Source assets: none. Cooked: zstd (level tuned per chunk class). GPU chunks in a cooked package may be `none` if the target streams uncompressed, or zstd with a decompress-to-upload-heap step.

### 2.5 Asset identity (GUID) and cross-references
Every native asset carries a stable 128-bit `GUID` in its container header. Cross-asset references use `GUID` (primary) plus a `StringRef` fallback path for human tooling. This generalizes the existing single-id contract (`FOCVehicleId`, `.ocbeam` NODE id ↔ bone `node_<id>`, `.ocaero` `PART.Name` ↔ `.ocbeam` `PART`). The importer maintains a **GUID ↔ source-path** map (`.ocmeta` sidecars, §13) so moving files never breaks references.

---

## 3. The unified container — `AVR1`

### 3.1 File header (fixed 64 bytes, offset 0)
| Off | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 4 | FourCC | `Magic0` = `'AVR1'` (`0x31525641`) | primary container magic |
| 0x04 | 4 | FourCC | `Subtype` | `'MESH'`,`'TEX '`,`'SKEL'`,`'ANIM'`,`'MATL'`,`'PREF'`,`'WRLD'`,`'PAK '` |
| 0x08 | 2 | u16 | `ContainerVersion` | framework version = **1** |
| 0x0A | 2 | u16 | `ContentVersion` | per-subtype schema version |
| 0x0C | 2 | u16 | `MinReaderVersion` | reader must be ≥ this ContainerVersion or refuse |
| 0x0E | 1 | u8 | `Endianness` | `0`=LE (only value in v1) |
| 0x0F | 1 | u8 | `AlignLog2` | base align = `1<<AlignLog2`; default `4` (16 B) |
| 0x10 | 4 | u32 | `HeaderSize` | = 64 (allows future growth) |
| 0x14 | 4 | u32 | `ChunkCount` | number of chunk-directory entries |
| 0x18 | 8 | u64 | `ChunkDirOffset` | file offset of the chunk directory |
| 0x20 | 8 | u64 | `FileSize` | total bytes (integrity) |
| 0x28 | 16 | GUID | `AssetGuid` | stable asset identity |
| 0x38 | 4 | u32 | `Flags` | bit0 `Cooked`, bit1 `Compressed`, bit2 `HasSourceHash`, bit3 `BigEndianReserved` |
| 0x3C | 4 | u32 | `HeaderCrc` | CRC32C of bytes 0x00–0x3B (cheap header sanity) |

Magic disambiguation: text formats have no magic bytes at 0x00 (`.ocmat`/`.ocworld`/`.ocprefab` begin with an `OCMAT 1` / `OCWORLD 1` / `OCPREFAB 1` version line, §7/§10/§11). A loader dispatches by reading the first 4 bytes: `'AVR1'` → binary container; else → text line-parser. Legacy `.ocbeam`/`.ocaero`/`.ocmap`/`.scene` keep their own `OCBEAM 1`/`OCAERO 1`/`OCMAP 1` first lines (unchanged).

### 3.2 Chunk directory (array of `ChunkCount` × 40-byte entries, at `ChunkDirOffset`)
| Off | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 4 | FourCC | `ChunkId` | e.g. `'MHDR'`,`'VTXS'`,`'IDXS'`,`'MLET'`,`'STRT'` |
| 0x04 | 2 | u16 | `ChunkVersion` | per-chunk schema version |
| 0x06 | 1 | u8 | `Compression` | `0`none `1`zstd `2`lz4 |
| 0x07 | 1 | u8 | `ChunkFlags` | bit0 `Required` (unknown+required ⇒ refuse), bit1 `GpuUploadable` (256-align), bit2 `Text` |
| 0x08 | 8 | u64 | `Offset` | file offset of chunk payload (aligned) |
| 0x10 | 8 | u64 | `SizeOnDisk` | compressed bytes |
| 0x18 | 8 | u64 | `SizeUncompressed` | == SizeOnDisk if Compression=0 |
| 0x20 | 8 | u64 | `Hash` | **xxHash64** of the **uncompressed** payload (see note) |

> **Hash note (v1, as implemented).** This field originally specified xxHash3-64. The implementation
> (`modules/formats/src/Avr1.cpp`) uses **xxHash64** — the same author's earlier function — because
> xxHash3 is not vendored in this tree and adding a third-party dependency is not a decision a format
> reader should take on its own. The field's size, offset and purpose are unchanged; only the
> algorithm differs. This is recorded here rather than left as a silent divergence, because a reader
> built to the original text would report every file written by this engine as corrupt. Changing to
> xxHash3 later is a `ContainerVersion` bump, not a silent swap.

**Chunk framework rules (the compat contract):**
- Chunks may appear in any order; the directory is authoritative. Duplicate `ChunkId` is illegal except where a spec says a chunk is arrayed (none in v1).
- **Forward compat:** a reader encountering an unknown `ChunkId` **skips it** unless `Required` is set (then it refuses to load and reports the id). This lets v2 add chunks that v1 readers ignore.
- **Backward compat:** a reader on a newer file checks `MinReaderVersion`; a writer only sets `Required` on chunks whose absence would corrupt interpretation.
- **Chunk versioning:** within a known chunk, additive fields grow the struct; readers use `SizeUncompressed` and documented field offsets, reading `min(known_size, on_disk_size)` and zero-filling the rest. Never reorder existing fields.
- Every file with `StringRef`s carries exactly one `STRT` chunk (the string table).

### 3.3 String table chunk `STRT`
A blob of `u16`-length-prefixed UTF-8 entries. `StringRef` = byte offset of an entry; entry = `[u16 len][len bytes]`. Offset `0xFFFFFFFF` = null. Offset `0` is reserved to always contain the empty string (`len=0`).

---

## 4. Legacy preservation — the load-direct contract

These four (plus `.octrack`) are **frozen text formats**. The engine ships faithful parsers that reproduce the exact semantics extracted in recon. No migration, no re-save required to load. Rules:

### 4.1 `.ocbeam` (recon `ocbeam`)
- Reader = a UE-free reimplementation of `ParseOcbeam`: line-trim, single trailing `;` strip, `#` comments, section keywords by `StartsWith`, `}` resets mode; `MATERIAL/NODE/BEAM/PANEL/PART`; skip-until-`}` for `BONE/SKIN/ANIM`; `GLB/COLLISION/HULL` mode-none; `REBOUND`/`SCALE`/`NORMALIZE`/`GLBXFORM` directives.
- **MATERIAL field-count fix (recon `ocbeam §11.1`):** accept **10–13** fields; fields 10–12 default (`TearStrainTension=-1`, `TearStrainCompression=-1`, `Density=1.0`). This resolves the shipped-parser-drops-13-field bug; existing 10-field samples still load.
- Coordinate/units preserved exactly (cm, X fwd/Y right/Z up; glTF→cage `(x,-z,y)`; render Y-mirror). PART `DETACH=` position-independent; PANEL 4th material override supported; `PartID` prefix parsed-and-discarded.
- The **embedded `GLB{}`** is extracted at import time and cooked into `.ocmesh`/`.octex`/`.ocmat`/`.ocskel` (§14.2). At *runtime load* of a loose `.ocbeam`, the glb path is skipped exactly as the shipped runtime does.

### 4.2 `.ocaero` (recon `ocaero`)
- Reader reproduces `ParseOcaero`: `#` strip, `OCAERO` line skipped, `HEADER{}`/`PART{}`, `ParseFloats` token heuristic, nearest-axis snap, flatten `slot = (yi*NP+pi)*NR+ri`, 9- and 10-field rows, ascending-axis assumption. Trilinear `SampleBaked` and the `flowDir`/inverse-attitude conventions are runtime concerns, not format concerns — the *format* is preserved verbatim.
- Fix flagged but non-breaking: **validate** the `OCAERO 1` version (recon notes it's currently unchecked); reject unknown major versions.

### 4.3 `.ocmap` / `.octrack` / `.scene` (recon `ocmap`)
- `.scene` reader: `#` truncate, trim, optional `;`, `\s+` tokenize, `MAP`/`CLIENT umap`/placement rows (`asset x y z yaw pitch roll [scale]`), `.ocbeam`→DEFORM classification, `d()` parse-fail→0.0.
- `.ocmap` reader: full record catalog (`OCMAP/ID/NAME/BUILD/ALGO/ROOT/CLIENT/SURFACE/GROUND/KILLZ/SPAWN/PLACE/DEFORM`), tolerates `SPAWN` (python-only), **accepts mixed LF/CRLF per line** (recon documented the writer's mixed endings).
- **`ID` = FNV-1a-64(NAME)** reproduced exactly (verified `demoworld → 0x376B85BC4D1A03BA`). **`ROOT`** currently a placeholder; the Aver writer emits a real BLAKE3-256 Merkle over placements+asset-content-hashes (recon-intended), but the **reader accepts any 64-hex ROOT** for back-compat.
- **Port fixes (writer only):** emit uniform **LF**; use **invariant/C locale** for floats (kills the comma-decimal corruption bug). Readers stay maximally tolerant.

### 4.4 Round-trip guarantee
`.ocworld` is a strict superset of `.ocmap` (§11). Loading an `.ocmap` and re-saving as `.ocworld` is lossless; downgrading an `.ocworld` that uses only `.ocmap`-expressible features back to `.ocmap` is lossless. `.scene` → `.ocmap`/`.ocworld` is the existing compile step, preserved.

---

## 5. `.ocmesh` — static mesh (full byte layout)

Subtype `'MESH'`. This is the centerpiece. Chunks: `MHDR` (required), `STRT`, `VTXS` (vertex data), `IDXS` (index data), `MLET` (meshlets, optional), `COLL` (collision, optional), `CBND` (cage-bind, optional), `MADR` (material-slot table).

### 5.1 `MHDR` — mesh header
| Off | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 4 | u32 | `MeshFlags` | see below |
| 0x04 | 1 | u8 | `LODCount` | ≥1 |
| 0x05 | 1 | u8 | `SubmeshCount` | material-slot groups |
| 0x06 | 1 | u8 | `StreamCount` | vertex-stream descriptors |
| 0x07 | 1 | u8 | `UVChannelCount` | 0–4 |
| 0x08 | 24 | AABB | `BoundsAABB` | whole-mesh, engine cm |
| 0x20 | 16 | Sphere | `BoundsSphere` | whole-mesh |
| 0x30 | 4 | u32 | `MaterialSlotCount` | == SubmeshCount unless slots shared |
| 0x34 | 4 | u32 | `BuilderVersion` (was `Reserved`) | see below |
| 0x38 | … | StreamDesc[StreamCount] | vertex-stream table | 8 B each (§5.2) |
| … | … | LodDesc[LODCount] | LOD table | 56 B each (§5.5) |
| … | … | SubmeshDesc[SubmeshCount] | mesh-level submesh table | 12 B each (§5.6) |
| … | … | SubmeshRange[LODCount*SubmeshCount] | per-LOD ranges | 40 B each (§5.6) |

`MeshFlags`: bit0 `HasColor`, bit1 `HasUV1`, bit2 `HasMeshlets`, bit3 `Index32` (else 16-bit), bit4 `PositionQuantized`, bit5 `HasSkin` (JOINTS/WEIGHTS streams present — used by `.ocskel`), bit6 `HasCollision`, bit7 `HasCageBind`, bit8 `Deformable` (position stream MUST stay `R32G32B32_FLOAT` + standalone for GPU cage-deform), bit9 `TwoSided`, bit10 `NegativeScaleBaked` (winding pre-reversed for the `bMirrorY` case, recon `arch §1`).

`BuilderVersion`: the offset formerly documented as `Reserved` (and, per §2.3's "Reserved fields are zero" rule, written as `0` by every writer before this one — that history is exactly what makes `0` safe to keep meaning "unknown/stale" rather than requiring a migration). Zero means "no meshlet ladder was ever cooked through a version-stamped builder" — either the file predates this field, or it genuinely carries no meshlets/coarserLods. A nonzero value is `aver::trifactor::kBuilderVersion` (`modules/trifactor/include/aver/trifactor/ClusterBuilder.hpp`) as it stood when `aver::trifactor::packLodDag` last wrote this mesh's `MLET`/coarser-LOD data; that constant is bumped whenever a change to Aver.Trifactor's cook algorithm changes what a re-cook would produce, so a reader compares its own build's `kBuilderVersion` against this field to tell a current ladder from a stale one without re-deriving anything. `kBuilderVersion` itself starts at `1`, never `0`, specifically so an old `Reserved == 0` file can never be misread as "matches the current builder" by coincidence — see `RelodTool`'s `--write` staleness report for the tool that reads this field today.

### 5.2 `StreamDesc` (8 bytes each)
| Off | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 1 | u8 | `Semantic` | 0 POSITION, 1 TANGENT_FRAME, 2 UV0, 3 UV1, 4 COLOR, 5 JOINTS, 6 WEIGHTS |
| 0x01 | 1 | u8 | `Format` | vertex-format enum (§5.3) |
| 0x02 | 1 | u8 | `BindSlot` | physical buffer/binding index |
| 0x03 | 1 | u8 | `Flags` | bit0 `Interleaved` |
| 0x04 | 2 | u16 | `Stride` | bytes between consecutive verts in this buffer |
| 0x06 | 2 | u16 | `OffsetInStride` | byte offset within the interleaved vertex |

**Canonical stream grouping** (mirrors UE `FStaticMeshVertexBuffers`, satisfies the GPU-deform requirement recon `gpudeform §7`):
- **BindSlot 0 — Position buffer:** POSITION only, `R32G32B32_FLOAT`, stride 12, tightly packed. Standalone so a compute UAV can alias it and every raster pass (depth/shadow/velocity) reads position from slot 0.
- **BindSlot 1 — Attribute buffer (interleaved):** TANGENT_FRAME (QTangent, 8 B) + UV0 (`R16G16_FLOAT`, 4 B) [+ UV1 4 B] [+ per-vertex extras].
- **BindSlot 2 — Color buffer:** COLOR `R8G8B8A8_UNORM`, 4 B (present iff `HasColor`).
- **BindSlot 3/4 — JOINTS/WEIGHTS** (skeletal only; `HasSkin`).

### 5.3 Vertex-format enum
| Val | Format | Bytes | Typical semantic |
|---|---|---|---|
| 0 | `R32G32B32_FLOAT` | 12 | POSITION (deformable) |
| 1 | `R16G16B16A16_SNORM` | 8 | TANGENT_FRAME (QTangent) |
| 2 | `R16G16_FLOAT` | 4 | UV (half) |
| 3 | `R16G16_UNORM` | 4 | UV (quantized over UV bounds) |
| 4 | `R8G8B8A8_UNORM` | 4 | COLOR / WEIGHTS |
| 5 | `R16G16B16A16_UNORM` | 8 | POSITION (quantized over AABB) |
| 6 | `R8G8B8A8_UINT` | 4 | JOINTS (4×u8) |
| 7 | `R16G16B16A16_UINT` | 8 | JOINTS (4×u16, >255 bones) |
| 8 | `R10G10B10A2_UNORM` | 4 | normal-only (alt octahedral) |
| 9 | `R32G32B32A32_FLOAT` | 16 | POSITION+pad / debug |

### 5.4 Tangent frame (QTangent, format 1)
Full TBN encoded as a unit quaternion, stored `R16G16B16A16_SNORM` (8 B). Decode: `q = snorm16→[-1,1]`, renormalize; `normal = rotate(q, +Z)`, `tangent = rotate(q, +X)`, `bitangent = rotate(q, +Y) * sign`, where the **bitangent handedness sign is folded into the quaternion's `w` sign** (if `w < 0`, flip). This is the Source-2/CryEngine convention: one 8-byte field for the whole basis, mirror-map safe. Importer computes tangents (MikkTSpace-equivalent, public-domain algorithm) from UV0 when the source lacks them.

### 5.5 `LodDesc` (56 bytes each) — points into `VTXS`/`IDXS`/`MLET`
| Off | Size | Type | Field |
|---|---|---|---|
| 0x00 | 4 | u32 | `VertexCount` |
| 0x04 | 4 | u32 | `IndexCount` |
| 0x08 | 8 | u64 | `VtxOffset` (into `VTXS`) |
| 0x10 | 8 | u64 | `VtxSize` |
| 0x18 | 8 | u64 | `IdxOffset` (into `IDXS`) |
| 0x20 | 8 | u64 | `IdxSize` |
| 0x28 | 8 | u64 | `MeshletOffset` (into `MLET`, 0 if none) |
| 0x30 | 4 | u32 | `MeshletCount` |
| 0x34 | 4 | f32 | `ScreenErrorThreshold` (LOD-select metric, screen-space error in px at reference resolution) |

Per-LOD vertex layout inside `VTXS` = the LOD's slot-0 position block, then slot-1 attribute block, then slot-2 color block, each aligned to 16 B, described by the stream strides. `IDXS` holds `IndexCount` × (2 or 4 B) per LOD.

### 5.6 Submeshes
`SubmeshDesc` (12 B, mesh-level): `u32 MaterialSlot`, `StringRef Name`, `u32 Flags`.
`SubmeshRange` (40 B, indexed `[lod*SubmeshCount + submesh]`): `u32 IndexStart`, `u32 IndexCount`, `u32 BaseVertex`, `u32 VertexCount`, `Sphere BoundsSphere` (16 B), `u32 MeshletStart`, `u32 MeshletCount`.

### 5.7 `MLET` — meshlets (GPU-driven rendering)
Limits: **64 vertices / 124 primitives** per meshlet (mesh-shader & DXR friendly). `ChunkVersion 3` (current) layout: an optional `GroupTable` prefix (below) at byte 0, then four back-to-back per-LOD sub-arrays (offsets from the LOD's `MeshletOffset`, which is chunk-relative and therefore already accounts for the `GroupTable`'s own byte length), then an optional per-LOD group section the `GroupTable` points into:
1. **MeshletDesc[]** (12 B each): `u32 VertexIndexOffset` (into sub-array 3), `u32 TriangleOffset` (into sub-array 4, in bytes), `u8 VertexCount`, `u8 TriangleCount`, `u16 Pad`.
2. **MeshletBounds[]** — `ChunkVersion 1`: **32 B each**: `Sphere Bounds` (16 B) + `f32[3] ConeApex` (12 B) + `i8[3] ConeAxis` (snorm) + `i8 ConeCutoff` (snorm). Enables cluster cone-culling.
   `ChunkVersion 2`: **40 B each** — the same 32 B as v1, plus `f32 OwnError` + `f32 ParentError` appended. Together with `LodDesc.ScreenErrorThreshold` (§5.5, still the whole-level value), these turn LOD selection from "one level for the whole mesh" into a per-cluster local test: a runtime draws a cluster iff `OwnError < pixelBudget AND ParentError >= pixelBudget`. `ParentError` is the converted screen-space error of the coarser cluster/group this one feeds into; a root cluster (no coarser group) stores `ParentError = FLT_MAX` (a finite sentinel, not IEEE +inf), so it is always drawn once nothing finer qualifies. The cook-time invariant `OwnError <= ParentError` for every cluster is what makes the test cover a mesh's surface exactly once, with no gaps and no overlap (see `aver::trifactor::validateClusterErrorBounds`).
   `ChunkVersion 3`: **48 B each** — the same 40 B as v2, plus `u32 FallbackAncestorId` + `u32 OwnerGroupId` appended. Both are **level-local indices**, like every other MLET offset/count, and both use `0xFFFFFFFF` (`kInvalidClusterId`, `aver::fmt::kInvalidClusterId`) for "no such cluster/group". `FallbackAncestorId` indexes the NEXT-COARSER LOD's own `MeshletDesc[]`/`MeshletBounds[]`: the coarser cluster a page-residency-aware streaming system should draw instead when this cluster's own page is not resident, chosen ONCE at cook time (the group's own output cluster whose bounding-sphere centre is nearest this cluster's own centre — see `aver::trifactor::Cluster::fallbackAncestorId`'s own comment for why this cannot instead be a runtime walk of parent pointers) and invalid only at the root level, which has no coarser level to fall back to. `OwnerGroupId` indexes THIS SAME LOD's own `ClusterGroupNode[]` (below): the back-reference a traversal needs to recurse past this cluster (pop a cluster id off a coarser group's `ChildClusterRange`, look up its `OwnerGroupId`, and that group's own `ChildClusterRange` is the next, finer set to descend into), invalid only for a LOD-0 cluster, since LOD 0 is built directly and never by a group.
   **Chunk versioning**: `MLET`'s `AvrChunk.Version` (§3.2) selects the stride for the WHOLE chunk (every LOD's MeshletBounds sub-array inside it) — readers must check it, not assume 48 B. A reader on a `ChunkVersion 1` file uses the 32 B stride and leaves `OwnError`/`ParentError`/`FallbackAncestorId`/`OwnerGroupId` at their defaults (`0`, `FLT_MAX`, `kInvalidClusterId`, `kInvalidClusterId`); a `ChunkVersion 2` file uses the 40 B stride and leaves only `FallbackAncestorId`/`OwnerGroupId` at their defaults. This is the additive-growth path §3.2 describes, applied as a version-gated element stride rather than a tail append because `MeshletBounds` is a repeated array, not a single fixed struct. A `ChunkVersion 1`/`2` file's bytes, and every offset computed from them, are read exactly as before `ChunkVersion 3` existed — and nothing in this codebase rewrites an old file's chunk version in place: a file stays at the version it was cooked with until something explicitly re-cooks and re-saves it (e.g. `RelodTool --write`, which never touches its input — see `tools/RelodTool.cpp`).
3. **MeshletVertices[]** (`u32`): indices into the LOD vertex buffer.
4. **MeshletTriangles[]** (`u8`): 3 local indices per triangle (0–63), tightly packed; each meshlet's block padded to 4 B.

**`GroupTable`** (`ChunkVersion 3` only; `LODCount` × 24 B, at byte 0 of the chunk): per LOD level, `u64 GroupNodeOffset` (chunk-relative, 0 if none) + `u32 GroupNodeCount` + `u64 GroupChildOffset` (chunk-relative) + `u32 GroupChildCount`. A `ChunkVersion 1`/`2` reader never looks for this table — it does not exist in those chunks, so every offset such a reader computes stays relative to byte 0 of the chunk exactly as it always has.

**`ClusterGroupNode[]`** (`ChunkVersion 3` only; per LOD level `>= 1`; `GroupNodeCount` × 32 B, at `GroupNodeOffset`): the hierarchy a traversal walks to descend from this LOD level into the finer one it replaced. Per node: `f32[3] SphereCenter` + `f32 SphereRadius` (16 B) — a TRUE sphere-of-spheres containing every child cluster's OWN bounding sphere in full, computed over the group's pre-simplification members, never recomputed from this level's already-simplified geometry (see `aver::trifactor::ClusterGroupNode`'s own comment for why those two are not interchangeable, and why using the wrong one culls away finer children that stick out beyond it) — then `u32 OwnClusterStart` + `u32 OwnClusterCount` (8 B, level-local span of THIS level's own `MeshletDesc[]`/`MeshletBounds[]` this group produced) then `u32 ChildClusterStart` + `u32 ChildClusterCount` (8 B, a span into `ClusterGroupChildren[]` below, not a direct range into the finer level's own arrays — a group's members are whatever `meshopt_partitionClusters` assigned it, not guaranteed contiguous in that level's cluster-id space). LOD 0 never has a `ClusterGroupNode[]` array: it is built directly by `buildClusters`, never by a group.

**`ClusterGroupChildren[]`** (`ChunkVersion 3` only; per LOD level `>= 1`; `GroupChildCount` × `u32`, at `GroupChildOffset`): flat, level-local indices into the NEXT-FINER LOD level's own `MeshletDesc[]`/`MeshletBounds[]`, concatenated one group's worth at a time in the same order as `ClusterGroupNode[]`, and sliced by each node's `ChildClusterStart`/`ChildClusterCount`.

A raster fallback (no mesh shaders): ignore `MLET`, draw `IDXS` directly. Generated by meshoptimizer (`meshopt_buildMeshlets`, MIT) at cook time.

### 5.8 `COLL` — collision (optional, `HasCollision`)
Reuses the cage-hull concept from `.ocbeam COLLISION{}`. Body: `u32 HullCount`, then per hull `{u32 VertCount; vec3f[VertCount]}` (convex hulls, engine cm), then optional `u32 TriMeshVertCount/IndexCount` + data for a concave collision mesh. This lets baked cage collision (`Scripts/bake_cage_collision.py` output) live in the mesh asset.

### 5.9 `CBND` — precomputed cage bind (optional, `HasCageBind`)
Stores the result of `BindToCage` (recon `gpudeform §6`) so the vehicle body skins to its cage without runtime rebind: `u8 K` (1–8), `u32 CageNodeCount`, then per vertex `K`×`{i32 CageNodeId; f32 Weight}` (weights pre-normalized inverse-distance; `-1` node = unused slot). This directly feeds `BindNodeIdx`/`BindNodeWt` GPU buffers. Optional — runtime can still bind on the fly.

---

## 6. `.octex` — texture

Subtype `'TEX '`. Chunks: `THDR` (required), `MIPS` (pixel data, `GpuUploadable`), optional `SRC ` (original PNG/JPEG/KTX2 bytes for re-cook), `STRT`.

### 6.1 `THDR`
| Off | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0x00 | 4 | u32 | `Width` |
| 0x04 | 4 | u32 | `Height` |
| 0x08 | 4 | u32 | `Depth` | 1 for 2D |
| 0x0C | 2 | u16 | `ArrayLayers` | ≥1 (cube = 6) |
| 0x0E | 1 | u8 | `MipCount` |
| 0x0F | 1 | u8 | `Dimension` | 0=2D,1=3D,2=Cube,3=2DArray |
| 0x10 | 2 | u16 | `Codec` | §6.2 |
| 0x12 | 1 | u8 | `ColorSpace` | 0=linear,1=sRGB |
| 0x13 | 1 | u8 | `Swizzle` | packed 4×2-bit (R/G/B/A source) |
| 0x14 | 1 | u8 | `WrapU` | 0 repeat,1 clamp,2 mirror |
| 0x15 | 1 | u8 | `WrapV` | — |
| 0x16 | 1 | u8 | `Filter` | 0 point,1 linear,2 aniso |
| 0x17 | 1 | u8 | `Flags` | bit0 `Premultiplied`, bit1 `NormalMap`, bit2 `HasSource` |
| 0x18 | … | MipDesc[MipCount*ArrayLayers] | mip table | 24 B each |

`MipDesc` (24 B): `u32 Width`, `u32 Height`, `u64 Offset` (into `MIPS`), `u64 SizeOnDisk` (per-mip may be individually zstd'd for streaming).

### 6.2 Codec enum (permissively-licensed)
| Val | Codec | Use / license |
|---|---|---|
| 0 | `RGBA8_RAW` | uncompressed 32-bpp |
| 1 | `RGBA16F_RAW` | HDR uncompressed |
| 2 | `BC1` | RGB/1-bit A (DX standard, unencumbered) |
| 3 | `BC3` | RGBA |
| 4 | `BC4` | 1-channel (roughness/height) |
| 5 | `BC5` | 2-channel (normal XY) |
| 6 | `BC6H` | HDR |
| 7 | `BC7` | high-quality RGBA |
| 8 | `ASTC_4x4`…`ASTC_8x8` (sub-enum in `Swizzle` hi bits) | Khronos permissively-licensed |
| 9 | `BASIS_UASTC` (KTX2) | Apache-2.0, transcodes to BCn/ASTC/ETC at load |
| 10 | `BASIS_ETC1S` (KTX2) | Apache-2.0, smallest |

Cook policy: albedo/emissive → BC7 (sRGB) or Basis-UASTC for cross-platform; normal → BC5; ORM/mask → BC4/BC7; HDR/IBL → BC6H. Source PNG/JPEG decoded with stb_image at import; the `SRC ` chunk optionally retains original bytes so a different platform target can re-transcode from a KTX2/UASTC intermediate without the original file.

---

## 7. `.ocmat` — material (text, cooked → `MATL` chunk)

Human-readable, OC-dialect. First line `OCMAT 1`. PBR metallic-roughness matching the glTF import model (recon `assets §0`, `arch §6`). Example:

```
OCMAT 1
# Ferrari 499P carbon body
SHADER standard            # standard | unlit | clearcoat | glass | decal
BLEND opaque               # opaque | masked <cutoff> | translucent | additive
CULL back                  # back | front | none
FLAGS twosided=0 castshadow=1

PARAM baseColorFactor 1.0 1.0 1.0 1.0
PARAM metallicFactor 1.0
PARAM roughnessFactor 0.45
PARAM emissiveFactor 0.0 0.0 0.0
PARAM normalScale 1.0
PARAM occlusionStrength 1.0

TEX baseColor   {guid:0x…}  uv0 sRGB
TEX metalRough  {guid:0x…}  uv0 linear     # B=metallic, G=roughness (glTF MR)
TEX normal      {guid:0x…}  uv0 normal
TEX emissive    {guid:0x…}  uv0 sRGB
TEX occlusion   {guid:0x…}  uv0 linear

# Optional custom node graph (only for SHADER custom)
GRAPH{
  NODE 0 TexSample baseColor
  NODE 1 Multiply in0=0 in1=param:baseColorFactor
  OUT BaseColor 1
}
```

Rules: `TEX slot {guid:…|path:…} uvN colorspace` binds an `.octex` by GUID (path fallback). `PARAM` scalars/vectors are shading defaults; a material **instance** (`.ocmat` with `PARENT {guid}`) overrides only listed params (Unreal-MID-equivalent, pay-for-what-you-use). The optional `GRAPH{}` block is a small node list (evaluated by the material compiler to HLSL/SPIR-V permutations); most materials never need it. Cooked: the compiler resolves the graph to a shader-permutation key + a packed parameter block stored in a `MATL` chunk inside `.ocpak`. This deliberately avoids the UE editor-material-recompile crash (recon `arch §6`): compilation is offline, runtime just binds params + a precompiled PSO.

---

## 8. `.ocskel` — skeletal mesh + skeleton

Subtype `'SKEL'`. **Reuses the `.ocmesh` chunks** (`MHDR/VTXS/IDXS/MLET/MADR`) with `HasSkin` set (JOINTS/WEIGHTS streams present in the attribute buffers), and adds `SKEL` (bones) + `SKIN` (bind metadata). This is the reuse win: one geometry codepath.

### 8.1 `SKEL` — skeleton
| Off | Size | Type | Field |
|---|---|---|---|
| 0x00 | 4 | u32 | `BoneCount` |
| 0x04 | 4 | u32 | `RootBone` (index, or 0xFFFFFFFF) |
| 0x08 | … | BoneDesc[BoneCount] | bone table |

`BoneDesc` (per bone): `StringRef Name`, `i32 Parent` (−1 root), `vec3f LocalTranslation`, `quat LocalRotation`, `vec3f LocalScale`, `f32[16] InverseBind` (row-major, engine space). Bones named `node_<id>` map 1:1 to `.ocbeam` NODE ids (recon `arch §2`) — this is how a skeletal body binds to its soft-body cage.

Fidelity fixes over the OCCompiler rig extraction (recon `assets §9`): **multi-skin support** (store `u8 SkinIndex` per skin group instead of "skin[0] only"), and skeleton stored in **engine space** (import bakes the glTF-space→engine orientation rather than deferring to a runtime `GltfToCage`).

### 8.2 `SKIN` — skin binding
| Off | Size | Type | Field |
|---|---|---|---|
| 0x00 | 1 | u8 | `InfluencesPerVertex` | 4 or 8 (fixes the fixed-4 limit) |
| 0x01 | 3 | pad | |
| 0x04 | 4 | u32 | `SkinnedVertexCount` (== LOD0 VertexCount) |
| 0x08 | … | — | (bind positions optional; runtime uses SKEL inverse-binds) |

JOINTS/WEIGHTS live in the vertex attribute buffers (`R8G8B8A8_UINT`/`R16G16B16A16_UINT` + `R8G8B8A8_UNORM` normalized). The **canonical vertex-order contract** that OCCompiler relied on implicitly is now explicit: SKIN indices are 1:1 with `VTXS` vertex order in this same file — no external UE `OcGltf` ordering dependency.

---

## 9. `.ocanim` — animation clip

Subtype `'ANIM'`. Fixes the three fidelity losses from recon `assets §7/§9`: **no forced 30 fps resample**, **cubicspline tangents preserved**, **step curves preserved**. Chunks: `AHDR` (required), `TRKS` (track data), `NOTF` (notifies, optional), `STRT`.

### 9.1 `AHDR`
| Off | Size | Type | Field |
|---|---|---|---|
| 0x00 | 4 | f32 | `Duration` (seconds) |
| 0x04 | 4 | u32 | `TrackCount` |
| 0x08 | 1 | u8 | `Storage` | 0=keyframed (full fidelity), 1=baked-uniform (resampled, runtime-cheap) |
| 0x09 | 1 | u8 | `Flags` | bit0 `Loop`, bit1 `AdditiveBase`, bit2 `RootMotion` |
| 0x0A | 2 | u16 | `SampleRate` | for `Storage=1` (e.g. 30/60), else 0 |
| 0x0C | 4 | StringRef | `SkeletonRef` | target skeleton name/guid hint |
| 0x10 | … | TrackDesc[TrackCount] | 24 B each |

### 9.2 `TrackDesc` (24 B)
`u16 BoneIndex`, `u8 ChannelMask` (bit0 T, bit1 R, bit2 S — matches `.ocbeam` ANIM mask), `u8 Interp` (0 LINEAR, 1 STEP, 2 CUBICSPLINE), `u64 KeyDataOffset` (into `TRKS`), `u32 KeyCount`, `u32 Reserved`.

### 9.3 `NOTF` — animation notifies (optional)
`u32 Count`, then per notify: `f32 Time` (seconds from the clip start), `u32 StringRef Name`.

A **named event the clip raises when playback crosses `Time`** — a footstep on the frame the foot
lands, a muzzle flash on the frame the weapon fires. The name is opaque to this format and is fired
verbatim as a graph event (`GraphHost.Fire`), so a project invents its own vocabulary with no engine
change; see `docs/AVER_NODE_NODES.md` §`CustomEvent`.

**The chunk is optional and its absence is not an error.** A clip with no notifies emits no `NOTF`
at all, so a file written by an engine that predates them is byte-identical to one written now, and
a reader that predates them ignores a chunk it never asks for. Notifies are stored **in file order,
not sorted** — a reader needing time order can sort a handful of entries far more cheaply than every
writer can be trusted to have done so. `Time` outside `[0, Duration]` is permitted on write and
**clamped on read**, so shortening a clip moves a trailing marker to the end rather than losing it.

**There is no notify STATE (a begin/end range), deliberately.** Tracking which states are open,
closing them when a clip is interrupted, and deciding what a loop does mid-state is machinery this
runtime does not have; a half-built version whose symptom is a hit window that never shuts is worse
than not having it. Two instant notifies express the same intent and say out loud that nothing is
tracking the span between them.

### 9.4 `TRKS` key layout
Per track, for each present channel, a sub-array. **Keyframed** (`Storage=0`): each key = `f32 Time` + value(s): T/S = 3×f32, R = 4×f32 quat; for `CUBICSPLINE`, each key = `Time` + `inTangent` + `value` + `outTangent` (glTF cubic spec preserved). **Baked-uniform** (`Storage=1`): no per-key time; `KeyCount = round(Duration*SampleRate)+1` samples at `frame/SampleRate`, values only (this is the runtime-fast form, equivalent to the current `.ocbeam` ANIM but with chosen rate). Quaternions renormalized on read; slerp for LINEAR, hold for STEP, Hermite for CUBICSPLINE. A cooker can emit both: keep the keyframed source, bake a uniform variant into `.ocpak` for shipping.

---

## 10. `.ocprefab` — prefab (text)

First line `OCPREFAB 1`. A reusable node tree of transforms + components + asset references + property overrides. This is what bundles a *vehicle* (cage + aero + body meshes + materials) or a *track prop* into one authorable unit.

```
OCPREFAB 1
NAME Ferrari499P
NODE root  T 0 0 0  R 0 0 0 1  S 1 1 1
  COMPONENT VehicleCage  cage={ocbeam:Ferrari499P.ocbeam}  aero={ocaero:Ferrari499P.ocaero}
  COMPONENT MeshRenderer mesh={guid:0x…} materials=[{guid:0x…},{guid:0x…}]
  NODE frontWing  parent=root  T 120 0 40  R 0 0 0 1  S 1 1 1
    COMPONENT MeshRenderer mesh={guid:0x…} materials=[{guid:0x…}]
    COMPONENT AeroLink part=RearWing
OVERRIDE root.MeshRenderer.materials[0].baseColorFactor 0.8 0.0 0.0 1.0
```

Rules: nodes form a tree via `parent=`; components are named + typed with key=value props; asset refs by `{kind:pathOrGuid}`; `OVERRIDE <node>.<component>.<prop>` patches. A prefab may reference other prefabs (`COMPONENT PrefabInstance prefab={guid:…}` + overrides) → nested prefabs. References the legacy `.ocbeam`/`.ocaero` directly, so vehicle content composes cleanly. Cooked → `PREF` chunk (flattened node/component/override tables) in `.ocpak`.

---

## 10a. `.ocgraph` — visual scripting graph (Aver Node, text)

First line `OCGRAPH 1`. The one format in this family with **two independent readers**: a C++ one
(`modules/formats/src/OcGraph.cpp`, used by the node editor to load/save) and a C# one
(`scripting/csharp/Aver.Graph/OcGraphParser.cs`, used by everything that actually *runs* a graph —
`GraphHost`, `HostBridge`). They agree on the load-bearing records but not on every corner of their
grammar, and §10a.3 below is about exactly where they part ways. For what a graph's nodes and
records mean at runtime — the class model, `PARAM` vs `VAR`, entry-point ordering — see
[`VISUAL_SCRIPTING.md`](../VISUAL_SCRIPTING.md); for every node type's pin shape, see
[`AVER_NODE_NODES.md`](../AVER_NODE_NODES.md). This section documents the **file grammar** only.

```
OCGRAPH 1
# A minimal spawnable actor class: orbits the point it started at.
NAME AN_Orbiter
DESCRIPTION Orbits the point it started at.

CLASS AN_Orbiter Actor mesh=Meshes/sphere.ocmesh

PARAM entity int
PARAM time float

VAR homeX float 0

NODE tick OnTick
ENTRY tick OnTick

NODE t Param param=time
NODE speed ConstFloat value=1.0
NODE phase Multiply
LINK t.value phase.a
LINK speed.value phase.b

NODE homeVal GetVar var=homeX
NODE result Add
LINK homeVal.value result.a
LINK phase.result result.b

NODE write SetVar var=homeX
PIN write exec in exec
PIN write value in float
PIN write then out exec
LINK tick.exec write.exec
LINK result.result write.value

OUT result result
```

### 10a.1 Records

| Record | Grammar | Notes |
|---|---|---|
| `OCGRAPH` | `OCGRAPH <version>` | Header, must be the first non-comment line. `version` defaults to `1` if omitted. |
| `NAME` | `NAME <name>` | Single token, no spaces. Empty/absent writes back as `untitled`. |
| `DESCRIPTION` | `DESCRIPTION <rest of line>` | Free text, taken from the **raw** line, not the comment-truncated one — a literal `#` inside a description is data, not a comment marker (both readers agree on this; it was a real bug in the C++ reader, fixed once both sides used the same rule). |
| `CLASS` | `CLASS <name> [parentName] [mesh=<path>] [material=<name>] [view=firstperson\|thirdperson]` | Optional; **at most one per file** (a second `CLASS` line is a parse error). Declares the graph itself a spawnable actor class. `parentName` defaults to `"Actor"` when omitted. `mesh=`/`material=` become the class's own default `CMeshRenderer`, not a per-tick write. `view=` (for `Character`-parented classes only) sets `CameraViewMode` on the spawned instance; omitted or meaningless when the class's native ancestor is not `AverCharacter`. |
| `COMP` | `COMP <id> <Kind> [parent=<id>] [pos=x,y,z] [rot=yaw,pitch,roll] [scale=x,y,z] [key=value ...]` | One entry in a class graph's **component tree** — one child entity of a spawned instance. `Kind` ∈ `Scene`\|`Mesh`\|`Light`\|`Camera`\|`SkeletalMesh`\|`Particles`. `parent=` names another `COMP` (absent = attached to the actor's own entity) and **may name one declared later in the file**; the C++ reader refuses a parent that names nothing, a duplicate id, and a parent *cycle*, which is the one structural rule a tree needs and a single left-to-right pass cannot check. Transform is centimetres, degrees in the engine’s own `Rot` order — **yaw, pitch, roll**, matching `ActorBuilder.Place` rather than inventing a second convention, and a scale multiplier defaulting to `1,1,1` — an axis that does not parse keeps its default rather than becoming zero, so a half-typed `scale=2,,2` cannot flatten an actor. Every other `key=value` is **kind-specific** and opaque to the format: `mesh=`/`material=` on a `Mesh`, `fov=`/`near=`/`far=` on a `Camera`, `effect=` on `Particles`. Asset paths are **content-relative** (`Meshes/Blaster.ocmesh`, not `Content/Meshes/…`) — the id is a hash of the string, so a path with the content root on the front resolves to nothing and the component draws nothing, silently. `CLight` and `CCamera` attach correctly but **no renderer reads them yet**, and the spawn path says so in the log rather than letting an author conclude their transform is wrong. This is what `mesh=` on the `CLASS` line was a single-slot stand-in for; the two coexist, and `CLASS mesh=` still applies to the actor's own entity. |
| `COMMENT` | `COMMENT <id> <x> <y> <w> <h> <r> <g> <b> <text...>` | One **comment box**: a titled, tinted rectangle drawn behind the nodes, grouping them and saying why they are wired the way they are. Purely editor furniture — nothing compiles it, nothing executes it, and a graph stripped of every `COMMENT` runs identically. It lives in the format anyway because a note kept in a sidecar file is a note that goes stale the first time the graph is copied or renamed by someone who does not know the sidecar exists. `x`/`y`/`w`/`h` are canvas units (logical pixels at zoom 1, the same units `NODE`'s `x y` use); `r`/`g`/`b` are `0`…`255` and are clamped, not wrapped, by the editor's setter. **All eight numbers are required**, so the title always begins at token nine — reading them optionally the way `NODE` reads its `x y` would make `COMMENT c1 Spawning logic` ambiguous between a box with no position and a box at `x=Spawning`. The title is the **rest of the raw line**, exactly as `DESCRIPTION` is: a literal `#` inside it is data (`counts the # of spawners`), not a comment marker. That closes the record on the right — there is no room after free text for a future `key=value`, which is why the colour is spelled out **now** rather than left for later; anything this record grows from here has to arrive as a sibling record. Ids are unique among comments only, and unrelated to node ids. Editor gestures: right-click ▸ *Comment Box*, or **C** to wrap the current selection; drag the **title bar** to move it (every node fully inside comes with it), drag the bottom-right grip to resize, double-click the bar for title/colour/delete. |
| `FUNC` | `FUNC <name> [pure]` | Declares one **user-defined function**: a named, callable subgraph living in the same file as the event graph. Compiled to its **own method**, reached by a direct call — so it can **recurse**, which is the one thing an inlined macro can never do. `pure` means no exec pins anywhere (entry, return, or call site) and callable from a data wire; without it the function is impure and has exec pins on all three. Purity is **declared, not inferred**, because a function whose body is only a call to another function is pure or impure *transitively*, and no rule that inspects the body's node types can see that — `Graph.Validate()` checks the declaration against the body, transitively, and refuses a lie. Duplicate names are refused. |
| `FUNCIN` | `FUNCIN <func> <pin> <type>` | One argument of `<func>`, in declaration order — the order is the emitted method's argument order, so reordering these reorders the call. `type` ∈ `float`\|`int`\|`bool`; `exec` is refused (a function's control flow is the `pure` flag, in one place, not an argument that could disagree with it). Must appear **after** its own `FUNC`: nothing does a second pass, and silently dropping an argument would give the function the wrong arity with no report of why. |
| `FUNCOUT` | `FUNCOUT <func> <pin> <type>` | One return of `<func>`, in declaration order. Same type rules as `FUNCIN`. A separate record rather than a direction token on one shared record, because inputs and outputs are independent lists and a single interleaved record would make the file say which came first when nothing depends on it. An input and an output **may share a name** (`x` in, `x` out is ordinary). Zero outputs compiles to `void`, one to that type, several to a boxed `object[]` — the same shape `Compile()` already uses for several `OUT` records. |
| `PARAM` | `PARAM <name> <type>` | Declares one argument the compiled graph accepts, in declaration order. `type` ∈ `float`\|`int`\|`bool` — `exec` is rejected at parse time with an explicit error (a parameter is data, not control flow). |
| `VAR` | `VAR <name> <type> [default]` | Declares one variable the graph remembers between ticks (see [`VISUAL_SCRIPTING.md` §4](../VISUAL_SCRIPTING.md) for the storage/lifetime contract). Same three types as `PARAM`, same `exec` rejection. An unparseable `[default]` falls back to the type's zero value rather than failing the graph. |
| `ENTRY` | `ENTRY <nodeId> <eventName>` | Declares which node begins the exec chain for a named event (`OnStart`, `OnTick`, or any project-invented name). The node may be of any type — `ENTRY` is what makes it a starting point, not the node's own type. |
| `NODE` | `NODE <id> <type> [x y] [key=value ...]` | One node. Two attributes concern **functions**: `func=<name>` says which subgraph this node lives in (absent = the event graph), and `call=<name>` says which function a `CallFunc` node calls. They are separate attributes because a call node has both — it *lives* in one subgraph and *calls* another, and overloading one attribute would make a recursive call indistinguishable from a node that merely sits inside the function it calls. A wire may not cross between subgraphs (`Graph.Validate()` refuses it): they compile to separate methods, so a link between them would join two method bodies' locals. Arguments cross through `FuncEntry`, results through `FuncReturn`, and nothing else crosses. `x`/`y` (editor canvas position) are optional and read only if the next token actually parses as a number — an unnumbered `key=value` token is never mistaken for a coordinate. Recognised `key=value` attributes: `value=` (constant literal), `param=`, `field=`, `class=`, `var=`, `name=`, `mesh=`, `material=` — one node type reads each, per [`AVER_NODE_NODES.md`](../AVER_NODE_NODES.md). An unrecognised `key=value` is ignored, not fatal. |
| `PIN` | `PIN <nodeId> <name> <in\|out> <type> [default]` | An explicit pin declaration. **The moment a node has even one explicit `PIN` line, its entire built-in default pin set is skipped for it** — not merged, replaced — see `AVER_NODE_NODES.md`'s "three traps" for why this matters in practice. |
| `LINK` | `LINK <srcNode>.<srcPin> <destNode>.<destPin>` | Connects an output pin to an input pin. A `LINK` between two `exec`-typed pins *is* a control-flow wire — `exec` is an ordinary pin type, not a second kind of record. |
| `OUT` | `OUT <nodeId> <pinName>` | Declares one value the graph hands back when pulled as pure dataflow. Order matters — it is the return order of the compiled method. |

### 10a.2 What the C++ reader owns, and the hazard that follows from it

`OcGraph.cpp`'s `classifyLine` recognises fourteen record kinds as its own:
**`OCGRAPH`, `NAME`, `DESCRIPTION`, `VAR`, `COMP`, `COMMENT`, `FUNC`, `FUNCIN`, `FUNCOUT`, `NODE`,
`PIN`, `LINK`, `ENTRY`, `OUT`.**
Only these have a field in `OcGraphData` (`modules/formats/include/aver/formats/OcGraph.hpp`) — the
struct that C++ code actually holds in memory. The list has grown three times, and each time for the
same reason: a record the C++ side can only copy verbatim is a record the **editor cannot edit**, so
`VAR` moved out of `Other` when the Variables panel needed to declare one, `COMP` when the component
tree needed to build one, `COMMENT` when comment boxes needed to be moved and resized, and the three
`FUNC*` records when the Functions panel needed to declare a function's signature. A function's BODY
needed nothing: its nodes are ordinary `NODE`/`PIN`/`LINK` records carrying a `func=` attribute, which
rides in the same `extraTokens` that already carries `param=`/`field=`/`class=`/`var=`.

**`CLASS` and `PARAM` are still invisible to the C++ side.** They are not malformed input and not
rejected; they simply fall into the same catch-all `Other` bucket as a comment or a blank line, and
`OcGraphData` has no field that could hold a class name or a parameter list even if the reader wanted
to keep one. (`PINVAL <nodeId> <pinName> <value>` — a fallback constant for an unconnected input pin
— rides through the same way; it is C#-only too, and not part of this section's closed list because
nothing in this engine's toolchain writes one from a level or a class declaration.)

This is why `writeOcgraph`'s `existing` parameter is load-bearing, not a convenience:

- Called with the file's original text as `existing` (`writeOcgraph(g, existing)`), every `CLASS`/
  `PARAM` line in that original text is copied through **verbatim, at its original position**
  — the same whole-file "preserve what you don't understand" contract that protects a stray comment.
  `saveOcgraph()` (`OcGraph.cpp`) reads whatever is already on disk at the target path before writing
  for exactly this reason — and correspondingly, `saveOcgraph()` at a path that does not exist yet
  (a brand-new file) starts from empty `existing`, has nothing to preserve, and needs nothing to
  preserve for the same reason.
- Called with **no** original text — `writeOcgraph(g)` with its default empty `existing`, or any
  caller that builds an `OcGraphData` from scratch (in memory, never parsed from the file it is about
  to overwrite) and hands it straight to `writeOcgraph` — every `CLASS`/`PARAM` line that used
  to be in that file **is gone from the output**, silently. There is no error, because from the C++
  side's point of view nothing was lost: those records were never modelled as data to begin with, so
  there is nothing to notice missing.

**Where a reader will actually meet this:** the C++ node editor (`sandbox/src/GraphEditor.cpp`)
protects itself correctly — it always calls `writeOcgraph(graph_, originalText_)` with the text it
loaded, per its own file-header comment, so opening and saving an *existing* graph that declares a
class or a parameter is safe. But the same fact cuts the other way: because `OcGraphData` has no field
for either, **the C++ editor has no way to create or edit a `CLASS` or `PARAM` record at all** — only
to silently carry one through if the file already had it. Declaring a graph as a class, or changing
its parameter list, is a text edit today (or done from the C# side, which does model both) — not
something the node editor's GUI can do, node-attribute panel included. `VAR` used to be in that
sentence and no longer is: it is modelled now, and the Variables panel declares, renames, retypes and
deletes one. Any future C++ tool that constructs a fresh `.ocgraph` programmatically and calls
`writeOcgraph` without first loading the file it is replacing will reproduce the drop described above
for real.

### 10a.3 Where the two readers disagree

Both readers accept the same *common* subset of well-formed files, but they are not one grammar
implemented twice — a file that only one side would accept is a file the other side's tooling cannot
safely round-trip. Known divergences, in the order a hand-editor is likely to hit them:

- **`OCGRAPH` version enforcement.** The C++ reader accepts any integer after `OCGRAPH` with no
  check at all (`out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;`). The C# reader — the one that
  actually compiles and runs a graph — rejects anything but `1` outright (`"Unsupported OCGRAPH
  version {version}"`). A file the C++ editor opens and saves without complaint can still fail to
  load at runtime if its header says anything other than `OCGRAPH 1`.
- **Forward-reference ordering — FIXED, and worth reading as a lesson about this document.** Every
  record that names a node (`ENTRY`, `OUT`, `LINK`, `PIN`) may now name one declared later in the
  file, in both readers. The C++ reader was a single left-to-right pass that checked each reference
  against the nodes parsed *so far*; the C# reader deferred the same check until the whole file was
  read. The divergence was never theoretical — it meant the node editor could not open
  `test-content/GraphDemo/Content/Scripts/IdleMotion.ocgraph`, this repository's own worked example,
  because it writes `ENTRY seed OnStart` above `NODE seed OnStart`, which is how every real graph
  here is written.

  It was fixed in two goes, and the gap between them is the point. `ENTRY` and `OUT` were deferred
  first; `LINK` and `PIN` kept their inline checks, so a graph that branches and *rejoins* — writing
  `LINK a.then merge.exec` above the section declaring `NODE merge` — was still refused. That is the
  FirstPerson template's own character graph, which the C# runtime parses fine, so the template RAN
  and only the editor could not open its largest file. It shipped in 0.3.0 that way and was found by
  opening it.

  **Writing a bug down here is not the same as deciding it.** This entry described the ordering gap
  accurately for months, as a known divergence rather than a defect, and being documented is exactly
  what let it survive being obviously wrong. A row in this table that says two readers disagree is a
  bug report, not a specification.
- **Duplicate node IDs.** A second `NODE` line reusing an already-seen `id` is a parse error in the
  C++ reader. The C# reader has no such check — it silently overwrites the dictionary entry, and any
  `PIN` line that arrived between the two `NODE` lines stays attached to the now-orphaned first node
  object.
- **`LINK`'s grammar.** The C++ reader accepts only the dot form, `LINK src.pin dest.pin`, and treats
  a missing `.` as a parse error. The C# reader additionally accepts a four-token space-separated
  form, `LINK srcNode srcPin destNode destPin` — a file authored in that form parses in C# and fails
  to parse in C++.
- **`PIN`'s direction token.** The C++ reader accepts only `in`/`out` (case-insensitive) and rejects
  anything else. The C# reader also accepts the long forms `input`/`output`, and does not reject an
  unrecognised token at all — it is silently read as `in`. Write `in`/`out` for a file both sides
  agree on.

None of this is a defect in either reader considered alone — each is internally consistent and does
what its own consumer needs. It is a property of having two independent implementations of one text
grammar with no shared parser, the same shape of risk the legacy `.ocmap`/`.ocbeam` family already
carries (§4), and worth knowing before hand-editing a graph rather than after a save silently narrows
what it says.

---

## 11. `.ocworld` — native scene/world (superset of `.ocmap`)

First line `OCWORLD 1`. **Every `.ocmap` record is a legal `.ocworld` record** (identity/env/placement lines parse identically), so an `.ocmap` is a valid — if minimal — `.ocworld`, and the round-trip in §4.4 holds. On top of `.ocmap`, `.ocworld` adds a scene graph, streaming, lighting/environment, terrain/georef, and prefab instancing.

```
OCWORLD 1
# --- identity block: byte-compatible with .ocmap ---
ID 0x376B85BC4D1A03BA          # FNV-1a-64(NAME), unchanged algorithm
NAME demoworld
BUILD 1
ALGO 3
ROOT <blake3-256 merkle over placements + asset content>   # real root now (reader tolerant)
CLIENT scene                   # or: CLIENT umap <path> (legacy passthrough)

# --- env block: .ocmap records verbatim ---
SURFACE 0 tarmac 1.00 0.015 0.30
SURFACE 1 kerb   0.92 0.020 0.40
SURFACE 2 grass  0.45 0.090 0.35
GROUND 0.0 0
KILLZ -5000.0
SPAWN 0 0 100 0

# --- .ocmap placements still valid ---
DEFORM tyre_barrier.ocbeam 12000.0 800.0 0.0 90.0 0.0 0.0 rubber
PLACE  kerb_4m 1200.0 400.0 0.0 0.0 0.0 0.0 1.000

# --- NEW: class-instance placement -- spawns a registered actor class instead of a mesh ---
PLACE  none 5000.0 -800.0 0.0 0.0 0.0 0.0 1.000 class AN_Orbiter

# --- NEW: scene graph, non-uniform scale, prefab instances, GUID refs ---
LAYER static
NODE grandstand parent=world  T 5000 -800 0  R 0 0 0 1  S 1 1 1
  PLACEG {guid:0x…} scale3=1 1 1            # GUID placement, non-uniform scale
  PREFAB {ocprefab:barrier_stack.ocprefab}  T 0 0 0

# --- NEW: streaming cells (anti-bloat / pay-for-what-you-use) ---
CELL grid 25600 25600           # cell size cm; instances auto-bucketed by AABB
STREAM cell_12_7 aabb=… assets=[{guid:…},…]

# --- NEW: lighting / environment ---
ENV sky {octex-cube guid:0x…} intensity 1.0
SUN dir -0.5481 0.3838 0.7431 color 1.0 0.98 0.92 lux 100000   # dir points TOWARD the sun (+Z up)
FOG exp density 0.0002 color 0.7 0.8 0.9

# --- NEW: terrain / georef (Cesium-style dormant hook) ---
GEOREF wgs84 lat 45.6156 lon 9.2811 alt 162.0    # origin for real-world tracks (Monza)
TERRAIN heightfield {guid:0x…} extent 800000 800000
```

Notes: `PLACE` (asset-name + transform, `.ocmap` style) and `PLACEG` (GUID + non-uniform scale) coexist; `DEFORM` unchanged (soft-body barriers). `LAYER`/`NODE`/`CELL`/`STREAM` give hierarchical + streamable worlds without the monolithic-level bloat. `GEOREF`/`TERRAIN` are the georeferenced-world hook the recon flagged as the likely dormant-Cesium role (`arch §6/§8`) — specified but engine-optional. Cooked `.ocworld` → `WRLD` chunk (flattened instance/cell/light tables) + a BLAKE3 Merkle `ROOT` computed over placements and referenced asset content hashes (the recon-intended real ROOT), and a `mapContentId` for the net map-parity gate (`net §4`).

**The `class` keyword-argument** on `PLACE`/`PLACEG` (`modules/formats/src/OcWorld.cpp`: parsed at
the trailing-token loop, written only when non-empty, same "no override is the default" rule
`GAMEMODE` follows) turns a placement into a **class instance** instead of an ordinary mesh: `class
<ClassName>` names any class already registered through the framework's class registry — a C#
`[AverClass(...)]` type (`docs/SCRIPTING_API.md` §11) or a graph that declared itself one with a
top-level `CLASS` record (`.ocgraph`, §10a above) — the placement format does not, and cannot, tell
which. When `class` is present, the leading asset column is **never read**: no mesh/physics entity is
built for that placement at all (this is the one case where a `PLACE` line's own asset name is
inert), and what the instance looks like is entirely up to the named class's own defaults and its own
`OnStart`/`OnTick`. See [`VISUAL_SCRIPTING.md` §6](../VISUAL_SCRIPTING.md) for the full load-time
pipeline (parse → skip the raw entity → collect → spawn once scripting is ready) and for a real,
running two-instance example.

---

## 11a. `.ocparticle` — particle effect (text)

Text, for the reason §1 gives: an effect is small authoring data an artist tunes by hand and diffs in
review, not bulk data a GPU maps. One `key value` record per line, `#` comments and blank lines
ignored, unknown records preserved verbatim through a load/save cycle (the `.ocgraph` rule, §10a
above — a file written by a newer tool must survive being opened and saved by an older one).

```
OCPARTICLE 1
NAME Snowfall
SHAPE box 400 400 15          # point | box x y z | sphere r
BLEND alphablend              # opaque | alphablend | premultiplied | additive
EMISSION 140 0 700            # rate/sec, one-shot burst count, pool cap
LIFETIME 7 10                 # seconds, min max -- drawn per particle at spawn
DIRECTION 0 0 -1 10           # cone axis, then half-angle in degrees
SPEED 25 40                   # cm/s, min max
GRAVITY 0 0 -3                # cm/s^2, a constant -- NOT a physics query
DAMPING 0.4                   # fraction of velocity removed per second, [0,1)
SIZE 3 4                      # cm, birth then death
COLOR start 1 1 1 0.85        # straight (non-premultiplied) rgba
COLOR end   1 1 1 0.6
TEX {guid:0x…}                # optional; untextured is a soft round sprite
GI on                         # off for self-lit effects -- see below
```

`GI` is the one field whose default is worth stating outright. With it on, the particle is modulated
by the scene's indirect light, so smoke and dust sit in the world instead of reading as pasted on.
Self-lit effects — sparks, embers, anything additive — should set `GI off`: a spark is its own light
source, and dimming it by the ambient around it is simply wrong.

**Ranges are enforced at parse time, not clamped.** `DAMPING` outside `[0,1)`, a colour component
outside `[0,1]`, an inverted `LIFETIME`, or a spread outside `[0,180]` are all refused with an error
naming the record. This is not pedantry: the simulator trusts these, and a negative `DAMPING` is a
velocity multiplier greater than one, so a mistyped sign does not damp gently — it accelerates every
particle until the effect fills the screen.

A parse failure leaves the caller's effect at this format's own defaults, never at the partial state
of a file that failed halfway.

---

## 12. Versioning & compatibility

- **Two version axes:** `ContainerVersion` (the `AVR1` framework) and `ContentVersion` (per-subtype schema). Text formats carry a single `OC<NAME> <n>` line.
- **`MinReaderVersion`** lets a writer mark a file that older readers must refuse rather than misread.
- **Forward compat:** unknown chunks skipped unless `Required`; unknown text sections skipped (OC-family behavior). New optional fields append to a chunk struct; readers honor `SizeUncompressed` and zero-fill unknown tails. New enum values must have a defined fallback (e.g. unknown vertex format ⇒ refuse that stream; unknown texture codec ⇒ if `SRC ` present, re-transcode, else refuse).
- **Backward compat:** writers never remove or reorder existing fields; deprecations set a `Deprecated` chunk flag and keep the field. A `MIGR` note chunk may record the source version chain.
- **Deterministic writes:** stable field order, sorted chunk directory, zeroed padding ⇒ identical bytes for identical input (clean diffs, cache hits).

---

## 13. Cooked / DDC / packaging

- **Source vs cooked.** Source = text (`.ocmat/.ocprefab/.ocworld` + legacy `.oc*`) and importer-neutral binaries. Cooked = platform-specialized, compressed, GPU-aligned, source stripped. Cooked artifacts live in `.ocpak` (subtype `'PAK '`), a container whose chunks are complete embedded assets (each with its own header) plus a `TOC ` chunk (`GUID → {chunkIndex, offset, size, subtype, dependencies[]}`).
- **DDC key.** `BLAKE3( sourceContentHash ‖ importerVersion ‖ targetPlatform ‖ cookSettings )`. A hit reuses the cooked blob. This is the offline analogue of UE's import-compile step (recon `arch §4`): `.ocbeam`/`.ocaero` compile once to a `UVehicleCageAsset`-equivalent; the Aver equivalent is a cooked cage chunk keyed the same way.
- **`.ocmeta` sidecars.** Each source asset gets a text `.ocmeta` (JSON-ish) holding its `GUID`, import settings, and source-file hash — so GUIDs are stable across moves and re-imports, and the cooker knows when to re-cook.
- **Platform variants.** Cook targets pick texture codecs (BC7/BC6H desktop vs ASTC mobile vs Basis universal), index width, position quantization, and whether to keep meshlets. Deformable vehicle-body meshes are exempt from position quantization (bit8 `Deformable`).
- **Streaming.** Mip data (`MIPS`) and per-LOD mesh blocks are individually addressable + individually compressed so higher mips / finer LODs stream on demand; `.ocworld` `CELL`/`STREAM` records drive spatial streaming.

---

## 14. Migration path

### 14.1 Legacy load-direct (zero migration)
`.ocbeam`, `.ocaero`, `.ocmap`, `.scene`, `.octrack` load unchanged through the faithful parsers (§4). No conversion, no re-save. They remain the source of truth for cages, aero, and worlds. Cooking them (optional) produces cage/aero/world chunks in `.ocpak` but the text stays canonical.

### 14.2 Extract the embedded GLB (closes the biggest gap)
Today geometry/PBR/textures survive only as base64 `.glb` bytes inside `.ocbeam` (recon `assets §0/§9`). The Aver importer:
1. Runs the `.ocbeam` parser; on `GLB{}`, base64-decodes to raw `.glb`.
2. Applies the recorded `GLBXFORM` (`yup/forward/mirror`) + `SCALE`/bounds-fit exactly as the UE factory did (`(x,-z,y)`, forward rotation, Y-mirror, per-axis AABB fit) so geometry lands in engine cm space with winding fixed.
3. Emits one **`.ocmesh`** per glTF part (submeshes = parts, names preserved to match `PART`/`FVehicleCagePartMesh`), **`.octex`** per image (PNG/JPEG via stb_image → BC7/BC5), **`.ocmat`** per glTF material (metallic-roughness → §7), and **`.ocskel`**/**`.ocanim`** if the glb has a skin/animations (rig extracted like `GltfRig`, but multi-skin + full-fidelity anim per §8/§9).
4. Optionally writes a **`.ocprefab`** binding the new meshes/materials to the original `.ocbeam` cage + `.ocaero`, so the vehicle is one asset.

After this, the opaque embedded-glb handoff is eliminated; `.ocbeam` can shrink to just the cage (glb optional/legacy).

### 14.3 UE `.uasset`-derived content
Recon (`assets §8`) shows the OCCompiler `.uasset` reader extracts only **component lists** (names + FRACTURE/DEFORM kind), not geometry. So mesh geometry is **not** taken from `.uasset`; it comes from the original glTF/OBJ/FBX sources (or the embedded glb, §14.2) via the importer. The `.uasset` component list is used only to author the corresponding `.ocprefab` component set (FRACTURE→fracturable part, DEFORM→skeletal part). No dependence on Epic's package binary at runtime — the fragile `.uasset` parse stays an editor-time import convenience.

### 14.4 Toolchain (polyglot)
- **Rust asset pipeline** (`aver-cook`) does import + cook + DDC over the C ABI: glTF/OBJ/PNG/JPEG in → `.ocmesh/.octex/.ocmat/.ocskel/.ocanim` out → `.ocpak`. glTF/JSON parse and image decode use permissive crates (or FFI to the C core).
- **C++ engine core** provides the authoritative readers (`libaver_assets`) exposed via the C ABI (§15); the C# editor P/Invokes them; the Rust tools FFI them. One reader implementation, three languages — matching the engine's polyglot spine.
- **C# editor** authors `.ocmat/.ocprefab/.ocworld` (text) and triggers cooks.

---

## 15. C ABI loader surface (the interop backbone)

A stable `extern "C"` surface so C#/Rust bind identically (mirrors the existing `oc_sim`/`oc_match` ABI style, out-pointer returns, `*_abi_version()` guard):

```c
uint32_t     ocasset_abi_version(void);              /* == 1 */

/* open by bytes or path; returns opaque handle or NULL */
OcAsset*     ocasset_open_memory(const void* data, uint64_t size);
OcAsset*     ocasset_open_file(const char* utf8_path);
void         ocasset_close(OcAsset*);

/* container introspection */
uint32_t     ocasset_subtype(const OcAsset*);        /* FourCC */
uint16_t     ocasset_content_version(const OcAsset*);
void         ocasset_guid(const OcAsset*, uint8_t out_guid[16]);
uint32_t     ocasset_chunk_count(const OcAsset*);
int          ocasset_find_chunk(const OcAsset*, uint32_t fourcc, OcChunkView* out);
/* OcChunkView { const void* ptr; uint64_t size; uint16_t version; } — decompressed, engine-space */

/* legacy text formats (faithful parsers) — parse into POD out-structs */
int          ocbeam_parse(const char* text, uint64_t len, OcBeamData* out);
int          ocaero_parse(const char* text, uint64_t len, OcAeroData* out);
int          ocmap_parse (const char* text, uint64_t len, OcMapData*  out);

/* typed mesh accessor built on chunk views */
int          ocmesh_get_header(const OcAsset*, OcMeshHeader* out);
int          ocmesh_get_lod(const OcAsset*, uint32_t lod, OcMeshLod* out);
```

All returns are error codes (`0`=ok); all geometry/pixels handed back are already little-endian engine-space, decompressed, ready for GPU upload (respecting the 256-B alignment so the caller can copy straight to an upload heap). Struct returns use out-pointers (no cross-ABI struct-by-value).

---

## 16. Licensing summary (permissively-licensed posture)

| Concern | Choice | License |
|---|---|---|
| Hashing | xxHash3 / BLAKE3 / FNV-1a (compat) | BSD-2 / CC0-Apache / public-domain |
| Compression | zstd, LZ4 | BSD |
| Texture GPU codecs | BCn, ASTC | unencumbered DX standard / Khronos permissively-licensed |
| Texture supercompression | Basis Universal / KTX2 | Apache-2.0 |
| Image decode | stb_image | public domain |
| glTF/JSON parse | self-contained (OCCompiler-style) | own code |
| Mesh optimize / meshlets | meshoptimizer | MIT |
| Tangents | MikkTSpace-equivalent | public domain |

No GPL runtime, no Unreal-derived tech, no proprietary codecs. Every dependency is MIT/BSD/zlib/Apache-2.0/public-domain, satisfying the hard requirement.

---

---

## A. `.octemplate` — project template manifest (text)

First line `TEMPLATE`. Declares a reusable project template (asset bundle + starter content). Discovered by walking up from the executable to find a `templates/` directory containing valid subdirectories, each with a `<name>.octemplate` manifest. The manifest has these records:

| Record | Grammar | Notes |
|---|---|---|
| `TEMPLATE` | `TEMPLATE` | Header, must be first non-comment line. Required. |
| `NAME` | `NAME <name>` | Single token. Required; manifest parse fails if absent. |
| `DESCRIPTION` | `DESCRIPTION <rest of line>` | Optional; defaults to empty. |
| `PREVIEW` | `PREVIEW <relative-path>` | Optional image path (PNG/JPEG), resolved relative to template directory. Defaults to no image. |
| `STARTMAP` | `STARTMAP <path>` | Optional level path. Defaults to `Maps/Default.ocmap`. |

Example:
```
TEMPLATE
NAME BeautifulScene
DESCRIPTION A starter level with a sky and terrain
PREVIEW Thumbnail.png
STARTMAP Maps/MyStartLevel.ocmap
```

**Template instantiation:** When a new project is created from a template, only the template's `Content/` directory is copied byte-for-byte into the new project. The project's own `.ocproject` manifest is written fresh with the new project's name — there is no find-and-replace pass over copied content, so template design (class names, graph names, asset filenames) is preserved exactly. The `.octemplate` manifest and its PREVIEW image are never copied into the project (they exist only in the template's root directory, not under `Content/`).

---

### Deliverable file map (paths this design implies under `C:/Users/User/Documents/Aver Engine`)
- `docs/formats/` — this spec split per format (container, ocmesh, octex, ocmat, ocskel, ocanim, ocprefab, ocworld, legacy, .octemplate).
- `engine/assets/` — C++ readers/writers (`libaver_assets`) + C ABI header `include/ocasset.h`.
- `tools/aver-cook/` — Rust importer/cooker.
- `editor/` — C# authoring for text formats.
- `samples/` — round-trip the existing `Ferrari499P.ocbeam/.ocaero` and `demoworld.scene/.ocmap` to prove byte-fidelity load and lossless `.ocworld` upgrade.

This design preserves the four legacy formats byte-for-byte, gives the seven missing asset types real versioned formats under one skippable-chunk container, keeps authoring data human-readable while making bulk data GPU-ready and GPU-driven-capable, and provides a concrete import/cook/DDC path that finally lifts geometry/material/texture/skeleton/animation data out of the opaque embedded-glb and into first-class, permissively-licensed native formats.