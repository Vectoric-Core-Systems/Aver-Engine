# Fabric G6: the Godot shading-language front-end (contracts)

G6 is the shader half of the Fabric Connector (`FABRIC_CONNECTOR.md`, "Rendering": the shader-language row): a converted Godot 4 project keeps its `.gdshader` /
`.gdshaderinc` files, and Aver runs them. This file holds the design and one section per package. 2D (`shader_type canvas_item`) is out of scope for the whole
converter: the front-end parses and checks it, flags it, and the emitter refuses it.

Rules every package follows (the engine cannot be run, so nothing here has been seen on a GPU):

- Defaults unchanged: a project, level or material that does not use a Godot shader renders exactly as today. New shader code sits behind a compile-time define in
  a variant that is built only when something uses it, or in generated graph code; nothing is added to `PSRayDriven` or the RT shadow code.
- The `VoxiFrame` cbuffer is mirrored three times (`VoxiRenderer.hpp`, `voxi.hlsl`, `voxi_gi.hlsli`); a field added for a shader is added to all three.
- Every shader change is verified with the compile-only tests for both D3D12 (DXIL) and Vulkan (SPIR-V); never a device.
- Every GPU-visible change adds steps to "Headed checks for the owner" at the end of this file.

| Package | What | Status |
|---|---|---|
| SH1 | the front-end: preprocessor, lexer, parser + type checker, HLSL emitter, uniform layout, the hook table (this file) | DONE headless (`GodotShaderTest`) |
| SH2 | spatial shaders bound to the engine (hooks, uniforms, textures, the material path) | DONE headless, GPU side unseen (`GodotShaderMaterialTest`, `VoxiSpirvCompileTest`, `FabricShaderTest`, `ShaderTests`) |
| SH3 | sky, fog and particles shaders bound to the engine, `VisualShader` resources, `GPUParticles3D` with a shader process material | DONE headless, GPU side unseen (`GodotShaderEnvTest`, `FabricEnvShaderTest`, `FrameworkGodotEnvAbiTest`, `EnvShaderTests`) |

---

## SH1: the front-end

Status: done and tested headless. Nothing renders yet: the emitted HLSL is compiled by DXC (DXIL and SPIR-V) in the test and never run on a device.

### Files

| File | Role |
|---|---|
| `modules/formats.godot/include/aver/formats/godot/GodotShader.hpp` | the whole public API (types, AST, module, options, results) |
| `src/GodotShaderPre.cpp` | preprocessor |
| `src/GodotShaderParse.cpp` | lexer, parser and type checker (one pass: Godot requires every name, function included, to be declared before use, so identifiers and types resolve while the AST is built) |
| `src/GodotShaderTypes.cpp` | type lattice, per-shader-type built-in variable tables, render modes, the built-in function overload table, operator typing rules, constant folding |
| `src/GodotShaderHlsl.cpp` | the HLSL emitter and the uniform layout |
| `src/GodotShaderHlslLib.cpp` | the fixed HLSL helper library (`gdShaderHlslLibrary()`) |
| `src/GodotShaderInternal.hpp` | private: overload table, typing rules |
| `tests/formats.godot/src/GodotShaderTest.cpp` | the test (target `GodotShaderTest`) |

The module stays `Aver.Formats.Godot` (Core and Platform only). No dependency was added; DXC is the engine's own and only the test loads it.

### The three calls

```cpp
GdPreprocessed pp;                       gdShaderPreprocess(src, "x.gdshader", opts.pre, pp, diags);     // text + line map
GdShaderModule m;                        gdShaderCompile(src, "x.gdshader", opts, m);                    // typed AST + everything below
GdHlslResult r = gdShaderEmitHlsl(m, GdHlslOptions{});                                                   // HLSL + layouts + slots + hooks
```

`gdShaderCompile` preprocesses, lexes, parses and checks. The module is always filled as far as it got; `m.ok` is "no error diagnostic". Diagnostics
(`GdShaderDiag`) carry file (an `#include` names its own file), line, column (1-based, in the preprocessed line), a code and a message; `gdShaderDiagsToGodot` folds
them into the converter's `GodotDiagnostics`. The first error stops the parse (Godot does the same); warnings do not.

