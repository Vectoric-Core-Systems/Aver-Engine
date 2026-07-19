I have everything I need, including byte-level confirmation. Key findings verified: `fnv1a64("demoworld") = 0x376B85BC4D1A03BA` (matches sample), the Java `ROOT` formula `sha256("demoworld|build1|scene")` matches the sample byte-for-byte, and the sample has **mixed line endings** (`\r\n` on exactly the lines Java emits via `%n`: `ID`, `DEFORM`, `PLACE`; `\n` everywhere else).

Here is the specification.

---

# OpenConstructor `.scene` / `.ocmap` Format Specification

Source of truth:
- `OCMapConverter.java` (standalone `.scene` → `.ocmap` converter; **no UE dependencies**)
- `umap_to_ocmap.py` (UE-editor authoring exporter; UE-coupled; emits the *same* `.ocmap` grammar plus two extra records)
- `README.md`, sample `demoworld.scene`, sample `demoworld.ocmap`

Authoritative **reader** is `MapRegistry` / `OcMap.cs` (C# server) — referenced (`README.md:5,31`, `OCMapConverter.java:3`) but **not provided**. Reader tolerances (unknown-field handling, line-ending acceptance, field-count strictness) can only be inferred from writer output below; pin them against `OcMap.cs` when porting.

---

## 1. The object-reference world model

`README.md:3-8`, `umap_to_ocmap.py:6-16`. The `.ocmap` stores **only "locations + asset names"** — a list of placement rows (`PLACE`/`DEFORM` = asset name + transform) plus identity and an environment table. It does **not** bake geometry.

| Consumer | What it does with the `.ocmap` |
|---|---|
| **Server** (collision) | Rebuilds collision by instantiating each placement's asset collision proxy from the shared asset library; simulates `DEFORM` cages as deformable barriers. (`umap_to_ocmap.py:8-11`) |
| **Client** (visuals) | Opens the named `CLIENT umap` level, **or** (`CLIENT scene`) builds the world from the same shared assets. (`umap_to_ocmap.py:11-12`, `OCMapConverter.java:30`) |

Both sides key off the same asset names + transforms, so server collision and client visuals stay in lockstep without shipping geometry in the map file.

---

## 2. `.scene` (authoring input) grammar

Text, UTF-8 (`OCMapConverter.java:33`). Parsed line-by-line.

### 2.1 Line preprocessing (`OCMapConverter.java:95-100`, `strip()`)
1. Truncate at first `#` (rest of line is a comment).
2. `trim()` leading/trailing whitespace.
3. If the result ends with `;`, drop the `;` and `trim()` again (trailing semicolons optional/ignored).
4. Empty result → skip line (`:34`).

### 2.2 Tokenizing
Split on `\s+` (one or more whitespace) (`:36`). `t[0]` uppercased → directive key (`:37`). **Asset names and paths cannot contain spaces.**

### 2.3 Directives & rows

| Form | Condition | Effect | Ref |
|---|---|---|---|
| `MAP <name>` | `t.length ≥ 2` | `mapName = t[1].toLowerCase()` (overrides filename default) | `:38` |
| `CLIENT umap <path>` | `t.length ≥ 3` **and** `t[1]=="umap"` (case-insensitive) | `clientUmap = t[2]` | `:39` |
| `CLIENT …` (anything else, e.g. `CLIENT scene`) | otherwise | `clientUmap = ""` (→ emits `CLIENT scene`) | `:39` |
| **placement** `<asset> x y z yaw pitch roll [scale]` | line is not MAP/CLIENT and `t.length ≥ 4` | append a `Placement` | `:41-47` |
| (too few tokens) | `t.length < 4` | skip with stderr warning `skip (need asset + x y z)` | `:41` |

### 2.4 Placement row fields (`.scene`)

| Col | Field | Java | Type | Units / coords | Notes |
|---|---|---|---|---|---|
| 0 | `asset` | `t[0]` verbatim | string | — | case preserved; may contain `.` (e.g. `tyre_barrier.ocbeam`) |
| 1–3 | `x y z` | `d(t,1..3)` | double | cm, UE world (see §5) | |
| 4–6 | `yaw pitch roll` | `d(t,4..6)` | double | degrees (see §5) | **row order is yaw, pitch, roll** |
| 7 | `scale` | `t.length>7 ? d(t,7) : 1.0` | double | uniform scalar | defaults `1.0` if omitted |

`d()` (`:101`) parses a Java `double`; **any parse failure silently yields `0.0`** (no error).

### 2.5 Map-name default
If no `MAP` directive: `mapName` = input filename with the last `.ext` stripped, lowercased (`:29`).

### 2.6 PLACE vs DEFORM classification
A placement becomes a **`DEFORM`** row iff `asset.toLowerCase().endsWith(".ocbeam")`, else **`PLACE`** (`:21`, `:71`). This is decided at *write* time; the `.scene` has no separate DEFORM keyword.

### Sample `demoworld.scene`
```
MAP demoworld
CLIENT umap /Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack
tyre_barrier.ocbeam 12000 800 0 90 0 0 1.0     # → DEFORM (ends .ocbeam)
kerb_4m 1200 400 0 0 0 0 1.0                    # → PLACE
```

---

## 3. `.ocmap` (compiled world) grammar

Text, UTF-8. Line-oriented, whitespace-delimited. `#` lines are comments. **Records appear in a fixed order** (below). No binary, no endianness — but note the **line-ending caveat** in §6.

### 3.1 Header / identity records

Emit order (`OCMapConverter.java:54-61`; `umap_to_ocmap.py:118-128`):

| Line | Grammar | Meaning | Ref |
|---|---|---|---|
| **magic+version** | `OCMAP 1` | literal magic `OCMAP`, format version `1` (only version seen) | `java:54` |
| comment | `# …` | free text | `java:55` |
| **ID** | `ID 0x<16 UPPERCASE hex>` | ContentId = **FNV-1a-64 of the NAME string** (UTF-8 bytes) | `java:56`, `:103-107` |
| **NAME** | `NAME <mapname>` | map name (lowercased) | `java:57` |
| **BUILD** | `BUILD <int>` | content/build version; hardcoded `1` (Java) / `BUILD` var =1 (py) | `java:58`; `py:126` |
| **ALGO** | `ALGO <int>` | algorithm id; hardcoded `3` in both. **Meaning undocumented — ambiguous** | `java:59`; `py:125` |
| **ROOT** | `ROOT <64 lowercase hex>` | SHA-256 digest; **stand-in only** (see §3.1.1) | `java:60`, `:108-113` |
| **CLIENT** | `CLIENT scene` \| `CLIENT umap <UEpath>` | client visual source | `java:61` |
| separator | `#` | | `java:62` |

#### 3.1.1 ID (FNV-1a-64) — exact algorithm (`java:103-107`)
```
h = 0xcbf29ce484222325           // 64-bit offset basis
for each byte b of NAME (UTF-8): // b taken as unsigned: (b & 0xFF)
    h ^= b
    h  = h * 0x100000001b3        // 64-bit wraparound (Java long overflow / mask 0xFFFFFFFFFFFFFFFF in py)
```
Printed as `0x%016X` (uppercase, zero-padded to 16). Verified: `NAME demoworld` → `0x376B85BC4D1A03BA` (matches sample). Python mirror: `py:35-39`.

#### 3.1.2 ROOT (SHA-256) — **placeholder, diverges between tools**
- **Java:** `sha256hex(mapName + "|build1|scene")` — note `build1` and `scene` are **literals**, *not* the BUILD/CLIENT values (`java:51`). Verified: `demoworld` → `024529dbb2…1758a0db` (matches sample).
- **Python:** `sha256(name + "|build" + BUILD + "|client-half-stub")` (`py:116`).

Consequences to document/fix in the port:
- ROOT is a **stand-in**; per `README.md:38-39` the intended value is a **Merkle hash over placements + asset content hashes** once the asset library exists.
- ROOT currently hashes **only the name string** → identical map names collide to the same ROOT regardless of contents; the two tools produce **different** ROOTs for the same world.

#### 3.1.3 CLIENT (`java:39,61`)
| Value | Meaning |
|---|---|
| `CLIENT scene` | no UE level; client build-from-assets (empty `clientUmap`) |
| `CLIENT umap <path>` | client opens this UE level for visuals |

`<path>` is a UE **package.object** path, e.g. `/Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack`. Python builds it as `f"{pkg}.{pkg.split('/')[-1]}"` from the world's outermost package (`py:82-83`).

### 3.2 Environment records

| Record | Grammar | Columns (name ← py var) | Ref |
|---|---|---|---|
| **SURFACE** | `SURFACE <id> <name> <grip> <roll> <rest>` | `id` int; `name` string; `grip` float `%.2f`; `roll` float `%.3f`; `rest` float `%.2f` | `java:63-65`; `py:31-32,129-130` |
| **GROUND** | `GROUND <z> <flag>` | `z` = ground-plane height (`%.3f` py / `0.0` java); `<flag>` always `0` — **meaning ambiguous** (likely ground surface-id) | `java:66`; `py:132` |
| **KILLZ** | `KILLZ <z>` | kill-plane Z; below this, entities are removed. Java `-5000.0`; py `ground_z − 5000.0` | `java:67`; `py:133` |
| **SPAWN** | `SPAWN <x> <y> <z> <yaw>` | **Python-only** player spawn; `yaw` always `0.0` | `py:134` |
| separator | `#` | | `java:68`; `py:135` |

SURFACE column semantics (inferred from python variable names `grip, roll, rest`; confirm against `OcMap.cs`):

| Col | Inferred meaning |
|---|---|
| `grip` | grip / friction coefficient (1.00 tarmac … 0.45 grass) |
| `roll` | rolling resistance (0.015 tarmac … 0.090 grass) |
| `rest` | restitution / bounce (0.30–0.40) |

Default SURFACE table: **Java emits 3 rows** (`0 tarmac`, `1 kerb`, `2 grass`); **Python emits 4** (adds `3 gravel`) (`py:31-32`). Both are hardcoded defaults ("tune / derive from physical materials later", `py:30`).

### 3.3 Placement records

| Record | Grammar | Ref |
|---|---|---|
| **PLACE** | `PLACE <asset> <x> <y> <z> <yaw> <pitch> <roll> <scale>` | `java:76-77`; `py:139` |
| **DEFORM** | `DEFORM <asset> <x> <y> <z> <yaw> <pitch> <roll> <material>` | `java:72-73`; `py:143` |

Transform tuple — **identical field order in both `.scene` and `.ocmap`**: `x y z yaw pitch roll` then `scale` (PLACE) or `material` (DEFORM).

| Field | Java printf | Type | Units | Notes |
|---|---|---|---|---|
| `asset` | `%s` | string | — | verbatim; DEFORM asset ends `.ocbeam` |
| `x y z` | `%.1f` | float | cm (UE world) | |
| `yaw pitch roll` | `%.1f` | float | degrees | order = yaw, pitch, roll |
| `scale` (PLACE only) | `%.3f` | float | uniform scalar | |
| `material` (DEFORM only) | literal | string | — | **always `rubber`** in both tools (`java:72`; `py:105,143`) |

Notes:
- **DEFORM carries no scale** (dropped); PLACE carries no material.
- **Row ordering differs by tool:** Java writes placements in **input order**, interleaving PLACE/DEFORM (`java:70-80`) — hence sample shows DEFORM before PLACE. Python writes **all PLACE first, then all DEFORM**, each group preceded by a `#` count comment (`py:136-143`).

### Sample `demoworld.ocmap` (annotated)
```
OCMAP 1
# object-reference world exported by OCMapConverter (placements = asset name + transform).
ID 0x376B85BC4D1A03BA                    ← FNV-1a-64("demoworld")
NAME demoworld
BUILD 1
ALGO 3
ROOT 024529dbb250619b7e8331975f0f06b6029fff86144834dce2ed63171758a0db   ← sha256("demoworld|build1|scene")
CLIENT umap /Game/FIA_WEC/Tracks/ConstructorsTestTrack/ConstructorsTestTrack.ConstructorsTestTrack
#
SURFACE 0 tarmac 1.00 0.015 0.30
SURFACE 1 kerb   0.92 0.020 0.40
SURFACE 2 grass  0.45 0.090 0.35
GROUND 0.0 0
KILLZ -5000.0
#
DEFORM tyre_barrier.ocbeam 12000.0 800.0 0.0 90.0 0.0 0.0 rubber
PLACE kerb_4m 1200.0 400.0 0.0 0.0 0.0 0.0 1.000
```

---

## 4. Complete record catalog (reader must handle)

| Keyword | Arity (tokens after keyword) | Emitted by | Section |
|---|---|---|---|
| `OCMAP` | 1 (version) | both | header |
| `ID` | 1 | both | header |
| `NAME` | 1 | both | header |
| `BUILD` | 1 | both | header |
| `ALGO` | 1 | both | header |
| `ROOT` | 1 | both | header |
| `CLIENT` | 1 (`scene`) or 2 (`umap <path>`) | both | header |
| `SURFACE` | 5 | both | env |
| `GROUND` | 2 | both | env |
| `KILLZ` | 1 | both | env |
| `SPAWN` | 4 | **python only** | env |
| `PLACE` | 8 | both | placement |
| `DEFORM` | 8 | both | placement |
| `#…` | — | both | comment |

A faithful reader should ignore `#` lines and tolerate `SPAWN` (present only from the Python exporter).

---

## 5. Coordinate system, units, handedness

Derived from the UE authoring path (`umap_to_ocmap.py:102-109`) and consistent sample magnitudes (`12000`, `800` cm = 120 m, 8 m):

| Aspect | Value | Evidence |
|---|---|---|
| Handedness | **Left-handed** (Unreal) | UE `FTransform`/`FVector`/`FRotator` source, `py:103` |
| Axes | X forward, Y right, **Z up** | UE convention; `GROUND`/`KILLZ`/`SPAWN` treat Z as height (`py:96-98,111-113`) |
| Position units | **centimeters** | UE world units; `t.translation` copied raw (`py:103,139`) |
| Rotation units | **degrees** | UE `FRotator` from `rotation.rotator()` (`py:103`) |
| Rotation storage order in row | **yaw, pitch, roll** | row emits `rot.yaw, rot.pitch, rot.roll` (`py:139`), i.e. UE FRotator's Pitch(Y)/Yaw(Z)/Roll(X) reordered to yaw-pitch-roll |
| Scale | **uniform scalar** (X only) | `scl.x` taken; Y/Z of `scale3d` discarded (`py:109`) |

The Java converter is coordinate-agnostic (pure pass-through of `.scene` numbers); the coordinate contract is imposed by the authoring source (UE), so `.scene` numbers are expected to already be UE cm/degrees.

---

## 6. Encoding, delimiters, line endings, versioning

| Aspect | Value |
|---|---|
| Charset | UTF-8, no BOM (`java:33,85`; `py:147`) |
| Field delimiter | one or more ASCII whitespace (`\s+`) (`java:36`) |
| Comment | `#` to end of line; whole-line `#` also allowed |
| Magic / version | first line `OCMAP 1`; version integer `1` |
| Numeric format | Java `%.1f`/`%.3f`, Python `:.1f`/`:.3f` |
| **Line endings** | **Inconsistent from the Java writer.** Header/env lines appended with literal `"\n"` (LF); `ID`, `PLACE`, `DEFORM` lines use `String.format("…%n")` = `System.lineSeparator()` → **`\r\n` on Windows**. The provided sample confirms mixed LF/CRLF (CRLF on `ID`, `DEFORM`, `PLACE`). Python writes **uniform LF** (`newline="\n"`, `py:147-148`). **Port note:** a robust reader must accept both LF and CRLF per-line; a port's writer should emit consistent LF. |
| **Locale bug (Java)** | `String.format` uses the default JVM locale. Under a comma-decimal locale, floats print as `12000,0`, corrupting the whitespace split and numeric meaning. **Port must use invariant/C locale.** |

---

## 7. UE dependencies to strip/replace in the port

The Java converter (`OCMapConverter.java`) is **standalone, zero UE dependency** — a good direct porting base. The UE coupling lives entirely in the *authoring* exporter `umap_to_ocmap.py`:

| UE API / type | Line | Replace with (port) |
|---|---|---|
| `import unreal` | 20-23 | remove; the port ingests a DCC/glTF scene graph (`README.md:35-37`) |
| `EditorActorSubsystem/EditorLevelLibrary.get_all_level_actors()` | 42-44 | scene-graph node enumeration |
| `UnrealEditorSubsystem/EditorLevelLibrary.get_editor_world()` | 47-49 | scene/document handle |
| `actor.get_component_by_class(StaticMeshComponent)`, `comp.static_mesh`, `mesh.get_name()` | 52-58 | node→mesh-asset-name lookup |
| `SceneComponent`, `get_components_by_class`, `get_class().get_name()` | 71-72 | component/tag introspection |
| `actor.tags` | 64-68 | node metadata/tags |
| `actor.get_actor_transform()` → `.translation`, `.rotation.rotator()` (FRotator), `.scale3d` | 102-103 | engine-neutral transform (Vec3 cm, Euler deg, Vec3 scale) |
| `actor.get_actor_location()`, `get_actor_bounds(only_colliding_components=)` | 92-98 | bounds/height query |
| `actor.get_actor_label()` | 105 | node display name (used to build `<name>.ocbeam`) |
| Class-name string matches `"PlayerStart"`, `"Landscape"`; component hint `"VehicleCage"`; actor tags `OCDeform/ocdeform` | 28-32, 90-104 | port's spawn/ground/deform detection convention |
| `world.get_name()`, `world.get_outermost().get_name()` → `CLIENT umap` package.object path | 81-83 | port's client-level reference scheme |
| `unreal.log(...)` | 150-151 | logging |

Also note the **UE package-path string** baked into `CLIENT umap` (`/Game/...Name.Name`) is a UE asset reference; a royalty-free port needs its own client-level addressing scheme (or `CLIENT scene` build-from-assets).

---

## 8. Open ambiguities (confirm against `OcMap.cs` when porting)

1. **`ALGO 3`** — meaning of the algorithm id is undocumented; hardcoded `3` in both tools.
2. **`GROUND <z> <flag>`** — second field is always `0`; likely the ground-plane surface id (index into SURFACE table) but not shown in any code path.
3. **`ROOT`** — explicitly a placeholder (hashes only the name string); intended design is a Merkle hash over placements + asset content (`README.md:38-39`). Java vs Python formulas differ.
4. **SURFACE column semantics** (`grip/roll/rest`) — inferred from Python variable names only; the authoritative interpretation is in the unseen loader.
5. **DEFORM `material`** — only the literal `rubber` is ever produced; the full set of legal materials is defined by the loader, not these tools.
6. **Reader strictness** — field-count tolerance, unknown-keyword handling, and whether `SPAWN`/extra header ordering are mandatory can only be pinned from `MapRegistry`/`OcMap.cs`, which was not provided.

Relevant absolute paths:
- `C:\Users\User\Documents\OpenConstructorSupportAssets\OCMapConverter\OCMapConverter.java`
- `C:\Users\User\Documents\OpenConstructorSupportAssets\OCMapConverter\README.md`
- `C:\Users\User\Documents\OpenConstructorSupportAssets\OCMapConverter\demoworld.scene`
- `C:\Users\User\Documents\OpenConstructorSupportAssets\OCMapConverter\demoworld.ocmap`
- `C:\Users\User\Documents\Unreal Projects\OpenConstructor27\Scripts\umap_to_ocmap.py`
- Unseen authoritative reader (locate before porting): `MapRegistry` / `OcMap.cs` in the OCServer C# project.