`GdCompileOptions::pre.resolver` answers `#include "path"`: it fills the text and the name later diagnostics use. SH2 supplies one over `GodotProjectFs`
(`res://` and `uid://`). `pre.defines` predefines object-like macros (SH2 sets Godot's renderer macros, e.g. `RENDERER_FORWARD_PLUS`).
`globalUniformNames`, when non-empty, makes a `global uniform` that is not in the list an error (the project's shader globals).

### Preprocessor

`#define` (object-like; function-like with parameters; `##` pasting; a call whose argument list spans lines is joined), `#undef`, `#if` / `#ifdef` / `#ifndef` /
`#elif` / `#else` / `#endif` (integer expressions with `defined(X)` / `defined X`, `! ~ - + * / % << >> < > <= >= == != & ^ | && ||`, `?:`; an identifier that is not a
macro is 0), `#include`, `#pragma` (recorded in `GdPreprocessed::pragmas`; `disable_preprocessor` stops directives and macros for the rest of that file), `#error`,
`#warning`. Comments are removed first (string literals in hints are respected), CRLF and a UTF-8 BOM are accepted, backslash continuation works. The output keeps one
line per origin line and `GdPreprocessed::lines` maps each back to (file, line), so a type error inside an include names the include. Not supported: `#version`,
`#line`, `#extension` (errors), stringising `#`. Limits: include depth 25, macro nesting 64, 1 MiB of expansion per line.

### Language covered

- `shader_type spatial | particles | sky | fog` and `canvas_item` (parsed and checked, `is2D` set, a `gdshader.unsupported_2d` warning, the emitter refuses). `render_mode` lists are
  validated per type.
- Uniforms: every scalar, vector, matrix and sampler type (`sampler2D`, `isampler2D`, `usampler2D`, the `2DArray` / `3D` forms, `samplerCube`, `samplerCubeArray`,
  `samplerExternalOES` parsed), arrays (constant size), defaults folded to numbers, `global`, `instance` (scalars and vectors, at most 16, `instance_index(n)`),
  `group_uniforms` (group and subgroup), and the hints `source_color`, `hint_range(min, max[, step])`, `hint_enum("A", "B:3")`, `hint_normal`,
  `hint_default_white|black|transparent`, `hint_anisotropy`, `hint_roughness_r|g|b|a|normal|gray`, `hint_screen_texture`, `hint_depth_texture`,
  `hint_normal_roughness_texture`, `filter_nearest|linear|nearest_mipmap|linear_mipmap|nearest_mipmap_anisotropic|linear_mipmap_anisotropic`,
  `repeat_enable|disable`. A hint on the wrong type is an error.
- `varying` (with `flat` / `smooth` before or after, arrays), `const` (scalars, vectors, matrices, arrays, structs; folded when constant), `struct` (nested structs and
  arrays; constructor takes the members in order), functions with `in` / `out` / `inout` / `const` parameters, array parameters and returns, overloads resolved by exact
  argument types (Godot has no implicit conversions), the processor functions `vertex` / `fragment` / `light` (spatial), `start` / `process` (particles), `sky`, `fog`.
- Statements: blocks, local declarations (several declarators, `float[3] a` and `float a[3]`, `[]` inferred from the initialiser, `const`), `if` / `else`, `for` (with
  declarations and comma steps), `while`, `do`-`while`, `switch` (constant `int` labels, fall-through, duplicates rejected), `break`, `continue`, `return`, `discard`.
- Expressions with GLSL precedence (unary `+ - ! ~ ++ --`, `* / %`, `+ -`, `<< >>`, `< > <= >=`, `== !=`, `&`, `^`, `|`, `&&`, `||`, `?:`, assignments, the comma operator),
  swizzles (`xyzw`, `rgba`, `stpq`, one set per swizzle), constructors (scalar splat and conversion, vector truncation and concatenation, matrix from a scalar / matrix / columns /
  scalars, struct, array `float[3](...)` and `float[](...)`), indexing, `.length()`, the precision qualifiers (parsed and dropped).
- The built-in function set: trigonometry, exponentials, `abs sign floor trunc round roundEven ceil fract mod modf min max clamp mix fma isinf isnan step smoothstep`, bit casts,
  geometry, matrix functions, vector relational functions, derivatives (plain, coarse, fine), pack / unpack, bit operations, and the whole `texture` family
  (`texture textureProj textureLod textureProjLod textureGrad textureGather textureSize texelFetch textureQueryLod textureQueryLevels`) over every sampler type.
  `frexp`, `ldexp`, `umulExtended`, `imulExtended`, `uaddCarry`, `usubBorrow` type-check but have no HLSL spelling (the emitter reports them).
- Built-in variables per processor function with read / write rules, from the documented tables (below). Writing an `in` built-in, a uniform, a constant, or a varying
  outside `vertex()` / `fragment()` is an error; reading a built-in that belongs to another function is an error that names both; a custom function sees only the
  global ones (`TIME`, `PI`, `TAU`, `E`, and the sky shader's `POSITION`, `RADIANCE`, `LIGHTn_*`, `AT_*_PASS`).

### Where this front-end is more lenient than Godot (so it accepts what Godot accepts)

- An `out` built-in may be read (`ALBEDO *= 2.0`, `DIFFUSE_LIGHT += ...` are everywhere in real shaders).
- A varying read in `fragment()` / `light()` before any assignment in `vertex()` is a warning (`gdshader.varying_unassigned`), not an error.
- A non-void function's "returns on every path" check treats a `switch` as returning.
- Constructors accept a larger vector than needed (`vec2(vec4)`, `float(vec2)`: the leading components), as GLSL does.
- `VERTEX` is readable in `light()`; `canvas_item` tables are approximate (the shader is not converted).

### Assumptions to check against Godot 4.4's source (taken from the docs, or from memory where the docs page was ambiguous)

`AMOUNT_RATIO` (particles) is a `float`; `SDF` (fog) is a `float`; `MODEL_MATRIX` / `VIEW_MATRIX` are read-only in `vertex()` (the docs say `in`); `OUTPUT_IS_SRGB` is a `bool` in
`vertex()` and `fragment()`; `FRAGCOORD` in `light()` is a `vec4`. The built-in tables are plain data (`GodotShaderTypes.cpp`), one row per (name, stage set).

### Limits (so hostile or broken input is an error, never a crash)

Nesting of expressions and statements 96 (counted together), 300 chained binary operators / member accesses / commas in one expression, integer literals that do not fit 64 bits are
errors, an iterative `~GdExpr`. The test feeds truncated, mutated and hostile shaders (100 000 parentheses, 100 000-term sums, macro bombs).

---

## HLSL emission

`gdShaderEmitHlsl(module, GdHlslOptions)` returns `GdHlslResult`: `source` (needs `gdShaderHlslLibrary()` before it and the hook macros), `varyingIo`, the three uniform blocks,
`textures`, `samplers`, `hooks` (every `AVER_GD_*` macro the source uses, with read / written and type), `entries`. It fails (`ok = false`, `error`, `diags`) for `canvas_item`, for a module
with errors, and for the few constructs HLSL has no spelling for: an array-returning function, an array constructor outside a declaration / constant initialiser, a sampler array, a matrix-array
uniform, comparing structs or arrays, and the built-ins listed above. `GdHlslOptions::processorMask` emits only some processor functions (and the custom functions they reach), so a vertex
compile never references fragment-only hooks.

### Types and names

| Godot | HLSL |
|---|---|
| `bool bvecN` / `int ivecN` / `uint uvecN` / `float vecN` | `bool boolN` / `int intN` / `uint uintN` / `float floatN` |
| `mat2 mat3 mat4` | `float2x2 float3x3 float4x4` (GLSL memory order, below) |
| `sampler2D` ... `samplerCubeArray` | `GdSampler2D` ... (structs of a `Texture*<T>` and a `SamplerState`, defined by the library) |
| `struct S` | `GdS_S` with members `m_name`, plus a constructor function `gdctor_S(...)` |
| arrays | `T name[N]` (parameters and members too) |

Every user identifier is prefixed so no Godot name can collide with an HLSL keyword or intrinsic (a Godot variable may be called `frac`, `sample` or `float2`): locals and parameters `l_`, functions
`gdf_`, global constants `gdc_`, struct members `m_`, varyings `gdv_`. Precision qualifiers are dropped.

### Matrices

GLSL matrices are column-major and `A * B` is the matrix product with column vectors. The emitter keeps GLSL's memory order: an HLSL `floatNxN` holds GLSL column `i` in row `i`, so `m[i]` is column `i`
(same as GLSL), constructors from columns and from `N*N` scalars are the same text, and `uniform mat4` is four consecutive registers. The products become:

| GLSL | HLSL |
|---|---|
| `A * B`, `M * v`, `v * M` | `mul(B, A)`, `mul(v, M)`, `mul(M, v)` (always `mul(rhs, lhs)`) |
| `M * s`, `M + M`, `M / M` | component-wise, plain operators |
| `M *= B` | `M = mul(B, M)` |
| `transpose`, `determinant` | the intrinsics |
| `inverse(M)` | `gd_inverse(M)` (the inverse of the HLSL matrix is the stored form of the GLSL inverse) |
| `matrixCompMult`, `outerProduct` | `a * b`, `gd_outer` |
| `mat3(float)` (diagonal), `mat4(mat3)` | `gd_mat3_diag(s)`, `gd_mat4_from3(m)`; `mat3(mat4)` is a cast |
| `==` / `!=` on matrices | `gd_eq` (DXC's SPIR-V back end has no matrix `==`) |

Vector `==` / `!=` (one `bool` in GLSL) is `all(a == b)` / `any(a != b)`. Because the matrices are never stored in a constant buffer by the compiler (see the uniform layout), no
`-Zpr` / `row_major` setting matters.

### Built-in variables are hook macros

A built-in `NAME` is emitted as `AVER_GD_NAME` (an lvalue wherever Godot allows a write: it is usually a field of a context struct). `PI`, `TAU` and `E` are emitted as literals.
The consumer (SH2 / SH3) defines every macro in `GdHlslResult::hooks` before the generated source; a macro may expand to a field, to a function call (for reads) or to an
expression, and may be defined differently for the vertex, fragment and light compiles (use `processorMask`). A hook of sampler type (`RADIANCE` in sky shaders) must expand to a
`GdSamplerCube` value. Reading a built-in the shader only writes (`ALBEDO = x`) does not list a read.

The hooks, per shader type (`GodotShaderTest --hooks` regenerates this; the macro is `AVER_GD_` + the name; "Where" is the processor function(s); a name repeated with another access
is that stage's own row):

#### spatial

| Built-in | Type | Access | Where |
|---|---|---|---|
| `TIME` | float | in | everywhere |
| `PI` `TAU` `E` | float | in | everywhere (literals) |
| `OUTPUT_IS_SRGB` | bool | in | vertex, fragment |
| `VIEWPORT_SIZE` | vec2 | in | vertex, fragment, light |
| `VIEW_MATRIX` `INV_VIEW_MATRIX` `INV_PROJECTION_MATRIX` | mat4 | in | vertex, fragment, light |
| `MAIN_CAM_INV_VIEW_MATRIX` | mat4 | in | vertex |
| `NODE_POSITION_WORLD` `NODE_POSITION_VIEW` `CAMERA_POSITION_WORLD` `CAMERA_DIRECTION_WORLD` | vec3 | in | vertex, fragment |
| `CAMERA_VISIBLE_LAYERS` | uint | in | vertex, fragment |
| `INSTANCE_ID` | int | in | vertex |
| `INSTANCE_CUSTOM` | vec4 | in | vertex |
| `VIEW_INDEX` `VIEW_MONO_LEFT` `VIEW_RIGHT` | int | in | vertex, fragment |
| `EYE_OFFSET` | vec3 | in | vertex, fragment |
| `VERTEX` | vec3 | inout | vertex |
| `VERTEX` | vec3 | in | fragment, light |
| `VERTEX_ID` | int | in | vertex |
| `POSITION` | vec4 | out | vertex |
| `ROUGHNESS` | float | out | vertex, fragment |
| `ROUGHNESS` `METALLIC` | float | in | light |
| `POINT_SIZE` | float | inout | vertex |
| `MODELVIEW_MATRIX` | mat4 | inout | vertex |
| `MODELVIEW_NORMAL_MATRIX` | mat3 | inout | vertex |
| `MODEL_MATRIX` | mat4 | in | vertex, fragment, light |
| `MODEL_NORMAL_MATRIX` | mat3 | in | vertex, fragment |
| `PROJECTION_MATRIX` | mat4 | inout | vertex, fragment, light |
| `BONE_INDICES` | uvec4 | in | vertex |
| `BONE_WEIGHTS` `CUSTOM0` `CUSTOM1` `CUSTOM2` `CUSTOM3` | vec4 | in | vertex |
| `Z_CLIP_SCALE` | float | out | vertex |
| `NORMAL` `TANGENT` `BINORMAL` | vec3 | inout | vertex, fragment |
| `NORMAL` | vec3 | in | light |
| `UV` `UV2` | vec2 | inout | vertex |
| `UV` `UV2` | vec2 | in | fragment, light |
| `COLOR` | vec4 | inout | vertex |
| `COLOR` | vec4 | in | fragment |
| `FRAGCOORD` | vec4 | in | fragment, light |
| `FRONT_FACING` | bool | in | fragment |
| `VIEW` | vec3 | in | fragment, light |
| `POINT_COORD` | vec2 | in | fragment |
| `LIGHT_VERTEX` | vec3 | inout | fragment |
| `SCREEN_UV` | vec2 | in | fragment, light |
| `DEPTH` `ALPHA_SCISSOR_THRESHOLD` `ALPHA_HASH_SCALE` `ALPHA_ANTIALIASING_EDGE` `PREMUL_ALPHA_FACTOR` `METALLIC` `SPECULAR` `RIM` `RIM_TINT` `CLEARCOAT` `CLEARCOAT_GLOSS` `ANISOTROPY` `SSS_STRENGTH` `SSS_TRANSMITTANCE_DEPTH` `SSS_TRANSMITTANCE_BOOST` `AO` `AO_LIGHT_AFFECT`, `NORMAL_MAP_DEPTH` | float | out | fragment |
| `ALPHA` | float | out | fragment, light |
| `NORMAL_MAP` `BENT_NORMAL_MAP` `ALBEDO` `EMISSION` | vec3 | out | fragment |
| `ALBEDO` | vec3 | in | light |
| `ALPHA_TEXTURE_COORDINATE` `ANISOTROPY_FLOW` | vec2 | out | fragment |
| `SSS_TRANSMITTANCE_COLOR` `FOG` `RADIANCE` `IRRADIANCE` | vec4 | out | fragment |
| `BACKLIGHT` | vec3 | inout | fragment, light |
| `LIGHT` `LIGHT_COLOR` | vec3 | in | light |
| `SPECULAR_AMOUNT` `ATTENUATION` | float | in | light |
| `LIGHT_IS_DIRECTIONAL` | bool | in | light |
| `DIFFUSE_LIGHT` `SPECULAR_LIGHT` | vec3 | out | light |

#### particles

| Built-in | Type | Access | Where |
|---|---|---|---|
| `TIME` `PI` `TAU` `E` | float | in | everywhere |
| `LIFETIME` `DELTA` `INTERPOLATE_TO_END` `AMOUNT_RATIO` | float | in | start, process |
| `NUMBER` `INDEX` `RANDOM_SEED` `FLAG_EMIT_POSITION` `FLAG_EMIT_ROT_SCALE` `FLAG_EMIT_VELOCITY` `FLAG_EMIT_COLOR` `FLAG_EMIT_CUSTOM` | uint | in | start, process |
| `EMISSION_TRANSFORM` | mat4 | in | start, process |
| `EMITTER_VELOCITY` | vec3 | in | start, process |
| `USERDATA1` ... `USERDATA6` | vec4 | in | start, process |
| `ACTIVE` | bool | inout | start, process |
| `COLOR` `CUSTOM` | vec4 | inout | start, process |
| `VELOCITY` | vec3 | inout | start, process |
| `TRANSFORM` | mat4 | inout | start, process |
| `MASS` | float | inout | start, process |
| `RESTART_POSITION` `RESTART_ROT_SCALE` `RESTART_VELOCITY` `RESTART_COLOR` `RESTART_CUSTOM` | bool | in | start |
| `RESTART` `COLLIDED` | bool | in | process |
| `COLLISION_NORMAL` `ATTRACTOR_FORCE` | vec3 | in | process |
| `COLLISION_DEPTH` | float | in | process |

#### sky

| Built-in | Type | Access | Where |
|---|---|---|---|
| `TIME` `PI` `TAU` `E` | float | in | everywhere |
| `POSITION` | vec3 | in | everywhere |
| `RADIANCE` | samplerCube | in | everywhere |
| `AT_HALF_RES_PASS` `AT_QUARTER_RES_PASS` `AT_CUBEMAP_PASS` `LIGHTn_ENABLED` (n = 0..3) | bool | in | everywhere |
| `LIGHTn_ENERGY` `LIGHTn_SIZE` | float | in | everywhere |
| `LIGHTn_DIRECTION` `LIGHTn_COLOR` | vec3 | in | everywhere |
| `EYEDIR` | vec3 | in | sky |
| `SCREEN_UV` `SKY_COORDS` | vec2 | in | sky |
| `HALF_RES_COLOR` `QUARTER_RES_COLOR` | vec4 | in | sky |
| `COLOR` | vec3 | out | sky |
| `ALPHA` | float | out | sky |
| `FOG` | vec4 | out | sky |

#### fog

| Built-in | Type | Access | Where |
|---|---|---|---|
| `TIME` `PI` `TAU` `E` | float | in | everywhere |
| `WORLD_POSITION` `OBJECT_POSITION` `UVW` `SIZE` | vec3 | in | fog |
| `SDF` | float | in | fog |
| `ALBEDO` `EMISSION` | vec3 | out | fog |
| `DENSITY` | float | out | fog |

### Built-in functions

The overload table is data (`GodotShaderTypes.cpp`, a one-line signature per family, expanded for sizes 1..4). `GodotShaderTest --functions` regenerates the counts. "same name" is the HLSL
intrinsic of that name; `gd_*` is in the library; `@x` is an inline rewrite.

| Godot | HLSL |
|---|---|
| `radians degrees sin cos tan asin acos sinh cosh tanh pow exp exp2 log log2 sqrt abs floor trunc ceil modf isinf isnan length distance dot cross normalize reflect refract faceforward transpose determinant any all fwidth` | same name |
| `atan(x)` / `atan(y, x)` | `atan` / `atan2` |
| `asinh acosh atanh` | `gd_asinh gd_acosh gd_atanh` |
| `inversesqrt fract fma` | `rsqrt frac mad` |
| `round roundEven` | `round` (HLSL rounds half to even) |
| `sign` | `gd_sign` (HLSL's `sign` returns int) |
| `mod` | `gd_mod` (`x - y * floor(x / y)`, not `fmod`; vector / scalar forms) |
| `min max clamp` | same name; a scalar argument beside a vector is splatted (`@splat`) |
| `mix` | `gd_mix` (float, vector, scalar weight, `bool` / `bvec` select) |
| `step smoothstep` | `gd_step gd_smoothstep` (scalar edges beside a vector) |
| `floatBitsToInt floatBitsToUint intBitsToFloat uintBitsToFloat` | `asint asuint asfloat asfloat` |
| `matrixCompMult outerProduct inverse` | `a * b`, `gd_outer`, `gd_inverse` |
| `lessThan lessThanEqual greaterThan greaterThanEqual equal notEqual not` | the operator on the vectors (`@cmp`, `@not`) |
| `dFdx dFdy dFdxCoarse dFdyCoarse dFdxFine dFdyFine` | `ddx ddy ddx_coarse ddy_coarse ddx_fine ddy_fine` |
| `fwidthCoarse fwidthFine` | `gd_fwidthCoarse gd_fwidthFine` |
| `packHalf2x16 packUnorm2x16 packSnorm2x16 packUnorm4x8 packSnorm4x8` and the `unpack` forms | `gd_` + the name |
| `bitfieldExtract bitfieldInsert bitCount findLSB findMSB` | `gd_` + the name |
| `bitfieldReverse` | `reversebits` |
| `texture textureProj textureLod textureProjLod textureGrad textureGather textureSize texelFetch textureQueryLod textureQueryLevels` | `gd_` + the name, over the sampler structs |
| `frexp ldexp umulExtended imulExtended uaddCarry usubBorrow` | none (reported) |

Integer-sampler `texture()` / `textureLod()` read the nearest texel through `Load`; `textureQueryLod` on them returns zero (an integer texture cannot be filtered).
In a stage without implicit derivatives (vertex, compute) define `AVER_GD_NO_DERIVATIVES` before the library: `texture()` then samples level 0 (Godot samples level 0 in `vertex()` too).

### Uniforms: the register layout

Uniforms are read from a `float4` register array, so the consumer fills a plain array of 16-byte registers and no compiler packing rule is involved. There are three blocks, with their own
macros: `AVER_GD_UBUF` (normal), `AVER_GD_IBUF` (instance uniforms), `AVER_GD_GBUF` (global uniforms); `GdHlslResult::uniforms` / `instanceUniforms` / `globalUniforms` list the slots
(`GdUniformSlot`: name, type, first register, first component, registers used, array stride) and `registerCount`. `gdShaderLayoutUniforms` gives the layout without emitting code.

Packing is std140's, in declaration order: a scalar takes one component; a `vec2` starts on component 0 or 2; a `vec3` and a `vec4` start a register; a scalar may follow a `vec3` in its fourth component;
a matrix starts a register and takes one register per column (`mat3` = 3 registers, three components used each); an array starts a register and each element takes its own register(s)
(`float[3]` = 3 registers, x only). `int`, `uint` and `bool` are the 32-bit pattern in the float slot (`asint` / `asuint`; `bool` is 0 / 1). Samplers take no register: they are texture
slots. Instance uniforms are laid out in declaration order (`GdUniform::instanceIndex` is the author's `instance_index` hint, for SH2's per-instance table).

### Textures and samplers

Each sampler uniform becomes `Texture*<T> gdt_<name> : register(t<base + i>)` and a `GdSampler*` value made by `gdsm_<name>()`; `GdTextureSlot` carries the name, type, register, the sampler
state, the hint (`GdTexHint`: default white / black / normal, roughness, screen / depth / normal-roughness texture, ...), `source_color` and the global flag. Sampler states are deduplicated by
(filter, repeat): `gds_<filter>_<repeat|clamp> : register(s<base + i>)`, `GdSamplerStateDesc` says which filter and repeat mode to create. A uniform without `filter_*` / `repeat_*` takes
`GdHlslOptions::defaultFilter` (linear mipmap) / `defaultRepeat` (disable), Godot's project defaults. A sampler uniform of the global scope is a texture slot with `isGlobal`.

### Varyings

`GdHlslResult::varyingIo` is a block of members for the stage IO struct (`nointerpolation int gdv_a : TEXCOORD3;`, `float3 gdv_b[2] : TEXCOORD4;`; semantics start at
`varyingSemanticBase` and advance by the rows a member uses). The generated code reads and writes `AVER_GD_VARYINGS.gdv_<name>`; the consumer defines `AVER_GD_VARYINGS` as the vertex
output / pixel input instance.

### Entry points

Processor functions are emitted as `<entryPrefix>_vertex()`, `_fragment()`, `_light()`, `_start()`, `_process()`, `_sky()`, `_fog()` (all `void`, no parameters: they communicate through the
hooks). `GdHlslResult::entries` lists them. The consumer writes the real entry (`VSMain`, `PSMain`, a compute kernel) around them. `discard` in `fragment()` / `light()` and in custom
functions is HLSL `discard`.

### The helper library

`gdShaderHlslLibrary()` is generated text, included once: the sampler wrapper structs, the `gd_texture*` family for every sampler type, `gd_mod / mix / sign / step / smoothstep / asinh ...`,
bit helpers, pack / unpack, matrix helpers (`gd_mat*_diag`, `gd_mat*_from*`, `gd_inverse`, `gd_outer`, `gd_eq`). It guards itself (`AVER_GD_LIBRARY`).

---

## The SH1 contract for later packages

1. **Never re-parse.** A consumer compiles with `gdShaderCompile` once and reads `GdShaderModule` (uniforms with hints and folded defaults, varyings, render modes, processor functions, `usesDiscard`).
2. **Hooks.** Define every macro in `GdHlslResult::hooks` before the generated source (type and read / write use are listed); compile each stage with its own `processorMask`; define
   `AVER_GD_NO_DERIVATIVES` for vertex and compute compiles; define `AVER_GD_UBUF` / `IBUF` / `GBUF` as `float4` arrays of at least `registerCount` registers and `AVER_GD_VARYINGS`.
   The test's stub (`hookPrelude` in `GodotShaderTest.cpp`) is the smallest complete example: constant buffers for the three blocks, a static IO struct for the varyings, a `static` variable per hook.
3. **Bindings.** Texture `t#` and sampler `s#` registers are `textureRegisterBase + i` / `samplerRegisterBase + i`; SH2 chooses the bases for its pipeline layout (and the Vulkan binding shifts, as the
   other variants do).
4. **Hints are data.** `source_color` (sRGB sampling), `hint_normal` / `hint_roughness_*` / `hint_default_*` (the texture to bind when none is set), `hint_screen_texture` / `hint_depth_texture` /
   `hint_normal_roughness_texture` (renderer targets, which SH2 must provide) arrive as `GdTexHint` on the texture slot; the emitter does not act on them.
5. **Report.** Anything the emitter refuses (`GdHlslResult::diags`) or the front-end warns about is for `FabricReport` ("unsupported" / "approximated" with the message as the reason);
   `gdShaderDiagsToGodot` converts the diagnostics.
6. **Not in SH1:** reading `.gdshader` files from a project (a resolver over `GodotProjectFs`), `ShaderMaterial` mapping, `.ocgraph` / material-graph integration, the engine side of any hook, instance
   and global uniform storage, and everything about rendering. A graph domain for vertex output, varyings, screen / depth textures and the sky / fog / particle domains is SH2 and SH3's decision;
   SH1 emits HLSL source text and is neutral about how it reaches the shader.

## Tests

`GodotShaderTest` (`tests/formats.godot/src/GodotShaderTest.cpp`): the preprocessor (macros with arguments, `##`, `#undef`, conditionals, `defined()`, includes through a resolver with the line map,
`#pragma`, `#error`, CRLF); operator precedence checked by folding 24 constants against C++; every statement kind present in the AST; overload resolution (user and built-in); 44 type, syntax and
include errors checked at their exact line and column (one inside an include); uniform hints, groups, instance and global uniforms; the built-in variable rules of all four shader types; the uniform
layout against hand-worked registers; emitted-text spot checks; robustness against truncated, mutated and hostile input; and **twelve representative shaders** (water, toon with `light()`,
dissolve with `discard`, triplanar with sampler parameters, vertex wind with an instance uniform and a vertex texture read, fresnel rim, a language torture shader, varyings, a built-ins shader that
calls every supported overload family, particles, sky, fog) emitted and compiled by DXC to **DXIL** (`build/bin/dxcompiler.dll`) and to **SPIR-V** (`third_party/dxc-spirv`) per stage (`vs_6_5`, `ps_6_5`,
`cs_6_5`) with a stub hook prelude. `AVER_GD_DUMP=1` writes each compiled source to `%TEMP%/AverGodotShaderTest`. `GodotShaderTest <file.gdshader>` compiles one file and prints the diagnostics
and the HLSL; `--hooks` and `--functions` print the tables above. DXC accepting the text proves it is valid HLSL, not that it means what the author wrote: matrix order, `mod`, packing and
sampler states are reasoned from the rules above and need the first GPU run (SH2) to confirm.

---

## SH2: spatial shaders on Aver materials

Status: engine, converter, `Aver.Fabric` and headless tests done. Nothing here has run on a GPU, in the editor or in the game (the engine-run ban): the generated HLSL compiles to DXIL and SPIR-V in the engine's real
translation unit and the converter's output reads back through the engine's own loaders, which proves the plumbing and the text, not the picture. Read the headed checks below before trusting a pixel.

### The shape of it

```
foo.gdshader --gdShaderForEngine--> GdEngineShader --FabricShader.cpp--> Content/Shaders/foo.ocgraph   (DOMAIN material, one GodotShader node: the emitted HLSL)
ShaderMaterial resource ----------------------------------------------> Content/Materials/<Name>_<mat>.ocmat  (GRAPHREF, FX, GDREGS, TEX per sampler slot)
                                                                         Fabric/shaders.json                  (uniform layout, sampler slots, globals, materials)
```

At run time `GameContent::resolveMaterialGraph` loads the graph (the existing GRAPHREF path), `MaterialGraphRegistry::add` unpacks the node (`compileMaterialGraph`) and `materialGraphHlsl` builds the arms.
A project with no converted shader gets exactly the code it always did (`MaterialGraphRegistry`: `hasGodotShaders()` false, `vsOutMembersDefine()` empty, the generated text has no `AVER_GD`; `GodotShaderMaterialTest` pins it).

### The files

| File | Role |
|---|---|
| `modules/formats.godot/src/GodotShaderEngine.cpp`, `include/.../GodotShaderEngine.hpp` | `gdShaderForEngine`: compile, give each sampler a material texture slot, emit the fragment and the vertex source (slot-sampler mode, `GDN()` name macro, shader-unique prefix), fold the uniform defaults into the table, read the render modes and the hooks the shader uses, and list what the engine does only approximately (`GdEngineNote`) |
| `modules/formats.godot/src/GodotShaderHlsl*.cpp` | SH1 emitter, extended: `GdHlslOptions::namePrefix`, `nameMacro`, `slotSamplers` / `samplerSlots`; `GdHlslResult::varyingSlots`; the library gained `GdSampler2D { slot, m }` under `AVER_GD_SLOT_SAMPLERS` with `gd_*` (raster) and `gdr_*` (ray) texture functions |
| `modules/formats/.../OcGraphGodot.hpp`, `src/OcGraphGodot.cpp` | the `GodotShader` node: `packGodotShader` / `unpackGodotShader`, percent-escaped chunked attributes (survive the `.ocgraph` line format and the editor's rewrite), the `gdfx` render-mode bits |
| `modules/render.pbr/shaders/material_godot.hlsl` | the engine side of every hook (below) |
| `modules/render.pbr/shaders/material_godot_lib.hlsl` | the SH1 helper library as a shipped file; `GodotShaderMaterialTest` fails if it differs from `gdShaderHlslLibrary()` (regenerate: `GodotShaderTest --library`) |
| `modules/render.pbr/src/MaterialGraphHlsl.cpp`, `MaterialGraphRegistry.cpp` | the `GodotShader` branch of `compileMaterialGraph`, the arms, both flavours, the varying plumbing, the fx fallback |
| `modules/render.pbr/.../Material.hpp`, `MaterialFx.hpp`, `MaterialSystem.cpp`, `Material.cpp`, `pbr_abi.h` | `MaterialDesc::gd`, `MaterialFx::gdVertex / gdDiscard`, key bit `kGdVertex` (22), the uniform-table texture, the C ABI |
| `modules/formats/src/OcMat.cpp` | `GDREGS`, FX keys `gdvs` and `gddiscard` |
| `modules/rhi*`, `shared_prelude.hlsl`, `voxi.hlsl`, `voxi_rt.hlsli`, `VoxiRenderer.cpp` | `Format::RGBA32Float`, `AVER_GD_VSOUT_EXTRA` in `VSOut`, `VSMainGd`, the ray adapters `averRtGd*`, the variant's defines and vertex entry |
| `tools/fabric/FabricShader.cpp` | the converter |
| `scripting/csharp/Aver.Fabric/Nodes/Shading.cs` | `Shader`, `ShaderMaterial`, `RenderingServer.global_shader_parameter_*` |

### Uniforms: the table

`MaterialDesc::gd.regs` is the shader's uniform block (SH1's register layout: normal block, then instance, then global; 4 floats a register, ints and bools as bit patterns). `.ocmat` writes it as `GDREGS` lines of hexadecimal words.
`MaterialSystem` uploads it as an `RGBA32Float` texture `registerCount` texels wide (one register per texel, 256 registers at most) bound to **texture slot 7** (`TextureSlot::Layer1Normal`, `kGdTableSlot`), rebuilt only when the values change.
The generated arm copies the registers into static arrays (`gGdU / gGdI / gGdG`, the `AVER_GD_UBUF / IBUF / GBUF` of the SH1 contract) with `Load`, so a vertex stage reads them too and a ray hit reads them through the bindless table (`texIndex[7]`).
`aver_pbr_set_gd_registers` (and `Aver.Scripting.Pbr.PbrMaterial.SetGdRegisters`) rewrites the table: **no recompile for a changed parameter**, one tiny texture per change. A material that sets registers must not use slot 7 itself (the `.ocmat` reader overwrites it).
A `source_color` vec3 / vec4 is stored linear: Godot decodes it when it uploads (piecewise sRGB, not the engine's gamma 2.2), so the converter (defaults and `shader_parameter/*`) and `Aver.Fabric` do the same.

### Textures and sampler states

A `sampler2D` uniform is given a material slot at convert time: `source_color` samplers take the slots the engine decodes as sRGB (0, 4, 5), the others the linear ones (1, 3, 6, 2), in declaration order; slot 7 is the table. So at most 3 sRGB and 4 linear textures; more is an error with that message.
`gdsm_<name>()` returns `{ slot, mode }`; the mode word is bit 0 nearest, bit 1 repeat, bit 2 mipmaps, bits 8-9 the colour an unbound texture reads (white, black, transparent, flat normal: `hint_default_*`, `hint_normal`).
The engine has one material sampler (wrap, linear, anisotropic), so Godot's states are emulated in `gdSlotSample`: `repeat_disable` clamps the coordinate to the edge texels, `filter_nearest*` samples with `Load` at the texel (and the mip level when mipmaps are asked for), the rest use the sampler.
`texture`, `textureLod`, `textureGrad`, `textureSize`, `textureQueryLevels`, `textureQueryLod`, `texelFetch`, `textureProj*` and `textureGather` are implemented for `sampler2D`. 3D, cube, array and integer samplers are an error ("the engine's material takes sampler2D textures only").
`hint_screen_texture`, `hint_depth_texture`, `hint_normal_roughness_texture` and global samplers are **not bindable**: they take a spare slot that stays unbound, so the shader reads the hint's default colour, and the report says so (approximated).
The engine does have a copy of the opaque scene for blended draws (`gBlendBackdrop`, t10) and of the depth buffer; SH2 does not bind them to the material pass: it is declared after the generated code in `voxi.hlsl` and exists only in the blended replay.

### The hooks (`material_godot.hlsl`)

One context struct `gGd` and the `AVER_GD_<NAME>` macros of every spatial built-in in the SH1 table (the vertex-only, the fragment-only and the shared ones). **Spaces are Godot's**: the basis change of `aver::fmt::axis` (engine = (-z, x, y) for a direction, times 100 for a position)
is applied at the boundary, so `VERTEX` in `fragment()` is Godot view space in metres (x right, y up, -z forward), `NORMAL` / `TANGENT` / `BINORMAL` view-space directions, `VIEW` the vector to the camera, `MODEL_MATRIX`, `VIEW_MATRIX`, `INV_VIEW_MATRIX`, `NODE_POSITION_*`,
`CAMERA_POSITION_WORLD`, `CAMERA_DIRECTION_WORLD` in Godot world axes; matrices keep GLSL's memory order (SH1: an HLSL `float4x4` holds GLSL column i in row i).
In `vertex()` `VERTEX` / `NORMAL` are model space (or Godot world space with `world_vertex_coords`; with `skip_vertex_transform` the shader transforms by `MODELVIEW_MATRIX` itself and the engine reads the result as view space), `UV`, `UV2`, `COLOR`, `TANGENT` are read from the mesh and written back.

| fragment() output | lands in |
|---|---|
| `ALBEDO`, `ALPHA`, `METALLIC`, `ROUGHNESS`, `EMISSION`, `AO` | `AverAuthored` `baseColor`, `opacity`, `metallic`, `roughness`, `emissive`, `occlusion` (unwritten ones keep Godot's defaults: white, 1, 0, 1, 0, 1) |
| `ALPHA_SCISSOR_THRESHOLD` | `alphaCutoff`; the converter makes the material masked (`AlphaMode::Mask`) when the shader writes it, blended when it writes `ALPHA` (or uses `blend_add / sub / mul`) |
| `NORMAL_MAP`, `NORMAL_MAP_DEPTH` | `normalTS` (xy scaled by the depth, then normalised) |
| `NORMAL` (written) | converted to a tangent-space perturbation in the frame `averPerturbNormal` builds (orthonormalised: approximated) |
| `SPECULAR` | `reflectance = 0.08 * SPECULAR`, re-derived in `gdPost` (F0 and F) |
| `RIM`, `RIM_TINT`, `BACKLIGHT` | the sun's share, added as glow in `gdPost` (the fx evaluator's formulas) |
| `ANISOTROPY` | the lobe is widened (`rough *= 1 + 0.5 |a|`), no stretched highlight |
| `CLEARCOAT`, `CLEARCOAT_GLOSS` | `coatWeight`, `coatRoughness = 1 - gloss` (live with `RENDER.LAYEREDBSDF`) |
| `SSS_STRENGTH`, `SSS_TRANSMITTANCE_COLOR`, `SSS_TRANSMITTANCE_DEPTH` | `subsurfaceWeight`, `subsurfaceColor`, `subsurfaceRadius = depth / 4` |
| `discard` | HLSL `discard`; the material key asks for the late-Z variant (`gddiscard`) |
| written and ignored (reported) | `ALPHA_HASH_SCALE`, `ALPHA_ANTIALIASING_EDGE`, `ALPHA_TEXTURE_COORDINATE`, `PREMUL_ALPHA_FACTOR`, `AO_LIGHT_AFFECT`, `ANISOTROPY_FLOW`, `SSS_TRANSMITTANCE_BOOST`, `BENT_NORMAL_MAP`, `LIGHT_VERTEX`, `FOG`, `RADIANCE`, `IRRADIANCE`, `DEPTH`, `POSITION`, `POINT_SIZE`, `Z_CLIP_SCALE`, `MODELVIEW_*` writes |
| read as a constant (reported) | `PROJECTION_MATRIX`, `INV_PROJECTION_MATRIX` (identity: the per-frame block carries no projection), `VIEWPORT_SIZE` (1920 x 1080), `POINT_COORD`, `VERTEX_ID`, `INSTANCE_ID`, `INSTANCE_CUSTOM`, `CUSTOM0..3`, `BONE_*`, `CAMERA_VISIBLE_LAYERS`, `OUTPUT_IS_SRGB`; `FRAGCOORD.xy` and `SCREEN_UV` come from the clip position and the assumed size |

`render_mode` -> material: `cull_*`, `blend_add / sub / mul / premul_alpha`, `depth_draw_*`, `depth_test_disabled`, `shadows_disabled` (no shadows received), `render_priority` become the FX record; `unshaded`, `ambient_light_disabled`, `specular_disabled`,
`diffuse_lambert_wrap` are the `gdfx` bits `gdPost` acts on; `world_vertex_coords` and `skip_vertex_transform` steer the vertex hook. Approximated (reported): `diffuse_toon / lambert / burley`, `specular_toon / blinn / phong`, `vertex_lighting`, `depth_prepass_alpha`, `blend_premul_alpha`, `alpha_to_coverage*`.
Unsupported (reported): `fog_disabled`, `wireframe`, `shadow_to_opacity`, `light()`.

### The vertex stage and varyings

`fx.gdVertex` (the shader has a `vertex()`) sets key bit `kGdVertex`; the scene variant is built with `VSMainGd` (`voxi.hlsl`: `VSMain`, then `averGdVertex`, which switches on `gMaterialGraphId`) and `AVER_MATX_GDVS=1`, `AVER_GD_NO_DERIVATIVES=1` (texture reads in `vertex()` sample level 0).
Varyings: the shaders' varying members are named in `AVER_GD_VSOUT_MEMBERS` (TEXCOORD4 upward, `nointerpolation` for `flat`) by `voxiShaderPrelude()`, put into `VSOut` only when the variant defines `AVER_GD_VARYINGS_ON`, copied out by `averGdStoreVaryings` and back by `averGdLoadVaryings` (first line of `PSMainVoxi`).
The converted shaders share 24 rows of vertex output; a graph that would exceed it does not register and the material shades stock (logged). Member names carry the shader's unique prefix, so two shaders may declare the same varying.
A draw with a vertex stage is not in the depth prepass (`kFxNoPrepassKeys`), the shadow cascades and the depth-only draw use `VSMain` (the unmoved mesh), and ray hits and GI see the unmoved mesh.

### Ray-driven mode (`RENDER.RTRENDERMODE 1`, the default from the Medium ray-tracing tier up)

G4 found that ray-driven mode draws primaries by ray and hides the raster material features. A converted shader is a material graph, and graphs have a ray twin, so **its `fragment()` runs on the ray hit**: the same source is included a second time (the `T_` name space) with the library
redirected to a derivative-free flavour (`gdr_*` over `averRtGd*`, which read the bindless table with `SampleGrad` using the hit's footprint or `Load`) and `ddx / ddy / fwidth` read zero, `discard` removed (a compute pass has none). What it therefore does **not** do there:
`vertex()` (the BLAS is the unmoved mesh), `discard` (use `ALPHA_SCISSOR_THRESHOLD`, which the any-hit test honours through the graph), `gdPost` (unshaded, RIM, BACKLIGHT, SPECULAR, ambient / specular mode bits), derivative-based effects, and the `NORMAL` write is the world-anchored frame.
Texture footprints use the mesh UV's gradients scaled by nothing (a shader that multiplies `UV` by 10 samples a blurrier mip than raster does). **Register pressure**: the ray twin is inlined into `rtHitSurface`, which `PSRayDriven` (single pass and split) and the compute stages call;
G4 recorded a first-frame TDR from exactly that kind of growth on the RX 7800 XT. The compile tests prove the text compiles for every one of those entry points, not that the result fits the register file. If a level with converted shaders hangs on the first ray-driven frame, set `RENDER.RTRENDERMODE 0` and tell me which shader.
To see the full SH2 look (vertex stage, discard, rim, unshaded) use `RENDER.RTRENDERMODE 0`, ray tracing off, or Vulkan without ray queries.

### BaseMaterial3D fx in a project that also has converted shaders

Before SH2, registering any material graph compiled out the fx evaluator for every material (`#ifndef AVER_MATERIAL_GRAPH` around it), so BaseMaterial3D pixel features vanished project-wide. With a converted shader registered the renderer defines `AVER_GD_FX_STOCK`, the fx / stock evaluator is
compiled as `averEvalMaterialFx`, and the generated `averEvalMaterial` calls it for a material with no graph (id 0) in the fx variants. A project with a non-Godot graph and no converted shader still has the old behaviour (unchanged on purpose).

### The converter

`ShaderMaterial` as a node's material, a `.tres`, or a sub-resource; the shader as a `.gdshader`, a Shader `.tres` or an inline `Shader` sub-resource (the graph is named after the file, or `<scene>_<sub-resource id>`). `#include` goes through `GodotProjectFs` (`res://`, `uid://`, relative to the including file).
`[shader_globals]` supplies `global uniform` defaults and names (an unknown global is a shader error). A shader error is `unsupported` with `file:line:col: message`; no material is written and the node gets no material. `shader_parameter/*` of every uniform type is converted (Basis -> mat3 columns, Transform3D -> mat4, packed arrays and Arrays for uniform arrays),
a texture parameter is copied (`copyTexture`) or is a ViewportTexture (`@rt:viewport.<key>`) and bound in its sampler's slot, `next_pass` is dropped (reported). Every `GdEngineNote` is a report line (rating, subject, reason).

### Aver.Fabric

`ShaderMaterial` (loaded for an `.ocmat` with a `GRAPHREF`): `SetShaderParameter` packs into the table with the converter's rules and writes it to the engine (found by the `.ocmat`'s NAME, retried when the level loads it; `RefreshEngine()`), a texture goes to its sampler's slot with `aver_pbr_set_texture_path`;
`GetShaderParameter` returns what was set, or reads the table. `Shader` keeps the layout (`GetUniformNames`, `GetSamplerNames`, `SourcePath`; no `Code`: the HLSL is already in the engine). `RenderingServer.GlobalShaderParameterAdd / Set / Get / GetType / Remove` repack the global register of every live material that declares it.
Not done: `get_shader_parameter_list`, `ShaderInclude`, `Shader.get_default_texture_parameter`, per-instance `set_instance_shader_parameter` (instance uniforms are per material), `next_pass`, `ShaderMaterial` made in code before a converted project exists (no layout: `set_shader_parameter` stores the value only).

### Tests

- `GodotShaderMaterialTest` (201 checks, `tests/formats.godot`): the engine's library copy equals the front-end's; the `gdfx` bits; the `GodotShader` node round trip (a 20 KB source with spaces, `#`, `%`, tabs and CRLF); register-by-register defaults for scalar, vector, int, bool, mat3, array, instance and global uniforms
  (`source_color` decoded); sampler slot assignment and mode words; render modes; the refusal messages (four `source_color` textures, a particles shader, a cube sampler, a shader error); `light()` reported; a stock registry generating the code it always did; variant keys; and the engine's translation unit (shared prelude + material prelude + registry text, as `voxiShaderPrelude()` composes it, and
  as ActorPreview does) compiled to DXIL and SPIR-V for the raster pixel stage, the layered BSDF, the fx variant, the mesh-extra twin, and the vertex hook with and without the mesh-extra stream, for: an every-output shader, a `NORMAL` write, `vertex()` in model space / `world_vertex_coords` / `skip_vertex_transform`, a shader that reads every supported built-in, two shaders sharing every global name, and four at once.
- `VoxiSpirvCompileTest` (84 variants): 17 rows with two converted shaders registered, compiled against the real `voxi.hlsl`: the raster scene shader, the fx variant (plain, G-buffer, mesh extra, local lights), RT, the blended pass, `PSRayDriven` single pass and split, `CSRdVisibility / Shadow / Gi / ManyLights`, `VSMainGd`; the pixel and compute rows also to DXIL.
- `FabricShaderTest` (57 checks, `tests/fabric`, fixture `tests/fabric/data/ShaderGame`): five materials (a sub-resource, a `.tres` sharing the water graph, dissolve with `discard` and a mat3, toon with `light()` and an instance uniform, an inline `Shader`), a shader error and a particles shader; register contents, texture slots, FX records, the graph node, `shaders.json`, the `.ocmat` text round trip and the report.
- `Aver.Fabric.Tests` `ShaderTests`: layout parsing, packing of every type, the engine backend (a recording one), globals, and the converter's output loaded through `ResourceLoader`.
- Rerun: `MaterialGraphTest`, `MaterialGraphRegistryTest`, `MaterialFxTest`, `MaterialTest`, `OcMatTerrainTest`, `HairMaterialTest`, `GodotShaderTest`, `FormatsGodotTest`, `GodotMeshTest`, `FabricConvertTest`, `FabricEnvVolTest`, `FabricMaterialFxTest`, `FabricMeshExtraTest`, `ProjectRenderApplyTest`, `AbiEnumTest`, `SeparationTest`.

## Headed checks for the owner

Nothing in G6 has been drawn on a GPU yet. Checks 1 to 8 are what the first GPU run of an emitted shader must confirm, because compile-only tests cannot:

1. Open the water, toon and rim samples (`kShaders` in `GodotShaderTest.cpp`, or write them to `.gdshader` files) once SH2 can bind them. A shader with `ALBEDO = vec3(1.0, 0.0, 0.0)` must be red.
2. Matrix order: a vertex shader doing `VERTEX = (mat4(1.0) * vec4(VERTEX, 1.0)).xyz` must leave the mesh unchanged; one doing `VERTEX.xy = mat2(vec2(0.0, 1.0), vec2(-1.0, 0.0)) * VERTEX.xy`
   must rotate it a quarter turn counter-clockwise in Godot's +Y-up view (a mirrored result means `mul(rhs, lhs)` is wrong somewhere).
3. Uniform packing: set a `vec3` followed by a `float`, a `vec2` after a `float`, a `bool` and an `int`; each must arrive intact (a shifted value means the register table and the CPU writer disagree).
4. Sampler state: a texture with `filter_nearest, repeat_enable` must show hard texels and tile; one with the defaults must be smooth and clamp.
5. `mod(-0.25, 1.0)` must give 0.75 (not -0.25); `fract` of a negative number likewise.
6. A `flat` varying must not interpolate across a triangle; a smooth one must.
7. `discard` must cut the surface (the dissolve sample at `dissolve = 0.5` shows holes); `light()` must replace the lighting (the toon sample shows bands).
8. Vulkan: the same shaders on `--backend vulkan` look the same as on D3D12 (the SPIR-V compiled, but a descriptor-binding mistake only shows on a run).

### SH2

Setup for all of them: `AverAssetC fabric tests/fabric/data/ShaderGame <out>`, open `<out>/ShaderGame` (its level `main` holds seven boxes: Water, Ocean, Dissolve, Toon, Inline, Broken, Sparks), and set `RENDER.RTRENDERMODE 0` first (the raster path is the one with the full look). Then repeat the interesting ones with the default RT mode (check 18).

9. **Smoke.** Inline (a flat blue box: `col` = (0, 0, 1) from the material, not the shader's red default) must be blue; with `shader_parameter/col` removed from the scene it must be red. Broken and Sparks show the engine's default material (no material was written) and `FabricReport.md` says why.
10. **Uniform table, sRGB.** Toon: `color` = (0.1, 0.6, 0.2) is a dark green, not a pale one (the table holds the decoded linear value). Compare against the same colour as a StandardMaterial3D albedo.
11. **Uniform table, packing.** Water vs Ocean (two materials, one graph): different `shallow`, `amplitude`, `flow`, `octaves`, `foam` must show different looks; if `foam` (a bool) or `octaves` (an int) reads wrong, the bit-pattern registers are being flushed or converted by the `RGBA32Float` upload (a denormal int such as 1 or 3).
12. **Textures.** Ocean: `foam_tex` (a `source_color` texture) in slot 0 repeats; `normal_tex` in slot 1 shows normal-map shading. Dissolve: `noise` (`filter_nearest`, `repeat_disable`) shows hard texels and no wrap at the edges, and with `threshold` raised the box dissolves with a hard cut (`discard`).
13. **Sampler emulation.** In a copy of Water set `foam_tex` to `filter_nearest, repeat_enable` and a 4 x 4 texture: hard texels, tiling. With `repeat_disable` and `uv * 3.0`: the edge texels smear (clamp), no tiling.
14. **Vertex stage.** Water must wave (`VERTEX.y` moved by `wave_at`) and its `v_height` varying must change the colour across the surface (smooth, not blocky). Check that the wave is in Godot's up (`y`) = the engine's +Z. A wave along the wrong axis means the model-space basis conversion is wrong. The shadow of the box does not wave (documented), and neither does its GI.
15. **Render modes.** Dissolve: `cull_front` (you see the inside of the box), `blend_add` (it adds to what is behind), `shadows_disabled`, `unshaded` (flat colour, no lighting). Toon with the sun moving: lit by the engine's lights, not banded (`light()` is not run); `RIM` visible at grazing angles.
16. **Globals.** `wind_strength` is 0.75 in `project.godot`: Water's waves are 0.75 of their size. In a script, `RenderingServer.GlobalShaderParameterSet("wind_strength", 0.0)` must flatten the water at once, and a `ShaderMaterial.SetShaderParameter("amplitude", ...)` each frame must animate it with no hitch (one tiny texture upload per change; watch the frame time).
17. **Matrix order and basis.** A shader with `VERTEX = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz` in `world_vertex_coords` mode must leave the mesh where it is; `ALBEDO = NORMAL * 0.5 + 0.5` (view-space normal) must go from (0.5, 0.5, 1) facing the camera to green-ish for a surface facing up and red-ish for one facing right; `ALBEDO = vec3(fract(TIME))` must pulse.
18. **Ray-driven mode.** Default `RENDER.RTRENDERMODE` on the RX 7800 XT: Water, Ocean, Toon, Inline and Dissolve must show their albedo, alpha, roughness and the normal map (the ray twin); they must NOT wave, dissolve by `discard` (the `ALPHA_SCISSOR_THRESHOLD` cut still works), or show rim / unshaded. **Watch the first frame**: if it hangs, that is the register pressure risk in the ray twin (note which shaders were loaded); `RENDER.RTRENDERMODE 0` is the workaround.
19. **BaseMaterial3D beside shaders.** A level with a StandardMaterial3D using `uv1_scale` or `rim` next to the Water box: its fx must still work (the `AVER_GD_FX_STOCK` fallback). Before SH2 it would have vanished.
20. **Vulkan.** Repeat 9 to 17 on `--backend vulkan`: a descriptor-binding mistake in the new table texture or a vertex-stage resource read shows only here.
21. **Editor.** Open `Shaders/water.ocgraph` in the node editor: it shows one `GodotShader` node (unknown to the palette); saving must not corrupt it (the attributes are plain tokens). If the editor refuses the file, tell me how.

---

## SH3: sky, fog and particles shaders, VisualShader

Status: engine, converter, `Aver.Fabric` and headless tests done. Nothing here has run on a GPU, in the editor or in the game (the engine-run ban). The generated HLSL compiles to DXIL and SPIR-V inside the engine's real translation units (Voxi with the sky spliced in, the froxel fog passes, the GPU particle simulation) and the converter's output reads back through the engine's loaders, which proves the plumbing and the text, not the picture. Read the SH3 headed checks (end of this file) before trusting a pixel.

### The shape of it

```
sky / fog / particles .gdshader --gdShaderForEnv--> GdEnvShader --FabricShader.cpp--> Content/Shaders/<name>.<kind>.ocgraph   (DOMAIN godotenv, one GodotEnvShader node)
the ShaderMaterial's shader_parameter/* -----------> baked into the node's register table (reg=)                               Fabric/shaders.json (kind + layout)
VisualShader .tres --gdVisualShaderToCode--> shading-language text --(the same paths)--> Fabric/visualshaders/<name>.gdshader.txt (VisualShader.GetCode)
```

A `ShaderMaterial` whose shader is a sky, fog or particles shader is converted only where one of those can use it (`w.type` is `WorldEnvironment` / `Environment` / `Sky` for the sky, `FogVolume`, `GPUParticles3D`); on a mesh it is still refused, now with a message saying where it can go. The graph's domain is `godotenv`, which the gameplay and material sweeps skip (`ocGraphDomainOf` says Unknown). Code: `modules/formats.godot` `GodotShaderEnv.{hpp,cpp}` (`gdShaderForEnv`) and `GodotVisualShader.{hpp,cpp}` (`gdVisualShaderToCode`); `modules/formats` `OcGraphGodot` gained `GodotEnvPart` / `packGodotEnvShader` / `unpackGodotEnvShader`.

### Uniforms, textures and the shared rules

- At most 16 registers (the normal block, then the global block), SH1's std140-style layout, `source_color` decoded linear, `int` / `bool` as raw words. Instance uniforms are refused (Godot allows them in spatial shaders only).
- Sampler uniforms are **unbound**: these shader types have no material texture slots, so every `texture()` reads the colour its hint names (`hint_default_black`, `hint_default_transparent`, `hint_normal`, else white) and the report says `approximated` per uniform. Only `sampler2D` is accepted (a cube or 3D sampler is an error). Sampling `RADIANCE` in a sky shader is an error (the engine renders the sky shader directly; there is no radiance cubemap).
- Derivatives read zero (`ddx`, `ddy`, `fwidth` are redirected by macro): the sky is evaluated in compute and ray-miss code too.
- The SH1 helper library is copied at compose time with its names renamed (`gd_*` to `gde_*`, `Gd*` to `GdE*`, `gdSlot*` to `gdeSlot*`, `GD_SMP` to `GDE_SMP`; `aver::rhi::godotEnvRename`), so a sky or fog shader can share a translation unit with a converted spatial shader (tested: `Voxi PSMainVoxi raster, sky + spatial Godot shader`). The `AVER_GD_*` hook macros are defined for the shader and lifted again after it.
- Files: `modules/rhi/shaders/godot_env_common.hlsl`, `godot_sky.hlsl`, `godot_fog.hlsl`, `godot_particles.hlsl`; composition in `modules/rhi/include/aver/rhi/GodotEnvPrograms.hpp` and `src/GodotEnvPrograms.cpp` (`godotSkyProgram`, `godotFogProgram`, `godotParticlesProgram`, `spliceGodotSky`). They read the shipped files with `shaderFile()`, build strings and touch no device.

### Sky

`shared_prelude.hlsl` carries a marker line (`//@AVER_GD_SKY_SPLICE@`) before `skyColorFull` and `#if AVER_GD_SKY` branches in `skyColorFull`, `skyColor`, `averSkyRadianceCheap` and `averSkyIrradiance`; every one of those hunks is inside an `#if`, so without the define the file means what it meant. `VoxiRenderer::setGodotSky(program)` composes the program; `voxiShaderPrelude()` then prepends `#define AVER_GD_SKY 1` and splices the program at the marker (the sky revision is part of its cache key), and the next `prePass` rebuilds the scene pipelines as it does for a new material graph. With a Godot sky the background (`PSGodotSky`, a fullscreen pixel shader drawn in `transparentPass` over the pixels still at the far plane, before the froxel sky; ray-driven mode paints its own misses through `skyColorFull`), reflections and every ray miss (`skyColor`, `averSkyRadianceCheap`) and ambient light (`averGdSkyIrradiance`: the sky at the normal and at four directions 60 degrees off it, weights 0.4 and 4 x 0.15; `AVER_GD_SKY_AMBIENT_TAPS` 1 evaluates the normal alone) read the shader. The physical atmosphere's SH, the fog's in-scatter reference and the cloud pass keep using the engine's own sky.

Built-ins: `EYEDIR`, `POSITION` and the lights are in Godot's world axes and metres (a direction is `(e.y, e.z, -e.x)`); `TIME` is `gTime.x` (wraps at 3600 like Godot's default); `LIGHT0_*` is the sun (`LIGHT0_ENERGY` = the sun's intensity / 104719.755, the calibration G4 R2 uses for lights; `LIGHT0_SIZE` the sun's angular diameter in radians); `LIGHT1..3_ENABLED` are false; `AT_CUBEMAP_PASS` is true for reflections and ambient light, false for the background; `AT_HALF_RES_PASS` / `AT_QUARTER_RES_PASS` are false and `HALF_RES_COLOR` / `QUARTER_RES_COLOR` zero; `SCREEN_UV` and `SKY_COORDS` are the pixel in the background pass and (0.5, 0.5) elsewhere. `FOG` blends over `COLOR` (`mix(COLOR, FOG.rgb, FOG.a)`); `ALPHA` is ignored.
**Uniform values are baked into the sky program** (`static const` register arrays): a changed parameter recomposes the program and rebuilds the scene pipelines (a hitch; do not animate a sky uniform every frame; `TIME` is free).

### Fog

`FogVolumeDesc::shader` (1-based index into the frame's fog-shader table) travels in the packed block's shape word (low 3 bits the shape, the index above: `fogVolumeShapeOf` / `fogVolumeShaderOf`; no shader = today's word); a shader volume carries its centre (world cm) and local half extents (cm) in the albedo and emission slots (the shader supplies those). The `AVER_FOG_SHADERS` variants of the `AVER_FOG_VOLUMES` passes in `voxi_froxel.hlsl` call `averGdFogEval` for such a volume: the density is the shader's `DENSITY` per metre times the edge fade (the height falloff is a FogMaterial property and does not apply), `ALBEDO` scatters, `EMISSION` adds. At most `kGodotFogShaderMax` (4) fog shaders with 16 registers each ride a tail of `FroxelCB` (`gFvGd`), so a changed value needs no rebuild; a changed set of shaders rebuilds the volume variants (`VoxiFroxelFog::setFogShaders` / `setFogShaderRegisters`). Built-ins: `WORLD_POSITION`, `OBJECT_POSITION` (the volume's centre), `SIZE` (the node's `size`), `UVW` (0..1 across the volume, Godot axes), `SDF` (reads 1000: Aver has no scene SDF), `TIME`. Godot's fog shader has no `EXTENTS` (the request named one); `SIZE` is what the docs list.

### Particles

`ParticleEffect::godotShader` (the shader's name; `.ocparticle` record `GDSHADER <name>`, a version 2 record) and the GPU tier: an emitter that names a registered program gets its own `CSSpawn` / `CSUpdate` pipelines (`particles_sim.hlsl` with `AVER_GD_PARTICLES=1`, the program as the prelude; `ParticleGpu::gdKernelsFor`), built when the first instance is acquired. **start() runs at birth and process() replaces the update**: Godot's `process()` does the integrating (`TRANSFORM[3].xyz += VELOCITY * DELTA`), so for such an emitter the built-in gravity, damping, modules and position step are skipped; `ACTIVE = false` kills the particle (a death event fires). State: position, velocity, age and lifetime stay in the 64-byte `Particle`; `COLOR`, `CUSTOM`, `MASS` and the transform's basis live in the instance's state buffer (five float4 a slot; it reuses the trail buffer, `kGodotParticleStateVecs`); the draw reads colour (8 bits a channel, clamped to 0..1), a uniform scale (the mean basis length) and a spin about the view axis (`atan2` of the basis' first column) from the particle. Built-ins: `TIME`, `DELTA`, `LIFETIME`, `NUMBER` (the particle's id), `INDEX` (its slot), `RANDOM_SEED` (its seed), `EMISSION_TRANSFORM` (the spawn transform in `start()`, identity in `process()`), `RESTART` (the first update), `RESTART_*` and `FLAG_EMIT_*` true; `USERDATA1..6`, `EMITTER_VELOCITY`, `COLLIDED`, `COLLISION_*`, `ATTRACTOR_FORCE` and `INTERPOLATE_TO_END` read zero and `AMOUNT_RATIO` one (all reported). `emit_subparticle()` does not exist (a call is a shader error). The uniforms are the effect's **user parameters**: register `i` is `PARAM reg<i> color` (slot `i` of the 16), so `Entity.SetParticleColor("reg1", ...)` (what `GPUParticles3D` does on a parameter change) reaches a running emitter with no rebuild; the program is registered with the framework when the effect loads (`GameContent::loadProjectParticleEffects` reads `Shaders/<name>.particles.ocgraph`). An emitter that lands on the CPU tier (no compute or indirect support) simulates **without** the shader (the converter forces `SIM gpu`; the CPU executor cannot run HLSL). Because the shader owns the motion, the converted emitter's authored speed and gravity are zero and its shape a point.

### Converter

`GPUParticles3D` is now an `Aver.Fabric` class. With a particles `ShaderMaterial` as its `process_material` the placement gets `COMPONENT ParticleEmitter effect=Particles/<project>_<node>.ocparticle playing=<emitting> sim=gpu` and the effect file is written (rate `amount / lifetime`, or one burst of `amount` for `one_shot`; sprite size from a `QuadMesh` draw pass, else 1 m; the `reg<i>` parameters at the material's values). A `ParticleProcessMaterial` is **not** converted (reported unsupported on `process_material`; the node keeps its properties and emits nothing): that mapping did not exist before SH3 either, so nothing regressed. `explosiveness`, `randomness`, `speed_scale`, `local_coords`, `visibility_aabb` and `draw_order` are reported approximated, `preprocess`, `trail_*` and `sub_emitter` unsupported. The sky reports `Environment/sky_material` approximated (the level's own sky record stays the physical sky, for what the shader does not replace); a `FogVolume.material` that is a fog `ShaderMaterial` is converted.

### VisualShader

`gdVisualShaderToCode` reads a `.tres` `VisualShader` (spatial `vertex` / `fragment` / `light`, sky, fog) and generates text the way Godot's generator lays it out: nodes in dependency order from the output node (id 0) back, unconnected inputs from `default_input_values` or the node's own default, Godot's automatic port conversions (scalar to vector `vecN(x)`, vector to scalar the mean, vec2 to vec3 `vec3(x, 0.0)`, ...), `modes/*` and `flags/*` as `render_mode`. The output ports and their order are Godot 4.4's (`VisualShaderNodeOutput::ports`, read from its source). Nodes: `Input` (any built-in of the shader type), the constants (float, int, uint, bool, colour with alpha, vec2/3/4, transform), the parameters (float, int, uint, bool, vec2/3/4, colour, transform, texture; hints, global / instance qualifiers), `Texture` (`texture()`; an embedded texture is not bound: approximated), `FloatOp`, `IntOp`, `UIntOp`, `VectorOp`, `ColorOp`, `FloatFunc`, `IntFunc`, `VectorFunc`, `ColorFunc` (grayscale, sepia), `VectorCompose` / `Decompose`, `VectorLen`, `VectorDistance`, `DotProduct`, `Mix`, `Clamp`, `Step`, `SmoothStep`, `Fresnel`, `Compare`, `If`, `Switch`, `Remap`, `UVFunc`, the transform nodes (`TransformVecMult`, `TransformFunc`, `TransformOp`, `TransformCompose` / `Decompose`), `Reroute`, `Expression` (with its ports) and `GlobalExpression`. Everything else (curve and noise textures, derivatives, varyings, script nodes, the HSV functions, particle emission nodes, 3D texture sources) is reported `unsupported` per node (`<function>/<id> <class>`) and reads as unconnected. A `mode = 2` (particles) VisualShader is refused (its graphs end in `VisualShaderNodeParticle*` nodes); `mode = 1` is 2D. The enum orders of the operator and function nodes are Godot 4's as recalled and checked only by the fixtures; an inline `VisualShader` sub-resource of a scene is not read (save it as a `.tres`).

### Aver.Fabric

`ShaderMaterial` loads a godotenv `.ocgraph` (`ResourceLoader` maps `.ocgraph` to it; `EnvironmentKind` is "sky" / "fog" / "particles" and `Shader.GetMode()` Sky / Fog / Particles); `set_shader_parameter` repacks the register table as SH2 does and pushes it (`GodotEnv.SetRegisters`; a particles material tells its `GPUParticles3D`). `WorldEnvironment` registers a sky shader and makes it the sky once the environment is in the tree and `Sky.SkyMaterial` (now a `Material`) is one (`GodotEnv.UseSky`), `FogVolume` names its fog shader, `GPUParticles3D` (`Emitting`, `Amount`, `Lifetime`, `OneShot`, `ProcessMaterial`, ...) pushes the registers into the emitter's `reg<i>` user parameters, and `VisualShader : Shader` has `GetCode()` (the converter's text from `Fabric/visualshaders`). `Aver.Framework`: `GodotEnv` (`Register`, `SetRegisters`, `UseSky`, `Unregister`, `Count`) and `EnvFx.SetFogVolumeShader`; the C ABI is `framework_godotenv_abi.h` (version 1; the registry parses the graph and hands its text back to the host) and `aver_fw_fogvol_set_shader`. The hosts' `EnvFxFeed` calls `GodotEnvFeed::update` every frame (`Runtime/include/aver/game/GodotEnvFeed.hpp`): sky and fog shaders to Voxi, particles shaders to `particles::setGodotParticlePrograms`.

### Not done / approximated

- No texture binding in any of the three shader types (hint defaults); `RADIANCE`; `emit_subparticle`, particle collision and attractors; `SDF` in fog; instance uniforms; `canvas_item`.
- The CPU particle tier cannot run a shader, and there is no CPU interpreter of the language, so a shader's maths is checked only by DXC compiling it (a wrong sign or axis shows only on a GPU run).
- A sky uniform change rebuilds the scene pipelines. A sky shader's cost is paid at the background, at every reflection and at every ambient lookup (five times), and in ray-driven mode inside `PSRayDriven` and the compute stages (**register pressure**: see the SH2 note on the first-frame TDR; if a level hangs on its first frame with a Godot sky, set `RENDER.RTRENDERMODE 0` or simplify the sky, and tell me).
- `ParticleGpuParityTest` (needs a device) was not run and does not cover the Godot kernels.

### Tests

- `GodotShaderEnvTest` (125 checks, `tests/formats.godot`): preparation of the three shader types (registers, `source_color` decode, int / bool words, unbound samplers, the notes), refusals, the `GodotEnvShader` node round trip (denormal int words, odd text), the library rename, the splice marker, the fog shape word; the VisualShader fixtures (`tests/fabric/data/EnvShaderGame/visual`: spatial with an unknown node and an embedded texture, sky, fog) converted and fed back through `gdShaderForEngine` / `gdShaderForEnv`; and the engine's translation units compiled with DXC to DXIL and SPIR-V: `PSGodotSky`, `PSMainVoxi` raster and RT, `PSRayDriven` single pass and split, `CSRdGi`, `CSRdManyLights`, `PSVoxelDebug` and `VSky` with a sky, the same beside a spatial Godot shader, the froxel scatter (plain and RT) and integrate with one and two fog shaders, `CSSpawn` / `CSUpdate` with a start + process shader, start only and process only, and the default twins of every sky, fog and particles entry (54 compiles). `VoxiSpirvCompileTest` (84 variants) is unchanged and passes.
- `FabricEnvShaderTest` (41, `tests/fabric`): the converter on `EnvShaderGame`; graphs, registers at the material's values, the `.ocparticle` read back by the engine's parser, the ParticleEmitter component, the manifest, the VisualShader text and material, the report.
- `FrameworkGodotEnvAbiTest`: the registry through the DLL (refusals, byte-exact graph text, registers revision vs program revision, the sky, the fog shader index in the packed block, reset). `AbiEnumTest` pins `GodotEnvKind` against `AVER_FW_GENV_*`. `OcParticleTest`: `GDSHADER` round trip; an effect without a shader stays version 1.
- `Aver.Fabric.Tests` `EnvShaderTests` (4618 checks in all): the converter's output instantiated; the sky, fog and particles shaders registered through recording backends and parameters reaching them; `VisualShader.GetCode`; refusals.
- Rerun: `GodotShaderTest`, `GodotShaderMaterialTest`, `FabricShaderTest`, `FabricConvertTest`, `FabricEnvVolTest`, `EnvFxTest`, `FrameworkEnvFxAbiTest`, `ParticleSystemTest`, `EmitterSimTest`, `ParticleModulesTest`, `SeparationTest`, `AbiEnumTest`, `FabricManagedTests` (all of ctest's Fabric / shader / particle / env rows), `Aver.Framework.Tests`.

### Headed checks for the owner (SH3)

Setup: `AverAssetC fabric tests/fabric/data/EnvShaderGame <out>` and open `<out>/EnvShaderGame` (`main` has a sky shader, a fog volume `Mist` with a fog shader, a `Sparks` emitter with a particles shader, a `Plain` emitter with a `ParticleProcessMaterial`, a `RimBox` with a VisualShader and a `WrongBox`). `RENDER.VOLFOG 1` (the converter writes it when the environment enables volumetric fog).

22. **Sky, raster.** `RENDER.RTRENDERMODE 0`: the background is the gradient (blue overhead, pale at the horizon) with a sun disc where the sun is and no stars (`show_stars` is false in the material). Set `show_stars` true from a script (`SetShaderParameter`): stars appear in the background only, not in reflections (`AT_CUBEMAP_PASS`). A pause of a frame or two while the pipelines rebuild is expected.
23. **Sky, ambient and reflections.** The same sky tints the ambient light on a white box (bluer from above) and shows in a mirror sphere; compare with `GodotEnv.UseSky(null)` (the engine's own sky). Sun and ambient brightness are a calibration to check by eye.
24. **Sky, ray-driven.** Default `RENDER.RTRENDERMODE` on the RX 7800 XT: the background matches 22. **Watch the first frame** (register pressure in `PSRayDriven`); `RENDER.RTRENDERMODE 0` is the workaround, tell me.
25. **Sky axes.** A sky with `COLOR = vec3(EYEDIR.y)` is bright overhead and dark at the horizon (Godot's +Y up); `COLOR = EYEDIR * 0.5 + 0.5` shows the colour of each direction in Godot's axes (green up, red to the right of the start camera's view, blue behind it, since Godot forward is -z).
26. **Fog volume.** `Mist` shows haze that thins toward the top of the box (`1 - UVW.y`) with a wave along Godot's x (`sin(WORLD_POSITION.x * 0.5 + TIME)`) and an orange glow at one end (`smoothstep(0.8, 1.0, UVW.x)`: note which end it is). `SetShaderParameter("density", 0.0)` makes it vanish at once with no hitch. Two volumes with different shaders, and one with a plain `FogMaterial`, each look their own.
27. **Particles.** `Sparks` emits 64 particles over 2 s from the node; they start upward (`emit_dir` (0, 2, 0) times 1 to 2), fall under the shader's gravity and drag, spin the transform, vary in colour with the seed and die below y = -5 m. `SetShaderParameter("spin", 8.0)` and `"emit_dir"` change a running emitter. The sprite colour is 8 bits clamped; a shader that sets the TRANSFORM scale must change the size. The motion is along Godot's axes (up is the engine's +Z).
28. **Particle tier.** `RENDER.PARTICLEGPU 0` (CPU tier) shows the particles *without* the shader's motion (a still burst), as documented; `Plain` emits nothing.
29. **VisualShader.** `RimBox` is unshaded with cull disabled and shows a blue to orange rim (Fresnel power `rim_power` 5) whose alpha pulses; the Curve node's output reads as unconnected.
30. **Vulkan.** Repeat 22 to 27 on `--backend vulkan`.
