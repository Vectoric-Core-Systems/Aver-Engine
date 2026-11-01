# Godot feature parity plan

Status: plan, nothing implemented. Branch 08-gdint. Written 2026-10-31 from the classified gaps of five domain reviews (rendering, physics and geometry, animation and skeleton, UI and text and input, scripting and core and audio).

Paths in this file are relative to the engine root unless they start with a drive letter. Fabric is the Fabric Connector extension, in its own repository ([Fabric-Connector](https://github.com/Vectoric-Core-Systems/Fabric-Connector); local clone `C:/Users/User/AverBuilds/Fabric-Connector`), built against this engine ([FABRIC.md](../FABRIC.md)). Its plans (G3 to G6) are in that repository's `docs/plans`. The parity data is in `C:/Users/User/AverBuilds/parity`.

## 1. Scope and principles

1. Scope is Godot 4 3D and UI (Control) features. 2D (CanvasItem drawing beyond what UI needs, TileMap, 2D physics, 2D lights) is out.
2. Engine features are added under neutral names in engine core (`modules/`, `Runtime/`, `scripting/csharp/Aver.Framework`). Fabric maps Godot's API onto them. No Godot names appear in engine core.
3. Aver keeps its own renderer and Jolt. Pixel-identical rendering is not a goal. Godot's own physics engine is not ported. A feature counts as a gap when it is absent or unreachable, not when it only looks different.
4. WebP is decoded by a vendored libwebp (decoder only, BSD-3). The owner has approved this one dependency. Every other new dependency is an owner decision (section 6). Vendoring is done in the main session, not by a subagent.
5. Every wave keeps defaults unchanged. A new setting defaults to today's behaviour. Ordinary shader permutations stay byte-identical. `PSRayDriven` and the RT shadow code are not touched unless the owner lifts that rule (G4).
6. Every engine change keeps D3D12 and Vulkan in step (standing rule). ABI changes bump the ABI version and update the enum/row tests.
7. Verification is headless: build, `ctest` with the GPU device suites excluded (`ctest -E`, the list is in the no-engine-run memory), `tests/formats`, in the Fabric repository its managed tests (`Aver.Fabric.Tests`) and `tests/fabric` (its `ctest`), and a re-conversion of the 32 demos with a diff of `gaps.json` and the script compile errors. Nothing starts the editor, the game or a GPU test while the no-engine rule holds. Each wave lists headed checks for the owner.
8. Counts of demos blocked overlap between rows. Do not add them up.
9. Sizes are rough and assume headless work only: S is about a day or less, M a few days, L a week or more, XL more than that.

Claims in the gap tables come from the domain reviews, which read the code. The ones marked "checked" below I re-read for this plan:
- `modules/platform/src/Image.cpp` decodes with stb_image only; `undecodableContainer` names WebP, EXR and others as refused. `third_party` has dr_libs, stb, ufbx, meshoptimizer, fonts, imgui and others, no libwebp.
- `CMeshRenderer` (`modules/scene/include/aver/scene/Components.hpp:64`) holds one `i32 material`, and `visFade` is stored with the comment "the engine hard-cuts all three".
- Every material samples through one sampler, `gMaterialSampler` (`modules/render.pbr/shaders/material_prelude.hlsl:17`).
- The framework key enum ends at `AVER_FW_KEY_COUNT`, 50 slots (`modules/framework/include/aver/framework/framework_abi.h`).
- The physics catch-up cap is a constant, `fixedStep * 8` (`modules/physics/src/PhysicsWorld.cpp:448`).
- `rhi::TextureDim` is `Tex2D` and `Tex3D` only; the cascade count is a fixed 4 (`VoxiRenderer.cpp:39`).

Everything else is as reported by the reviews. Where a review said it could not check, this plan says "unverified".

## 2. Where we are

All 32 Godot 3D demos in the corpus were converted with the Fabric converter (`C:/Users/User/AverBuilds/parity`).

| Measure | Value |
|---|---|
| Demos converted | 32 |
| Non-converted items in `gaps.json` | 862 rows, 3872 occurrences |
| Rated unsupported / approximated | 490 / 372 |
| Demos whose converted scripts compile clean | 7 (csg, kinematic_character, particles, platformer, rigidbody_character, squash_the_creeps, truck_town) |
| Demos with at least one compile error | 25 by the logs (`script-errors.txt` counts 24) |
| Script compile errors, total | 557 |
| Most common error | `using System` clash with a script class named System, 32 files (one cascade, 73 errors in physics_tests) |
| Godot classes in scope (`classes.json`) | 798 |
| Classes with a C# type in Aver.Fabric | 318 |
| Classes missing | 480 (3 are casing false negatives: AudioStreamWAV, MultiplayerAPI, EncodedObjectAsID) |

Class coverage by base:

| Base | In scope | In Fabric |
|---|---|---|
| Resource | 386 | 129 |
| RefCounted | 148 | 40 |
| Node3D | 110 | 57 |
| Object | 65 | 20 |
| Control | 62 | 53 |
| Node | 27 | 19 |

Missing classes are mostly XR, RenderingDevice, GDExtension, VisualShader nodes and importer classes (section 5), plus a long tail with no demo use.

Largest single blockers by demos: WebP textures (31), shadow cascade and filter settings (26, visual only), viewport rendering properties (24), per-material sampler state (22), per-surface material overrides (19), key set (13), built-in `ui_*` actions (12), procedural texture resources (12), cast_shadow/gi_mode (12).

## 3. The gap table

184 items after merging duplicates across the five reviews. Per category:

| Category | Items |
|---|---|
| engine-missing (E) | 25 |
| engine-partial (P) | 49 |
| fabric-mapping (F) | 75 |
| converter-bug (C) | 20 |
| not-a-goal (N) | 15 |

Columns: gap, the Godot API involved, the neutral engine feature (or the existing engine feature the mapping targets), size, demos blocked. "n/a" in the engine column means no engine work. A demo count of 0 means no corpus demo needs it; the item is in the plan for API completeness only.

### 3.1 engine-missing (25)

| ID | Gap | Godot API | Neutral engine feature | Size | Demos |
|---|---|---|---|---|---|
| E1 | WebP not decoded (103 rows; real textures plus 26 icon.webp) | Texture2D from .webp, Sprite3D/Decal/material slots | WebP decode (lossy, lossless, alpha) in the platform image loader through vendored libwebp, also used by the converter | M | 31 |
| E2 | SVG textures | .svg imported as texture | SVG rasterisation at import | M | 7 |
| E3 | OpenEXR images | .exr textures | EXR float image decode | S | 3 |
| E4 | Sampled panorama sky for background, reflections and ambient | PanoramaSkyMaterial, Sky.radiance_size, sky_rotation | Sky texture slot sampled by the sky pass and sky lookups, with rotation and a blurred chain | L | 2 |
| E5 | Cube and texture-array dimensions | Cubemap, CubemapArray, Texture2DArray, samplerCube | RHI cube and 2D-array texture dimensions, creation, upload, sampling, mips | L | 1 |
| E6 | World-space text | Label3D | Glyph-quad (or texture) text drawable with billboard, fixed size, outline, shading, depth options | L | 5 |
| E7 | Output debanding | Viewport.use_debanding, project setting | Final-composite dither before 8-bit quantisation, switchable | S | 14 |
| E8 | Runtime texture from CPU pixels | Image, ImageTexture, set_pixel, create_from_image | Runtime texture object with update, mips, usable in material slots, shaders and UI | L | 2 |
| E9 | Line and point topology for scene meshes | ImmediateMesh/ArrayMesh PRIMITIVE_LINES, LINE_STRIP, POINTS, point_size | Line/point topology (or thin-quad emulation) in raster and RT geometry | M | 3 |
| E10 | Stencil buffer and stencil material modes | BaseMaterial3D.stencil_* | D32S8 depth format with per-material stencil state | L | 1 |
| E11 | Material next_pass | BaseMaterial3D.next_pass | Second draw of a mesh with another material, kept out of RT geometry | M | 2 |
| E12 | Frame-rate cap | Engine.max_fps | Frame limiter in the game loop, project key and runtime setter | S | 2 |
| E13 | Extruded 3D text mesh | TextMesh | Glyph outline flatten, triangulate with holes, extrude | M | 1 |
| E14 | SDF/MSDF glyphs and mipmapped glyph pages | FontFile.multichannel_signed_distance_field, generate_mipmaps | SDF atlas page format and shader, optional mip chain | M | 2 |
| E15 | OpenType GSUB/GPOS shaping, variable fonts, complex scripts | TextServerAdvanced features, FontVariation.variation_opentype | Shaping stage beyond kerning and Arabic joining | XL | 0 |
| E16 | Typed characters, key repeat, IME, OS clipboard | InputEventKey.unicode/echo, LineEdit typing, DisplayServer.ime_*, clipboard | Text-input and composition events and a clipboard seam in the platform window layer | M | 1 |
| E17 | canvas_item shaders and materials on UI | shader_type canvas_item, ShaderMaterial on Control | Custom fragment pipeline per UI draw | L | 1 |
| E18 | UI/2D content inside a SubViewport, GUI input into a viewport | SubViewport with Controls, SubViewportContainer | Render a UI draw list to an offscreen target with remapped pointer events | L | 1 |
| E19 | Video playback | VideoStreamPlayer, VideoStreamTheora | Video decode to a streaming texture with synced audio (only an encoder exists in `modules/media`) | L | 0 |
| E20 | Native OS menus and tray icon | NativeMenu, StatusIndicator | Platform seam for OS menu and status icon | M | 0 |
| E21 | Touch input | InputEventScreenTouch/Drag, TouchScreenButton | Multi-touch events from the window layer, mouse-to-touch emulation | M | 2 |
| E22 | CSG polygon swept along a path | CSGPolygon3D mode Path, path_* | Sweep a 2D polygon along a curve in `aver/formats/Csg.hpp` | M | 4 |
| E23 | COLLADA import | .dae scenes with skeleton and skin | COLLADA importer beside glTF/FBX/OBJ | L | 1 |
| E24 | Spring bone chains | SpringBoneSimulator3D, SpringBoneCollision3D | Bone-chain Verlet/spring solver with collision volumes | L | 0 |
| E25 | Audio input, band-limit filter, ADPCM/QOA data | AudioEffectRecord, AudioStreamMicrophone, AudioEffectBandLimitFilter | Capture path, filter, decoders | M | 0 |

### 3.2 engine-partial (49)

| ID | Gap | Godot API | Neutral engine feature | Size | Demos |
|---|---|---|---|---|---|
| P1 | One static sampler for all materials | texture_filter, texture_repeat, default anisotropy and mip bias | Selectable material samplers (point/linear, mip, wrap/clamp/mirror) in the .ocmat FX record, project anisotropy and LOD bias. Unverified: which sampler the bindless fetch of RT hits uses | M | 22 |
| P2 | No per-surface material override on an instance | surface_material_override/N, set_surface_override_material | Per-entity override list indexed by mesh material slot, riding on the existing per-material part split | M | 19 |
| P3 | Sun cascade controls fixed (4 cascades, 2048, lambda 0.85) | directional_shadow_mode, splits, max_distance, fade_start, blend_splits, atlas size | Cascade count, split ratios, max distance, fade, blending, resolution per sun and per project. Only affects raster/cascade fallback when RT shadows are on | M | 26 |
| P4 | Fixed 3x3 PCF, fixed 4096 local atlas, 8 shadowed lights | soft_shadow_filter_quality, positional atlas size, shadow_blur | Shadow filter quality setting and configurable local atlas size | M | 26 |
| P5 | No per-instance shadow/GI flags | cast_shadow, gi_mode, extra_cull_margin, ignore_occlusion_culling | Per-renderer flags: no shadow, shadows-only, GI mode, cull margin, occlusion opt-out. The converter drops these silently today (C20) | M | 12 |
| P6 | Volumetric fog scatters the sun only | light_volumetric_fog_energy, volumetric_fog_albedo/emission/gi_inject/ambient_inject | Local-light in-scatter and medium albedo/emission in the froxel pass | M | 2 |
| P7 | One sun only; no sky_mode | DirectionalLight3D.sky_mode, extra directional lights | Second directional light and separate lighting/disc contribution. Unverified depth of the one-sun assumption in ReSTIR/NRD2 | M | 1 |
| P8 | Ambient is a sky multiplier only | ambient_light_source/color/sky_contribution, reflected_light_source | Ambient colour and energy override, sky blend, reflection source | M | 9 |
| P9 | Fog extras missing | fog_sun_scatter, fog_sky_affect, fog_aerial_perspective, fog_light_energy | Sun in-scatter share, fog on sky pixels, aerial perspective strength (gated, default off) | S | 7 |
| P10 | Glow has intensity and threshold only | glow_levels, strength, bloom, blend_mode, hdr_scale, luminance cap | Per-level weights, strength, blend modes, HDR scale/cap as composite variants | M | 10 |
| P11 | Fog volume density texture not sampled | FogMaterial.density_texture | 3D texture sampled in the froxel volume pass | S | 1 |
| P12 | ReflectionProbe has data and math but no capture | ReflectionProbe update_mode, box_projection, interior | Six-view capture into a prefiltered map and probe sampling in the scene shader (needs E5 or an octahedral atlas) | L | 6 |
| P13 | Decal lacks emission, albedo mix, upper/lower/normal fade, modulate, layer mask | Decal.texture_emission, albedo_mix, upper_fade, lower_fade, normal_fade, modulate, cull_mask | Extend CDecal, the 192-byte voxi decal record and its HLSL mirror | S | 1 |
| P14 | Script changes to BaseMaterial3D are stored, not pushed | albedo_color, textures, emission, uv1_offset at run time | Per-instance material parameters from scripts (constant block or on-demand variants) | M | 3 |
| P15 | Particle emission/motion beyond the module set | emission_shape ring/points, radial/tangential/orbit accel, hue variation, box/heightfield/SDF collision, vector-field attractors | New emitter shapes and modules, each with CPU and GPU executors (pinned by ParticleGpuParityTest) | L | 3 |
| P16 | Particle trails as meshes | trail_enabled, TubeTrailMesh, RibbonTrailMesh | Mesh draw bent along trail history; ribbons are a stand-in | M | 1 |
| P17 | No script ABI for render settings | Viewport scaling_3d_scale/mode, fsr_sharpness, use_taa, mesh_lod_threshold, use_occlusion_culling | C ABI and C# wrapper for render scale, upscaler, sharpness, TAA, LOD threshold, occlusion, shadow settings. Applies at a frame boundary | S | 8 |
| P18 | No per-frame render statistics for scripts | RenderingServer.get_rendering_info | Objects, triangles, draw calls of the last frame, correct in ray-driven mode | S | 1 |
| P19 | Orthographic view skips the ray-driven pass | Camera3D.projection orthogonal | Orthographic primary rays, sky, fog, reflections, denoiser basis | M | 2 |
| P20 | Visibility range hard-cuts | visibility_range_fade_mode, visibility_parent | Dithered/alpha fade over the range margin using the existing FX dither (per-instance fade constant needed) | S | 3 |
| P21 | No proximity fade | proximity_fade_enabled/distance | Depth bound as read-only SRV in blended draws, both backends | M | 3 |
| P22 | SubViewport draws a cheap raster view | SubViewport shadows, GI, post | Improve the cheap path only; a second Voxi is blocked by renderer singletons | L | 1 |
| P23 | No script-visible skeleton pose seam | Skeleton3D.set_bone_pose_*, get_bone_global_pose, bone tracks, BoneAttachment3D | Script-driven pose-modifier layer on the engine skeleton (read and write bone poses, attach to any bone) in the existing `PoseModifierFn` chain, plus a framework ABI | L | 3 |
| P24 | Ragdoll has capsule + swing/twist only, one shared mass | PhysicalBone3D shapes, mass, friction, bounce, joint types | Per-bone shape, offset, mass, material, joint kind in the ragdoll builder | M | 0 |
| P25 | Retargeting is clip based | RetargetModifier3D, BoneMap, SkeletonProfile | Pose-to-pose runtime retarget through a bone map | M | 0 |
| P26 | No animation sections or markers | play_section*, add_marker | Named time markers and sectioned playback in the property-track player | S | 0 |
| P27 | No per-body inertia or centre-of-mass override | center_of_mass_mode, inertia | Body-level mass-property override that survives set_mass | S | 0 |
| P28 | Interpolation only for dynamic rigid bodies | physics_interpolation_mode, get_global_transform_interpolated, reset_physics_interpolation | Fixed-step interpolation for any entity, per-entity opt-out, runtime switch, interpolated-pose query | M | 2 |
| P29 | Fixed-step catch-up cap is a constant (8) | Engine.max_physics_steps_per_frame | Configurable cap as project key and setter (checked) | S | 1 |
| P30 | No triangle index on ray hits | intersect_ray face_index | Triangle index in the raycast result | S | 0 |
| P31 | Area3D gaps | body_shape_entered, gravity override on characters, wind on soft bodies | Per-shape events, area gravity/damp for kinematic controllers, area wind on soft bodies | M | 0 |
| P32 | Soft body scene API lacks per-vertex access | set_point_pinned, apply_impulse(point), get_point_transform, attachments | Pin/attach/impulse/readback by render-vertex index with a weld map | M | 1 |
| P33 | CSG is convert-time only | CSGShape3D live rebuild, bake_static_mesh | Runtime boolean builder via script ABI producing runtime mesh and collision | L | 0 |
| P34 | No mesh asset readback from scripts | Mesh.get_faces, get_aabb, create_trimesh_shape, navmesh parse | Read-only vertex/index/bounds access for assets | M | 0 |
| P35 | Runtime meshes: no skin, morph, custom channels, partial update | ArrayMesh bones/weights, blend shapes, ARRAY_CUSTOM | Skinned and morphed runtime meshes, region updates | L | 0 |
| P36 | Nav bake: rectangle cover, no contours; no 3D avoidance or carving | NavigationMesh sample_partition_type, use_3d_avoidance, carve | Contour-based bake, 3D avoidance, runtime carving | L | 0 |
| P37 | WOFF/WOFF2 fonts copied as converted but rejected | FontFile .woff/.woff2 | Web-font container decode to sfnt. WOFF needs only inflate; WOFF2 needs Brotli (dependency). Until then the converter must rate it unsupported | M | 1 |
| P38 | Glyph and line spacing do not reach the text engine | FontVariation.spacing_*, LabelSettings.line_spacing | Spacing parameters in the text block and measure ABI | S | 1 |
| P39 | No UI textures by content path | TextureRect, TextureButton, NinePatchRect, StyleBoxTexture | UI image service: path to GPU texture, ref-counted, usable from the draw list and managed code. BUILT 2026-11-01 (pulled forward from wave 8a), headless-tested, GPU unverified: see the wave 8 status | M | 5 |
| P40 | UI draw list is axis aligned | Control.rotation, scale, pivot_offset | 2D affine transform on rect, glyph, mesh and clip | M | 3 |
| P41 | One UI sampler/blend pipeline | CanvasItem.texture_filter, texture_repeat, blend modes, clip_children | Per-draw sampler and blend mode, mask-based child clip | M | 2 |
| P42 | Pointer modes collapse to free or captured | MOUSE_MODE_HIDDEN/CONFINED, warp_mouse, cursor shapes | Explicit pointer state (visible, hidden, captured, confined), cursor shape, warp. Fabric defaults MouseMode to Captured where Godot uses Visible | S | 0 |
| P43 | Key set too small (46 keys) | F1-F12, PageUp/Down, Home/End, Insert/Delete, punctuation, numpad, wheel as press, extra mouse buttons | Append key slots (graphs store literal ints, so append only), .ocinput version bump, Win32 and SDL tables | M | 13 |
| P44 | No half-axis action bindings | InputEventJoypadMotion with signed axis_value, get_action_strength never negative | Positive-only / direction-filtered binding mode | S | 9 |
| P45 | Gamepad: pad 0 only, 14 buttons, 6 axes, no rumble | Guide/Misc/paddles, joy_connection_changed, start_joy_vibration | Multi-pad state, wider button/axis sets, connection events, rumble | M | 5 |
| P46 | No runtime window control | DisplayServer.window_set_mode/size/flags, Window.mode | Window state (mode, size, position, flags) in the window layer with ABI | M | 5 |
| P47 | Vsync is a boolean; no adaptive/mailbox; no project key | VSYNC_ADAPTIVE/MAILBOX, display/window/vsync | Present-mode enum and WINDOW.VSYNC key | S | 4 |
| P48 | WAV loop ping-pong/backward, no loop offset for streamed files | loop_mode, loop_offset | Bidirectional loop and restart offset in streamed audio | M | 0 |
| P49 | Pitch shift is time-domain | AudioEffectPitchShift fft_size | Phase-vocoder shifter | M | 1 |

### 3.3 fabric-mapping (75)

The engine feature exists or no engine feature is needed; the work is in Aver.Fabric or the converter.

| ID | Gap | Godot API | Engine feature it maps to | Size | Demos |
|---|---|---|---|---|---|
| F1 | Viewport 3D rendering properties and enums missing | msaa_3d, scaling_3d_*, fsr_sharpness, use_taa, screen_space_aa, mesh_lod_threshold, use_occlusion_culling, find_world_3d | RENDER.MSAA/TAA/LODTHRESHOLD/OCCLUSIONCULL, AverSR, FSR1. MSAA conflicts with TAA and denoisers, so the mapping must pick consistent sets. Depends on P17 | M | 24 |
| F2 | DisplayServer vsync API missing | window_set_vsync_mode/get, VSYNC_* | `aver_fw_request_vsync`, `Display.SetVSync` | S | 4 |
| F3 | Joint3D family missing as nodes | PinJoint3D, HingeJoint3D, SliderJoint3D, ConeTwistJoint3D, Generic6DOFJoint3D | `physics_joints_abi.h`; `GodotJoints.cs` already has the parameter mapping. Needs node classes, node_a/node_b resolution, deferred creation, exclude-collision | M | 2 |
| F4 | RigidBody3D settings stored not pushed | can_sleep, continuous_cd, lock_rotation, axis locks, damp modes | `physics_body_ext_abi.h` (locked axes, CCD, sleeping) | S | 2 |
| F5 | Small body/character API | test_move, apply_floor_snap, platform angular velocity, sync_to_physics, contact getters | `aver_phys_body_test_motion`, kinematic controller, contact monitor | S | 0 |
| F6 | PhysicsServer3D space parameters | space_set_param/get_param, SPACE_PARAM_* | `PhysicsConfig` settings (velocitySteps, baumgarte) | S | 1 |
| F7 | Shape-owner API and shape RIDs | create_shape_owner, shape_owner_*, shape_rid | Mutable compound shapes (`aver_phys_body_compound_*`) | M | 1 |
| F8 | Point query and test-motion classes | intersect_point, PhysicsPointQueryParameters3D, PhysicsTestMotionParameters3D | `aver_phys_point_query`, `aver_phys_body_test_motion` | M | 0 |
| F9 | Debug collision display | debug_collisions_hint, debug_color, get_debug_mesh | `physics_debug_abi.h` (needs a JPH_DEBUG_RENDERER build; unverified for shipped builds) | M | 1 |
| F10 | Collision shape classes and scale | HeightMapShape3D, SeparationRayShape3D, CollisionPolygon3D runtime, capsule/cylinder scale | Heightfield and separation-ray shapes, `aver_phys_shape_scaled` | M | 0 |
| F11 | SoftBody3D node | SoftBody3D properties, pinned points | `modules/softbody`: cloth, inflate, jelly, .ocsoft. Converter generates .ocsoft; units differ (kPa, compliance) so expect tuning. Depends on P32 for script parts | L | 1 |
| F12 | PhysicalBone3D / PhysicalBoneSimulator3D | joint_type, swing_span, twist_span, physical_bones_start_simulation | Engine ragdoll (`aver_phys_ragdoll_*`, `Entity.StartRagdoll`). Depends on F13 | M | 1 |
| F13 | Skinned glTF meshes drawn in bind pose | MeshInstance3D.skeleton/skin, Skin, SkinReference, BoneAttachment3D | CSkeletalMesh + CAnimator; Fabric Skeleton3D addresses the entity through P23 | M | 3 |
| F14 | glTF animation clips not put into the model's AnimationPlayer | imported scene AnimationPlayer, AnimationTree anim_player | Converter decodes glTF channels into Animation resources (the platformer player's 8 clips never play today) | M | 1 |
| F15 | Bone-constraint modifiers | LookAtModifier3D, AimModifier3D, CopyTransformModifier3D, ConvertTransformModifier3D | `OcRigOpKind::AimAt/CopyBone/SetBone` as base. Depends on P23 | L | 0 |
| F16 | Animation API remainder | state_started/finished signals, node_changed, Animation.copy_track, compress | none (managed) | S | 0 |
| F17 | Custom AnimationNode and tree features | AnimationNodeExtension, custom timeline, root_motion_local, reset_ends | engine player mix (`PropPlayer::treeAdd/treeMix`) | M | 0 |
| F18 | Skeleton and mesh API remainder | blend shape count/names, get_bone_meta, register_skin | `aver_fw_morph_*` (weight by hash and count only; name list may need a read-only ABI call) | S | 0 |
| F19 | AnimatedSprite3D and SpriteFrames | play/stop/frame, SpriteFrames | Runtime meshes plus per-frame material swap or UV shift; needs Sprite3D run-time redraw and WebP | M | 1 |
| F20 | Light bake mode and GI enums | light_bake_mode, BAKE_*, GI_MODE_* | Store only (every light is dynamic) | S | 2 |
| F21 | Physical light units and camera exposure | light_intensity_lumens/lux, light_temperature, CameraAttributesPhysical | `CLight::intensityLux`, `sunTemperatureK`, RENDER.EXPOSURE. Derive Godot's exposure formula from its source (unverified here); local lights need a temperature tint | S | 1 |
| F22 | Light projector textures | OmniLight3D/SpotLight3D.light_projector | `CLight::cookie`. Unverified: cookie sampling on the raster local-light path | S | 1 |
| F23 | ProceduralSkyMaterial flattened to a two-colour dome | sky_top_color, sky_curve, sun_angle_max, sky_cover | Emit as a sky shader program (G6 AVER_CUSTOM_SKY). Opt-in: a sky shader costs at every sky lookup and in ray-driven mode | M | 29 |
| F24 | Volumetric fog parameters with existing settings | volumetric_fog_length, anisotropy, temporal_reprojection | `Voxi::Settings` fields; new project keys | S | 2 |
| F25 | Decal node not in Fabric | Decal, size, textures, fades | `CDecal`, `DecalPool`, OcWorld DECAL record. Axis mapping (projects along local +X) must be derived from Godot's Y-down projection | M | 6 |
| F26 | Procedural texture resources and texture class aliases | GradientTexture1D/2D, NoiseTexture2D/3D, FastNoiseLite, CurveTexture, AtlasTexture, CanvasTexture, Texture, CompressedTexture2D | Bake to PNG at convert time (noise not bit-identical); atlas as texture plus UV rect | M | 12 |
| F27 | ParticleProcessMaterial / GPUParticles3D | amount, lifetime, emission_shape, velocity, damping, curves | `.ocparticle` (shapes, rate/burst, ranges, 13 modules, sub-UV, sub-emitters). Godot per-particle range pairs do not all map | L | 4 |
| F28 | CPUParticles3D | 77 properties | Same `.ocparticle` path (CPU tier). Depends on F27 | M | 4 |
| F29 | GPUParticlesCollision/Attractor nodes (14 classes) | Collision Sphere/Box/HeightField/SDF, Attractor Sphere/Box/VectorField | `update.collidesphere`, `update.attractor` through user-parameter slots; box/heightfield/SDF need P15 | M | 3 |
| F30 | Particle sprite sheets | particles_anim_h_frames/v_frames/loop | per-particle sub-UV playback. Depends on F27 | S | 5 |
| F31 | Camera3D/World3D environment and attributes | Camera3D.environment/attributes, World3D.environment | Environment/Sky/CameraAttributes already publish through a WorldEnvironment; apply only for the current camera | S | 2 |
| F32 | Mesh LOD threshold and bias | mesh_lod_threshold, lod_bias, ImporterMesh LODs | RENDER.LODTHRESHOLD, meshoptimizer simplifier. Per-instance lod_bias needs a small CMeshRenderer field | M | 2 |
| F33 | Occluder classes | OccluderInstance3D, ArrayOccluder3D and four more | Accept and ignore; HZB culling is automatic | S | 1 |
| F34 | Primitive mesh classes at run time | CapsuleMesh, CylinderMesh, PlaneMesh, QuadMesh, PrismMesh, TorusMesh, SphereMesh segments/material | Generators exist in C++ (`FabricMeshGen.cpp`); duplicate in managed code over ArrayMesh | M | 4 |
| F35 | Smaller BaseMaterial3D conversions | emission_energy_multiplier, roughness_texture_channel, vertex_color_is_srgb, clearcoat_roughness, alpha_antialiasing_mode | OcMat fields. Emission calibration changes the look of emissive materials in 12 demos | S | 12 |
| F36 | Project defaults without a key | default_clear_color, screen_space_aa | Authored sky colour; RENDER.NEURAA/TAA | S | 3 |
| F37 | Stored-only RenderingServer quality calls | environment_set_ssao_quality, ssil_quality, ssr_max_steps, gi_set_use_half_resolution | Store; keep rated approximated | S | 1 |
| F38 | Procedural mesh API leftovers | SurfaceTool.set_custom/set_bones, MeshDataTool edges, ArrayMesh blend shapes | Managed ArrayMesh/SurfaceTool | M | 0 |
| F39 | Runtime-mesh budget (256 meshes) not raised | many ArrayMesh chunks | `aver_fw_rtmesh_configure`. Unverified whether voxel exceeds 256; unmeasured per-mesh cost | S | 1 |
| F40 | Geometry3D helpers and TriangleMesh | segment/triangle tests, clip_polygon, tetrahedralize | Pure managed maths | M | 0 |
| F41 | Navigation script API leftovers and shape sources | set_debug_enabled, region_*/agent_* getters, bake from sphere/capsule/convex | `nav.abi`, `aver_phys_body_triangles` | S | 1 |
| F42 | GridMap at run time | set_cell_item redraw, bake_navigation, physics_material | Runtime MultiMesh, compound shapes. Today built once at convert time | M | 0 |
| F43 | Godot compat aliases | playback_process_mode, Resource.instantiate, Object.set_script, get_cmdline_user_args | Aliases in Fabric | S | 1 |
| F44 | Viewport size, size_changed, content scale | Viewport.size_changed, Window.size | `aver_ui_viewport`; type get_viewport() as Window where root | S | 4 |
| F45 | Built-in ui_* actions not in InputMap | ui_accept, ui_cancel, ui_left, ... | Register Godot's default action set at start-up | S | 12 |
| F46 | Pointer position with no Control in the scene | Viewport.get_mouse_position | `aver_ui_pointer`; poll every frame. Unverified at run time | S | 5 |
| F47 | Label3D.font typed Resource | Label3D.font, Font.get_string_size | Type as Font | S | 1 |
| F48 | Translation API | tr, tr_n, TranslationServer, Translation | `Loc.Tr/TrN/SetCulture`, .po | S | 0 |
| F49 | TextServer object API, TextLine, TextParagraph | shaped_text_*, TextLine | UiTextLayout queries exposed through the UI ABI | M | 0 |
| F50 | CodeEdit and syntax highlighters | CodeEdit, CodeHighlighter | Built on Fabric TextEdit | L | 0 |
| F51 | RichTextLabel/TextEdit/Tree/ItemList remainder | dropcap, ruby, caret_blink, tab_stops, scroll bars | Fabric widgets | M | 1 |
| F52 | MenuBar, FoldableContainer, FoldableGroup, ReferenceRect | same | Composition of existing widgets | M | 0 |
| F53 | GraphEdit, GraphNode, GraphElement, GraphFrame | same | Draw-list meshes; needs F58 and P40 | L | 0 |
| F54 | FileDialog | FileDialog | `platform::openFileDialog` or embedded dialog; needs F63 | M | 0 |
| F55 | ColorPicker full feature set | wheel shapes, sliders, hex, eyedropper | Fabric widget | S | 1 |
| F56 | TextureProgressBar radial fills | fill_mode radial | Textured fan via `addMesh`; needs P39 | S | 1 |
| F57 | Control layout leftovers | pivot_offset_ratio, layout_direction RTL, accessibility_*, shortcut_context | Bidi text, accessibility layer | M | 2 |
| F58 | CanvasItem draw API partial | draw_polygon, polyline, circle, arc, draw_set_transform | Tessellate to `UiDrawList::addMesh` | M | 0 |
| F59 | CanvasLayer layer/offset/scale and z_index not applied | CanvasLayer, z_index | Sort Fabric's draw list; compose the content transform | S | 3 |
| F60 | Engine singleton members | physics_ticks_per_second setter, get_physics_interpolation_fraction, get_version_info, get_frames_drawn | `aver_phys_set_fixed_step`, `aver_phys_step_alpha`. Unverified: changing the step with live bodies | S | 2 |
| F61 | OS singleton surface | shell_open, get_static_memory_usage, get_user_data_dir, get_processor_count | BCL, `Paths.User` | S | 3 |
| F62 | Time and Performance singletons | Time.get_ticks_usec, get_datetime_dict, Performance.get_monitor | BCL, engine counters | S | 1 |
| F63 | FileAccess, DirAccess, JSON, ConfigFile, ProjectSettings.globalize_path | same | `Paths.User/Globalize`, `ConfigFile`, System.Text.Json | M | 1 |
| F64 | Thread, Mutex, Semaphore, WorkerThreadPool | same | System.Threading; deferred calls already thread-safe. Unverified: mesh building from workers | M | 1 |
| F65 | HTTPRequest, HTTPClient, WebSocketPeer, StreamPeerTLS | same | System.Net.Http, ClientWebSocket, SslStream | M | 0 |
| F66 | Audio API leftovers and Area3D audio routing | volume_linear, audio_bus_override, reverb_bus_*, AudioStreamWAV.load_from_buffer | Bus graph, zone send reverbs (`AverReverbVolume`) | S | 0 |
| F67 | Cursor shape, custom cursor, warp, joypad names, sensors | set_default_cursor_shape, warp_mouse, get_joy_name | Platform window; unverified for cursor shapes and hot-plug | S | 0 |
| F68 | XR nodes and XRServer as inert carriers | XROrigin3D, XRCamera3D, XRServer.find_interface | Aliases to Camera3D/Node3D; `find_interface` returns null | S | 1 |
| F69 | Object reflection and run-time set_script | get_property_list, get_method_list, set_script | Replace node with a script-class instance (identity changes; feasibility unverified) | M | 1 |
| F70 | ProjectSettings.get_setting for any key | get_setting/set_setting/has_setting | Fabric `project.json` (12 keys answered today) | S | 2 |
| F71 | Data and utility classes | RegEx, Crypto, HashingContext, ZIPReader, XMLParser, UndoRedo | BCL | M | 0 |
| F72 | Runtime model import classes | GLTFDocument, GLTFState, ImporterMesh | `Aver.Framework ModelImport`, `FabricModelTree.cpp` | L | 0 |
| F73 | String method coverage (80 of 116 unmapped) | rsplit, trim_prefix, get_slice, casecmp_to, ... | BCL | M | 2 |
| F74 | Array/Dictionary/Callable/Vector/Color method leftovers | find_custom, merged, bindv, snappedf, get_luminance | none (managed) | M | 1 |
| F75 | Variant dynamic dispatch covers few types | `.origin`, `.basis`, AABB members, Variant member set | Extend `GDScript.GetMember/SetMember/CallMethod`. Failures are run-time exceptions, not compile errors | M | 6 |

### 3.4 converter-bug (20)

| ID | Gap | Godot API | Fix | Size | Demos |
|---|---|---|---|---|---|
| C1 | Script class named System (or Math, Task) breaks `using System;`; async functions get no Task return type | utils/system.gd, `await` | Emit `global::System`, rename colliding classes, lower await. The 73-error cascade hides other physics_tests errors | S | 1 |
| C2 | Static singleton accessors emit methods, not properties | Input.set_mouse_mode/get_mouse_mode | In `StaticCall` also try the get_/set_ accessor mapping (`Emitter.Call.cs`); same fix clears similar singleton pairs | S | 7 |
| C3 | Enum constant resolution | Viewport.MSAA_*, Light3D.BAKE_*, Mesh.PRIMITIVE_*, ANIMATION_PROCESS_*, KEY_1, bare PROCESS_MODE_*, Msaa3D/Scaling3DMode digit casing | Derive nested enums and casing from extension_api.json; drop a trailing "Type" from the prefix; try bare identifiers against the base class enum; declare the enums in Fabric under each class item | S | 14 |
| C4 | AABB (and other acronym casings) not mapped | AABB, RID | Map AABB to `Aabb`; audit acronym names | S | 1 |
| C5 | Value-type and integer coercions | Vector3(Vector3i), Vector3i(Vector3), long to int, bool to long, round(int) | Fix `ConstructorCall`/`Adapt`; cast before `Math.Round` | S | 4 |
| C6 | Member access on a base-typed receiver | Material.set_shader_parameter, CameraAttributesPractical.dof_blur_far_*, Viewport.content_scale_size, Resource.instantiate | When the member is missing on the static type, find the Godot subclass and cast or call by name | M | 8 |
| C7 | Autoload used as a type qualifier; script enum passed to Variant | `Log.LogType` | Resolve autoload scripts as types; cast enums | S | 1 |
| C8 | Static GDScript functions that use call() emitted static | `static func` | Keep the function an instance method when it needs one | S | 1 |
| C9 | Packed array methods unmapped | push_back, append, resize, sort | Helpers or a Packed*Array wrapper (value semantics differ) | M | 1 |
| C10 | Utility functions without mapping | asinh, rotate_toward, type_convert, var_to_bytes, Color8 | Map to `GD.*` / BCL | S | 1 |
| C11 | Untyped receivers fall back to by-name calls | `.origin`, `.connect()`, `.visible` on untyped vars | Flow typing (assignments, return types, scene index). Works today at run time, slower | L | 25 |
| C12 | Binary Godot resources not routed to the converter | .mesh, .occ, .lmbake, VoxelGIData | Treat any RSRC-magic file like .res; `arrayMeshAsset` already exists (also removes the GridMap "no mesh" rows) | S | 5 |
| C13 | PhysicsMaterial and axis_lock_* reported as not read, but the runtime reads them | PhysicsMaterial, axis_lock_* | Make PhysicsMaterial a known resource class (emit `Object(PhysicsMaterial,...)`), rate Converted. Unverified: end-to-end load of the dictionary into the node | S | 7 |
| C14 | uid:// in project settings not resolved | audio/buses/default_bus_layout | Apply the uid resolver to settings | S | 1 |
| C15 | Editor-only settings rated unsupported | blender import, vram_compression, thread_rid_pool_prealloc, global_group, low_processor_mode, physics/3d/physics_engine, physics_interpolation warnings, gui/theme font mipmaps/MSDF | Rate approximated "changes nothing"; physics_engine reads as Jolt | S | 11 |
| C16 | Jolt limits not mapped | physics/jolt_physics_3d/limits/* | Write PHYSICS.MAXBODIES, MAXBODYPAIRS, MAXCONTACTS, TEMPALLOCMB (bytes to MB) | S | 1 |
| C17 | .hdr used as an ordinary texture refused | *.hdr textures | stb decodes Radiance HDR; route to the float-image path or tonemap-convert | S | 1 |
| C18 | InputEventAction inside a Shortcut kept as a dictionary | Shortcut.events | Write `Object(InputEventAction, ...)` | S | 1 |
| C19 | Key modifiers and wheel presses dropped | InputEventKey modifiers, wheel buttons | Emit Chorded and Pressed triggers (`InputTriggers.cs`) | S | 3 |
| C20 | Silently dropped rendering properties have no report row | cast_shadow, gi_mode, extra_cull_margin | Add rating rows so losses show in `gaps.json` | S | 12 |

### 3.5 not-a-goal (15)

Listed with reasons in section 5.

| ID | Gap | Size | Demos |
|---|---|---|---|
| N1 | Joint softness and solver parameters (bias, relaxation, limit softness) | S | 0 |
| N2 | shadow_reverse_cull_face, shadow_transmittance_bias, shadow_opacity | S | 1 |
| N3 | SSAO, SSIL, SSR, SDFGI, VoxelGI, baked LightmapGI | XL | 4 |
| N4 | Variable rate shading | S | 1 |
| N5 | XR rendering, OpenXR, XR trackers and modifiers | XL | 1 |
| N6 | Viewport debug draw modes | S | 1 |
| N7 | Editor, import and Godot-renderer-selection project settings | S | 31 |
| N8 | Named physics and render layers | S | 2 |
| N9 | Mesh shadow proxies and lightmap hints | S | 4 |
| N10 | glTF import-time classes (GLTFAnimation, GLTFSkeleton, GLTFSkin) | S | 0 |
| N11 | Exclusive fullscreen (map to borderless) | S | 1 |
| N12 | Editor plugin and editor script classes, @tool scripts | XL | 3 |
| N13 | VisualShaderNode classes built from scripts | XL | 0 |
| N14 | RenderingDevice family (36 classes) | XL | 0 |
| N15 | GDExtension virtual-interface and platform bridge classes | XL | 0 |

## 4. Waves

Order rule: cheap fixes that unblock the most scripts first, then engine features by demos blocked per unit of effort, with dependencies respected. Each wave ends with a re-conversion of the 32 demos and a comparison with the baseline in section 2.

**How they run (owner, 2026-11-01).** Wave 1 is done. The waves that change the renderer run one after another in the main session: 2, 4, 6, then 8. Waves 3 (input, window, display), 5 (physics, geometry, navigation) and 7 (skeleton, animation, ragdoll) run in parallel, each in its own session, git worktree and branch (`parity/wave-3`, `parity/wave-5`, `parity/wave-7` in both repositories), and are merged back as they finish. Every wave runs on Sonnet.

**Moved to a later project (owner, 2026-11-01), not in waves 2-8:** XR / OpenXR (N5, F68 beyond the carriers that keep scenes loading), baked lightmaps and the GI bakers (N3), the panorama sky (E4), cube texture dimensions (E5), ReflectionProbe capture (P12), stencil (E10), video playback (E19) and spring bones (E24).

**Look calibration lives in Fabric (owner, 2026-11-01).** Where matching Godot's look needs a different value (emission, exposure, units, tonemap, light falloff, fog density), the engine gains only the neutral capability, with its defaults unchanged; the Godot values and the mapping are the Fabric Connector's, applied when it converts or runs a Godot project.

Standard headless check for every wave (called "standard" below): build; `ctest -E <GPU suite list>` (engine, then the Fabric repository: `Aver.Fabric.Tests`, `tests/fabric`); `tests/formats`; reconvert the 32 demos; diff `gaps.json` and the script compile errors against the previous wave. Engine ABI changes also run the ABI enum/row tests. Vulkan code is compiled and, where there are CPU-side tests, tested; shader changes compile for both HLSL and SPIR-V.

### Wave 1: converter and Fabric API completions

Contents: C1 to C10, C12 to C20 (all converter bugs except C11); F20, F33, F36, F37, F43, F45, F47, F60 (members only), F61, F62, F73, F74, F75; F63 and F64 (BCL-backed).
Size: about 25 S items and a few M. No engine change.
Unblocks: the cascades behind most of the 557 compile errors. The `using System` cascade (73), the mouse-mode accessors (29 to 42), enum names (136 plus), AABB, coercions, `ui_*` actions in 12 demos, `.mesh` resources in 5, PhysicsMaterial rows in 7. Remaining errors after this wave are mostly missing Fabric classes and members handled in waves 3 to 7. Target: the count of demos with compile errors and the error total in `script-errors.txt` fall; the physics_tests list is re-read once the cascade is gone.
Verification: standard, plus converter unit tests for each fix (`Aver.Fabric.Tests`). C13 needs a test that loads a converted PhysicsMaterial into a body (the unverified link).
Headed check for the owner: none required.

### Wave 2: images, textures and materials

Contents: E1 WebP (vendored libwebp in the main session; image loader plus converter transcode path), F26 procedural texture resources and texture aliases, P1 sampler state, P2 surface material overrides, P14 script material parameters, F35 (deferred from wave 1 because it changes emissive looks), E8 runtime Image/ImageTexture, E3 and E2 if the owner approves their decoders.
Size: E1 M, P1 M, P2 M, P14 M, F26 M, E8 L.
Unblocks: textures in 31 demos (E1), 22 demos with sampler states (pixel-art nearest filtering, anisotropy), 19 demos with surface overrides, 12 demos with procedural textures, `Image`/`ImageTexture` script code in 2.
Dependencies: E1 before F19, F23 art, P39. P1 must check the RT hit path sampler (unverified) and not touch `PSRayDriven`; nearest filtering may show only on raster draws unless the owner lifts the G4 rule. P2 rides on the existing part split and does not apply to LOD-substituted or skinned meshes.
Verification: standard, plus CPU tests for WebP decode (known vectors: lossy, lossless, alpha), sampler selection in `.ocmat` round trip, override list application in `GameContent` part building. Converter: all 87 `.webp` files in the corpus decode.
Headed check for the owner: pixel-art demos with nearest filtering, a WebP-textured level, a multi-material mesh with an override, `procedural_materials` demo.

**Status (2026-11-02, branch `parity/wave-2b` here, `parity/wave-2` in the Fabric repository, not merged).** Built and tested headless: P1, P2, P14, F26, F35, E8, and E1 (the loader already decoded WebP, SVG and OpenEXR; the Fabric converter now copies and cooks them). E2 and E3 were not part of this wave. The Fabric side is in that repository's `docs/plans/WAVE2_MATERIALS.md`.
- **P1.** `.ocmat` FX `texfilter` (aniso, trilinear, bilinear, pointmip, point) and `texaddr` (wrap, clamp, mirror) pick one of 15 static samplers for a material's own maps; the default writes nothing and the ordinary pipelines are byte-identical. `RENDER.TEXTUREANISO` (1 to 16) and `RENDER.TEXTUREMIPBIAS` are latched when the renderer starts. The ray-traced hit path fetches through the bindless table with `gMaterialSampler` and never reads the FX record (the unverified item is now checked: it uses the engine sampler), so a nearest or clamped material is smooth in reflections and GI; `PSRayDriven` is untouched. `rhi::AddressMode::Mirror` and `SamplerDesc::mipBias` were appended; a pipeline layout now declares up to 20 static samplers (Direct3D 12 and Vulkan both).
- **P2.** `scene::CSurfaceMaterials` (16 slots of material tokens, registered by name), the `SurfaceMaterials` level component (`slot0` ... `slot15` by material name), `Entity.SetSurfaceMaterial` and `planEntityDraws` take the override per mesh material slot (`MeshPart::slot`), on the part split and on the one slot of a single-slot mesh, posed parts of a skinned mesh included. A LOD- or soft-body-substituted draw, the GPU cluster path and instanced foliage keep the entity's material; a streamed level's chunk-owned placements are not bound. Prefabs keep the component by material name; a slot a prefab leaves out reverts to 0.
- **P14.** `aver_pbr_find`, the UV transform, sampler, channel, vertex-colour and alpha-extension setters on the material (`docs/ABI.md`, "Finding an instance, sampler state ..."); Aver.Fabric's `BaseMaterial3D` pushes to them.
- **F35.** `PARAM emissiveEnergy` (a scale on the glow, default 1, not written at 1), FX `roughchan` / `metalchan` (which channel of the metal-rough map each scalar reads) and `vcsrgb`; `ImportedMaterial::emissiveEnergy` carries it through the material cook. Emission calibration (decision 15) is not decided or changed: the converter writes colour and energy whose product is what the factor held before.
- **E8.** `framework_rtex_abi.h` (own version 1): script-built textures from pixels, updated whenever, named `@rt:rtex.<key>` in a material slot, a GPU handle for the UI; `Aver.Framework.Image` (CPU image, blit, resize, convert, decode) and `RuntimeTexture`. Uploads happen the frame after a change; the old GPU texture retires six frames later.
- **Tests.** Engine ctest with the GPU exclusion list: 508 of 508 pass (VoxiSpirvCompileTest, the SPIR-V compile of the shaders, included), on top of 08-gdint 413ed7fa. New or changed: `MaterialFxTest` (sampler words, channels, energy, round trips), `MaterialTest` (the cook carries the energy), `ProjectRenderApplyTest` (aniso and bias), `SceneSubmissionTest` and `LevelComponentsTest` (override planning and the level record), `PrefabSystemTest` (named slots, sparse revert), `FrameworkRtexAbiTest`, `AbiEnumTest`, `Aver.Framework.Tests` `ImageTests` (a null-source `Blit` threw `NullReferenceException`; fixed). Fabric repository: `Aver.Fabric.Tests` 6220 checks, `FabricWave2Test` 54, 36 of 36 ctest. The 32-demo dry-run reconversion drops every `surface_material_override` row (144 unsupported to 0) and every `.webp` row (69 to 0); all 87 `.webp` files of the corpus decode.
- **Unverified (needs a GPU or the editor).** Nearest, clamped or mirrored sampling on screen; the six-frame retirement of replaced runtime textures; `@rt:rtex` textures in a material and in the UI; the per-slot override on a real multi-material mesh; SPIR-V pipelines using the new sampler registers beyond the compile test; the pipeline-variant cost of many sampler states.
- **Not done.** A material a script makes with `new StandardMaterial3D()` has no `.ocmat` and no engine material, so its values are kept and it is not drawn. An override material that is not an `.ocmat` file is not applied. `NoiseTexture3D` and `CurveXYZTexture` have no 2D picture. Sampler states other than the five filters (anisotropy per material) and the FX groups other than UV and sampler are not pushed at run time.
- **Headed checks for the owner:** nearest-filtered pixel-art materials (and their reflection), a WebP-textured level, a multi-material mesh with `surface_material_override/N`, `procedural_materials`, a script changing `albedo_color` / `uv1_offset` / a texture, and an `ImageTexture` updated every frame on a mesh and in the UI.

### Wave 3: input, window and display

Contents: P43 key set, P44 half-axis bindings, P45 gamepad, P42 pointer modes, F46 and F67 pointer and cursor, P46 window control, P47 and F2 vsync, E12 frame cap, F44 viewport size and signals, E21 touch (mouse emulation first), F68 XR carriers.
Size: mostly S and M.
Unblocks: key set (13 demos, F10 appears in 12), half-axis (9), gamepad (5), pointer without UI (5), vsync (4 plus the 3 using the API), window (5), frame cap (2).
Notes: key slots are appended only (saved graphs store literal ints); `.ocinput` version bump; Win32 and SDL key tables both updated (SDL has never been compiled locally, per memory, so CI covers it). Exclusive fullscreen maps to borderless (N11).
Verification: standard, plus unit tests for key tables, binding modifiers (half-axis), and ABI rows. Platform behaviour is not testable headless.
Headed check for the owner: F-keys, PageUp/PageDown, gamepad rumble, fullscreen toggle, vsync modes, a mouse-driven demo with no UI.

**Result (branch `parity/wave-3` in both repositories).** All the contents are in. Engine: framework ABI 2.8 (`docs/ABI.md`, "Input and display"), 59 appended key slots and a managed `Key` enum that mirrors them, four pads with 21 buttons, a connection flag, a name and rumble (XInput, SDL), touch state with mouse emulation, pointer modes, cursor shapes and images and a warp, window requests with the published state, present modes (Vulkan has all four, Direct3D keeps its pair and reports what it uses) and a frame cap in `Engine`, the `clamp` modifier and `.ocinput` version 2, the `WINDOW.VSYNC` and `WINDOW.MAXFPS` keys. The shipped game applies the pointer, window, present and cap requests (`GameDisplay.cpp`); the editor ignores them. Fabric: Godot's key, mouse-button and joypad numbers for all of it, the converter's input-map writer (every key, the nine mouse buttons, 21 pad buttons, Meta chords, half axes), `Input` (mouse mode default Visible, joypads and rumble, `joy_connection_changed`, cursor, warp, touch emulation, sensors), `DisplayServer`, `Window` controls, `Viewport.size_changed` and the pointer position, `Engine.max_fps`, and the XR nodes and `XRServer` as inert carriers. What is not done: `Input.set_custom_mouse_cursor` with a texture (its pixels are not readable from a script until wave 2's image classes land; the RGBA path works), a joypad's GUID and vendor details, motion sensors, F13 to F35, and the `emulate_touch_from_mouse` project setting. What nothing here could run: a window changing mode, a cursor changing shape, a pad rumbling, a finger, the SDL paths (compiled by CI only).

### Wave 4: rendering settings, lights, shadows, environment

Contents, in order: P17 render settings ABI, F1 Viewport properties, F37, E7 debanding, F31, F21, F22, F24, P3 cascades, P4 shadow filter, P5 per-instance shadow and GI flags, P8 ambient, P9 fog extras, P10 glow, P18 render stats, P20 visibility fade, F32 LOD mapping, F23 procedural sky as an opt-in sky shader, P6 and P11 fog depth.
Size: about 15 items, S to M.
Unblocks: 24 demos' viewport settings, 26 demos' shadow settings, 29 demos' sky look (opt-in), 14 debanding, 12 cast_shadow/gi_mode, 10 glow, 9 ambient, 7 fog.
Notes: most sun and shadow settings change only the raster or fallback path when RT is on (default from Medium), so the visible gain is on Low tier, Vulkan without ray queries, and the cascade fallback. Defaults unchanged: a new post composite variant (glow blend, debanding) must leave the ordinary permutations byte-identical. F23 stays opt-in until measured headed (sky shaders cost at every sky lookup and in ray-driven mode). MSAA/TAA/denoiser combinations: F1 chooses a consistent set (MSAA off when TAA or a denoiser is on) and documents it.
Verification: standard, plus shader compile for D3D12 and Vulkan, CPU tests for settings round trips (project keys, ABI), unit tests for the Godot-to-Aver exposure and cascade-split mappings.
Headed check for the owner: the lights_and_shadows, global_illumination, sky_shaders, volumetric_fog demos; a Low-tier cascade view; the sky shader cost on the target GPU. Items that change the look need owner approval before they become default.

**Status (2026-11-02, branch `parity/wave-4` here, `parity/wave-4` in the Fabric repository, not merged).** Built and tested headless. The Fabric side is in that repository's `docs/plans/WAVE4_RENDER.md`. Every default is unchanged; the one look change is named under "Owner decisions" below.
- **P17 render settings ABI: done.** `framework_render_abi.h` (own version 1, `docs/ABI.md`): render scale, MSAA, sharpness, TAA, LOD threshold, occlusion, sun-shadow cascades/split/distance/filter and the light-shaft fog knobs, applied at the frame boundary through `voxi::applyScriptRenderSettings` (one template both hosts use); a released key restores the project's value. The shipped game applies scale, MSAA, shadow and fog keys; sharpness and TAA are stored (the upscaler is chosen when the project starts); LOD threshold and occlusion are stored there (the shipped runtime has neither) and applied by the editor's Play. C#: `RenderSettings`, `RenderKey`, `RenderStats`.
- **P3 cascades, P4 shadow filter: done.** `RENDER.SHADOWCASCADES / SHADOWSPLIT / SHADOWDIST / SHADOWRES / SHADOWFILTER` and the same `voxi::Settings` fields. Count, split lambda, maximum distance and the cascade edge (512 to 4096, latched when the renderer starts) are CPU only; the filter width rides the spare `gCascadeSplit[c].z`. Cascade blending and the fade start are not configurable (the fade stays at 0.84 of the last cascade). With ray-traced shadows on (the default from Medium) the atlas is not drawn, so none of this shows there.
- **E7 debanding, P10 glow: done, glow level weights not done.** `PostSettings::glowBlend` (add, screen, soft light, replace, mix), `glowStrength`, `glowLumCap`, `deband`, as post-volume keys, in the `AVER_POST_TONEMAP_EXT` composite only. `rhi/Glow.hpp` is the CPU twin. The glow pyramid has no per-level multiplier, so Godot's `glow_levels`, `hdr_scale` and the bloom term are stored only.
- **F24 volumetric fog parameters: done.** `RENDER.VOLFOGLENGTH / VOLFOGANISO / VOLFOGTEMPORAL`.
- **P5 per-instance flags, F32 LOD bias: done, with limits.** `CMeshRenderer::renderFlags / cullMarginCm / lodBias / fadeAmount` (appended, zero-safe). Shadow mode off acts on the cascades and the GI shadow map, not on the ray-traced sun shadow (the instance masks are not touched); shadows-only is the hidden-from-owner route (invisible to cameras, still a caster, GI and ray geometry, so it still appears in reflections); GI dynamic and disabled put the mesh in the mover lane (out of the voxel bake); the cull margin grows the frustum test; the occlusion opt-out and the LOD bias apply in the editor only (the shipped runtime has neither LOD selection nor occlusion culling).
- **P8 ambient: partly.** `SkyAtmosphere::ambientOverride / ambientColor / ambientEnergy / ambientSkyContribution`, the level `SKY ambient ...` tokens and script keys (worldfx ABI 2, `Sky.SetAmbient`) replace the Physical sky's irradiance coefficients on the CPU (`rhi::blendAmbientSh`; no shader). It does nothing under the Authored two-colour dome, which is what the converter writes for a ProceduralSkyMaterial. The reflection source is not done.
- **P18 render stats: done.** `aver_fw_render_stats` (objects, triangles, draw calls, translucent draws, shadow draws, frame), counted from the replayed draw list only once a script has asked; holds in ray-driven mode (draw calls read 0 there).
- **P20 visibility fade: partly.** The fade amount is computed per frame and published (`CMeshRenderer::fadeAmount`, `scene::rangeFadeAmount`); the draw is still a hard cut. A per-instance dither needs a per-draw constant in the material FX block, which is shader and pipeline-variant work left out under the GPU-safety rule.
- **P9 fog extras, P6 and P11 fog depth: not done.** Sun scatter, sky affect, aerial perspective, local-light in-scatter and the density texture are shader work in the shared fog or the froxel pass; Fabric stores them and rates them (`WAVE4_RENDER.md`).
- **Fabric side (F1, F37, F31, F21, F22, F23, F32 and the mappings):** done; see that repository. F23 is opt-in (`--procedural-sky`).
- **Tests.** Engine ctest with the GPU exclusion list: 510 tests; new or changed: `GlowTest`, `FrameworkRenderAbiTest`, `AtmosphereTest` (ambient SH), `ProjectRenderApplyTest` (shadow and fog keys), `FormatTest` (ambient token), `CameraProjectionTest` (render flags, fade amount), `FrameworkWorldFxAbiTest` (version 2), `AbiEnumTest` (render keys), `TonemapTest` / `ColourVisionTest` (sizes). Shader checks: the eight ordinary `PSComposite` permutations compile to byte-identical DXIL (dxc `ps_6_0`, base against this branch); the EXT variants compile (DXIL, and SPIR-V in `VoxiSpirvCompileTest`).
- **Shader changes and what they could cost.** (1) `post.hlsl` / `color.hlsli`, EXT composite only: a glow blend function and a three-line debanding add; a few ALU per pixel, only when a Godot tonemapper, glow blend or debanding selects that composite. (2) `voxi.hlsl` `shadowSampleCascade`: the 3x3 kernel offset is multiplied by a constant-buffer value (`gCascadeSplit[c].z`) and the quadrant inset is a `max` of two values; same taps, no loop, no ray query, not in `PSRayDriven`; the DXIL of the scene permutations changes, the values at the default (1.0) are identical. Nothing else in the shaders changed.
- **Unverified (needs a GPU or the editor).** The EXT composite on screen (glow blends, debanding); the cascade count, split, distance and filter on a Low-tier view and under Vulkan; a 4096 cascade atlas (256 MiB) on the target GPU; the ambient override on a physical sky; shadows-only and the mover lane for GI modes, in raster and in ray-driven mode; the render scale, MSAA and shadow keys changed from a script at run time; the statistics in ray-driven mode.
- **Headed checks for the owner:** the lights_and_shadows, global_illumination, sky_shaders and volumetric_fog demos; a Low-tier cascade view with 2 and 4 cascades and a hard and a wide filter; a glow-enabled level with each blend mode; debanding on a sky gradient; a physical-sky level with an ambient colour; `cast_shadow` off and shadows-only on a mesh next to a mirror; a script changing `RenderSettings` in the shipped game and in Play.
- **Owner decisions.** (1) A Godot glow-enabled Environment now converts to the soft-light blend (Godot's default) instead of the old additive; it is the only default look change: keep it or hold it additive until a headed look. (2) Physical-light exposure reaches the lights only, not the sky or emission. (3) `--procedural-sky` stays opt-in until timed. (4) Unstated volumetric fog values keep the engine's defaults, not Godot's.

### Wave 5: physics, geometry, navigation

Contents: F3 Joint3D, F4, F5, F6, F7, F8, F9, F10, C16 follow-up, P27, P28 interpolation for any node, P29 step cap, P30, P31, F11 SoftBody3D with P32, E22 CSG path mode, E9 line and point primitives, F34 primitive meshes, F38, F39, F40, F41, F42.
Size: F11 L, E22 M, F3 M, P28 M, E9 M, rest S to M.
Unblocks: soft_body_physics (17 SoftBody3D errors), csg road demos (4), joint demos (2), interpolation (2), line meshes (3), primitive meshes (4), voxel runtime-mesh budget (1).
Notes: Godot limits are relative to the joint frame; Aver measures them from the pose at creation, so authored joints with off-frame bodies differ (F3). Jolt swing limits differ from Godot Euler limits at large angles. F11 parameters change units (kPa, compliance) and need eye tuning. E9 can start as camera-facing quads rebuilt each frame (loses 1 px width); an RHI topology touches both backends and is the owner's call. P28 matters for CharacterBody3D: players are kinematic and not blended today.
Verification: standard; physics ABI tests (`tests/physics*`); CSG output compared by vertex/triangle counts and bounds; navigation by existing nav tests.
Headed check for the owner: joint behaviour, soft body parameters, the interpolation demo.

**Status (branch `parity/wave-5`, both repositories).** Landed with tests, headless: F3 joint nodes (`Joints3D.cs`), F4 rigid-body settings pushed (`can_sleep`, `continuous_cd`, `lock_rotation`, axis locks as the engine's own lock, custom centre of mass, `inertia`), F6 space parameters, F7 shape owners and shape ids, F8 point query and test-motion classes (`test_move`, `body_test_motion`), F9 `debug_collisions_hint` (the engine's debug lines; needs the debug renderer), F10 `HeightMapShape3D`, `SeparationRayShape3D`, `CollisionPolygon3D`, `BoxShape3D.extents`, F11 SoftBody3D with P32 by render-vertex index through a weld map, F34 primitive meshes (vertex order checked against Godot 4.7.2 for plane and sphere), F38 the `SurfaceTool` setters (accepted, not kept), F39 `RENDER.RUNTIMEMESHES` (the converter states 1024), F40 `Geometry3D` / `Geometry2D` / `TriangleMesh`, F41 the navigation debug switch and agent getters, C16 follow-up `PHYSICS.MAXSTEPS` from `physics/common/max_physics_steps_per_frame`, P27 inertia (`aver_phys_body_set_inertia`), P28 pose interpolation for any entity (`framework_interp_abi.h`, `docs/PHYSICS_INTERPOLATION.md`), P29 the step cap, P30 the face index (the engine already reported a mesh triangle's user data; Fabric passes the triangle number), E22 `sweepPolygon` and the converter's path mode, E9 lines and points as crossed quads of a fixed width in the managed runtime mesh (no RHI topology; the quads are not camera-facing and not a pixel wide).
Not done, with the reason: P31 (per-shape area events, area gravity for kinematic controllers, area wind on soft bodies: no demo, and the shape index is not in the event queue), F5 beyond `test_move` (apply_floor_snap, platform angular velocity, contact getters), F8 `PhysicsServer3D` shape creation calls, F42 GridMap at run time, F12-style ragdolls (wave 7). The remaining `physics_tests` compile errors are wave 3's `DisplayServer` window calls and a Godot 3 material flag. Unverified: everything that needs a physics world or a GPU (joints, soft bodies, interpolation on screen), which no headless test reaches.

### Wave 6: particles, decals, sprites

Contents: F27 ParticleProcessMaterial/GPUParticles3D, F28, F29, F30, P15 (shapes and modules, CPU and GPU together), P16 trails (ribbon stand-in first), F25 Decal node and converter, P13 decal properties, F19 AnimatedSprite3D (needs Sprite3D redraw and E1).
Size: F27 L, P15 L, others S to M.
Unblocks: particles (4 demos, 18 materials), flipbook particles (5), decals (6 demos, 69 nodes), sprites (1).
Notes: each new module needs both executors (ParticleGpuParityTest). P13 changes a 192-byte record mirrored in HLSL (static_assert), so C++ and HLSL change together. Decal axis mapping is derived from Godot's source, not guessed. Godot's range pairs and curves map only partly onto start/end pairs and four-stop curves; document what is lost.
Verification: standard, plus particle parity tests, `.ocparticle` round trip, decal record size assertion.
Headed check for the owner: particles demo, decals demo, sprites demo.

### Wave 7: skeleton, animation, ragdoll

Contents in order: P23 pose seam (framework ABI, a new component beside `CControlRig`, ordering against rig/IK/ragdoll in the modifier chain), F13 skinned mesh binding, F14 glTF clips into AnimationPlayer, F12 PhysicalBone3D to ragdoll, P24 authored ragdoll data, F16, F17, F18, P26, P25, F15, E24 if wanted.
Size: P23 L, F13 M, F14 M, F12 M, others S to M.
Unblocks: platformer player (clips and skin), ragdoll_physics, ik (also needs E23 COLLADA, an owner decision), physics_tests moving platform.
Notes: glTF import bakes the skeleton into engine space, so bone names and rests must round-trip against the converter's Skeleton3D. F14 needs glTF channel decoding in the converter; Godot's 30 fps import resample and CUBICSPLINE will differ.
Verification: standard, plus a pose seam round trip test (set a bone pose, read the final pose, check the chain order), and a platformer reconversion that now writes the clips. Rendering of skinned meshes cannot be judged headless.
Headed check for the owner: platformer player animation, ragdoll start/stop, a bone attachment.
Status (2026-11-01, branch `parity/wave-7` in both repositories): P23, F13, F14, F12, P24, F16, F17 (the base class and helpers only), F18, P26, P25 and F15 are built and tested headless; the engine side is in [ANIM_POSE_SEAM.md](../ANIM_POSE_SEAM.md), the Fabric side in the Fabric repository's `docs/plans/WAVE7_SKELETON.md`. Not built: E24 spring bones (moved to a later project) and E23 COLLADA (the `ik` demo still needs it; reported). Nothing was run with a GPU or the editor: skinned rendering, a ragdoll in a running game and the bone attachment are headed checks.

### Wave 8: text, UI, and the gated long tail

Contents, in order: P39 UI textures (then F56, F59, P40, P41, F58), E6 Label3D with F47 (needs a glyph atlas page readable by materials), P38, E14, F48 to F57, E16, P37 (WOFF only), then the renderer items that are held for owner decisions: P12 ReflectionProbe, E4 panorama sky, E5 cube dimensions, E10 stencil, E11 next_pass, P21 proximity fade, P19 orthographic, P22, P7, E17, E18, plus C11 type inference, F65 to F72, E13 TextMesh, E19, E20, E23, E25.
Size: P39 M, E6 L, the long tail mostly L.
Unblocks: textured UI in 5 demos, Label3D in 5 (71 nodes, 513 property rows), the rest 0 to 1 demos each.
Notes: all GPU-dependent items here (UI textures, Label3D, SDF glyphs, canvas shaders, SubViewport UI, stencil, cube dimensions) cannot be verified while the no-engine rule holds; they land unverified and are listed for headed testing. E6 should stay opt-in for sorting with blended draws and ray-driven mode until checked.
Verification: standard; CPU layout tests for text meshes; glyph atlas page lifetime tests (eviction while a mesh references UVs).
Headed check for the owner: the labels_and_texts demo, truck_town speedometer, touch buttons.

Status (2026-11-01): **P39 is built** (pulled forward from 8a by the owner after a converted godot-fps drew no UI image), headless-tested, never run on a GPU. Engine: `UiImageCache` in `Aver.Render.UI` (path to texture, refcounted, premultiplied RGBA8Unorm with a mip chain, destroyed 8 ticks after its last reference or use, behind the UI renderer's binding), the host callbacks `aver_ui_set_image_host` / `_image_acquire` / `_release` / `_size` / `_use` in `ui_abi.h`, `GameUiImages.hpp` installing the table in the editor (Play) and the shipped game, and the engine's own `image` widget resolving its path through it ([ABI.md](../ABI.md) "Images by content path"). Fabric: `Texture2D.HandleResolver` is set by default to the engine resolver (`UiImages`), references are released at the start and end of a play session, and a non-PNG texture is sized by the engine. Tests: `UiImageCacheTest`, `UiImageAbiTest`, `UiRenderTest`, and `UiImageTests` in `Aver.Fabric.Tests`. Headed check for the owner: godot-fps main menu (TextureRect, NinePatchRect, button StyleBoxTexture and icons draw), on D3D12 and Vulkan, then Stop and Play again. Unverified: the premultiplied look at the edges of translucent art, the sampler's use of the mip chain, a Play stop while the menu is on screen. F56 (radial TextureProgressBar) can now be built on it.

Status (2026-11-01, found by running a converted godot-fps, fixed in the general code paths, headless-tested, not run since): (1) a mesh spawned at run time (`Entity.SetMesh`, which Fabric scenes and any C# game use) drew the flat fallback under level-scoped material residency, because only a level's own placements bound their materials. `GameContent::authoredOrResolve` now finds the project's `.ocmat` by the surface's name on first sight, binds it and remembers a miss until the project's materials are released; the editor (`SandboxApp::resolveSurface`) and the shipped game (`buildDrawLook`, once per token behind the draw-look memo) both call it, and a bound token costs the one lookup it always did. Test: `RuntimeMaterialResolveTest`. (2) The "kMeshRendererVisible bit is clear" warning fired for meshes a script hid on purpose. `kMeshRendererVisibilitySet` (bit 2 of `CMeshRenderer.flags`) is set by `Entity.SetVisible`, the seed in `EnsureMeshRenderer` and a sequence's visibility track (`setMeshRendererVisible`), never by zero-filled storage, so the warning keeps its meaning (`meshVisibilityUndecided`) and an entity hidden through the API is neither warned about nor counted. Test: `SceneTest` (mesh visibility decided). The Fabric side of the same run (mesh surfaces and their materials, `Skin` binds, `MeshInstance3D.skeleton`, downgraded nodes) is in Fabric's `docs/FABRIC.md`.

Split wave 8 once wave 7 is done: 8a (UI textures and Label3D) then 8b (long tail, item by item as the owner decides).

### Dependencies

| Item | Needs |
|---|---|
| F1 | P17 |
| F12 | F13, P23 |
| F13, F15, P25, E24 | P23 |
| P24 | F13 |
| F28, F30 | F27 |
| F29 (box/height/SDF) | P15 |
| F19 | E1, Sprite3D redraw |
| E6 | glyph atlas exposed to materials; E14 helps quality at range |
| F56, F53 | P39, P40 |
| F54 | F63 |
| F11 (script parts) | P32 |
| P12 | E5 or an octahedral atlas |
| E1 | libwebp vendored (main session) |

## 5. Not a goal

| ID | What | Reason |
|---|---|---|
| N1 | Joint softness and solver parameters | Godot's own Jolt module ignores the same parameters; Godot's physics is not ported |
| N2 | Shadow reverse cull, transmittance bias, opacity | Godot-renderer bias workarounds; Aver has its own bias model (`CLight` shadowBias/shadowNormalBias). Stored by Fabric |
| N3 | SSAO, SSIL, SSR, SDFGI, VoxelGI, LightmapGI | Aver's voxel cone and ray-traced GI, sky occlusion and reflections cover them visually. No lightmap sampler exists on the GPU. A baker plus sampler would be XL |
| N4 | Variable rate shading | Performance hint with no visual meaning; no shading-rate path in the RHI. Accept and ignore |
| N5 | XR (OpenXR, XR nodes' real behaviour, trackers, modifiers) | No XR path in the engine; would touch swapchain, camera and input. Scenes still load through F68. Confirm with the owner |
| N6 | Viewport debug draw modes | Diagnostics of Godot's renderer; Aver has its own debug views. Stored only |
| N7 | Editor, import and renderer-selection project settings | Change nothing about Aver's output. Reported as recorded, not actioned (C15) |
| N8 | Named physics and render layers | Editor labels; numbers carry the behaviour. Kept as comments |
| N9 | Mesh shadow proxies, lightmap hints | No consumer; no lightmap baker |
| N10 | GLTFAnimation, GLTFSkeleton, GLTFSkin | Import-time classes; Aver has its own importer |
| N11 | Exclusive fullscreen | Deliberately avoided (`Window.hpp`: breaks alt-tab). Maps to borderless fullscreen, reported approximated |
| N12 | Editor plugin and editor script classes, @tool | Aver has no editor scripts. Game logic of the affected demos is unaffected |
| N13 | VisualShaderNode classes built from scripts | .tres VisualShaders convert text-to-text; script-built graphs are editor-style tooling |
| N14 | RenderingDevice family | Aver has its own RHI |
| N15 | GDExtension virtual-interface and platform bridge classes | Engine-internal or platform-specific; the GDExtension host serves a small class set |

## 6. Decisions the owner must make

New dependencies (each needs a yes under the no-new-deps rule):
1. SVG rasteriser (for example nanosvg) for E2. 7 demos, mostly icons. Option: do nothing and let the converter drop `.svg` icons.
2. OpenEXR decoder (for example tinyexr) for E3. 3 demos.
3. Brotli decoder for WOFF2 (P37). WOFF alone needs only inflate. 1 demo.
4. HarfBuzz or a large hand-written shaping subset for E15. No demo needs it. Recommended: defer.
5. Theora or a per-OS video codec for E19. No demo needs it.

Scope:
6. XR (N5): confirm it is out of scope.
7. COLLADA importer (E23, L) or an offline conversion of the one `.dae` to glTF. Affects only the ik demo.
8. Baked lighting (N3): confirm Aver stays on real-time GI.
9. Spring bones (E24), run-time CSG (P33), runtime mesh skinning (P35): none is needed by a demo. Build for API completeness or defer.
10. Native menus and tray (E20), video (E19), mic capture (E25): same question.

Expensive or risky renderer work:
11. Touching `PSRayDriven` (the G4 rule). Needed to show nearest filtering and cube/panorama sky in ray-driven mode (P1, E4, P12, P19). Lift it for one measured change at a time, or accept raster-only effects.
12. Panorama sky (E4, L), cube dimensions (E5, L), ReflectionProbe capture (P12, L), stencil (E10, L), next_pass (E11, M), proximity fade (P21, M), SubViewport quality (P22, L). Do each, or leave GI and ray tracing to cover them. The reviews rate P12 as adding little when RT or GI is on.
13. Line and point topology (E9): an RHI change for both backends, or camera-facing quads.
14. Procedural sky as default or opt-in (F23). It changes the look in 29 demos and risks the documented first-frame TDR in ray-driven mode. Recommended: opt-in.
15. Emission calibration (F35) changes emissive looks in 12 demos. Approve the change before it lands.

Process:
16. The no-engine-run rule. Everything GPU-dependent lands unverified (UI textures, Label3D, SDF glyphs, canvas shaders, SubViewport UI, stencil, cube textures, sky shader cost). Say when headed checks may run, or accept unverified landing with the headed lists in section 4.
17. Key slot extension needs an `.ocinput` version bump and appended slots only. Confirm this is acceptable for existing projects.
18. Platform: SDL code paths (never compiled locally) get CI as their only check. Confirm that is enough for waves 3 and 8.

## Unverified items (collected)

- Which sampler the RT/bindless hit fetch uses (P1): read in wave 2, the engine's `gMaterialSampler`; not confirmed on screen.
- Cookie sampling on the raster local-light path (F22).
- Exponential fog on sky pixels in ray-driven mode (P9).
- Godot's physical exposure constants (F21).
- Depth of the one-sun assumption in ReSTIR/NRD2 (P7).
- End-to-end load of a converted PhysicsMaterial into the node (C13).
- Whether the shipped build has `JPH_DEBUG_RENDERER` (F9).
- Whether voxel exceeds 256 runtime meshes and the per-mesh cost (F39).
- Changing the physics step with live bodies (F60).
- Soft-body vertex welding of primitive meshes (F11).
- Pointer polling with no UI present, cursor shapes, joypad hot-plug (F46, F67).
- Mesh building from worker threads (F64).
- Feasibility of `set_script` by node replacement (F69).
- The physics_tests errors behind the `using System` cascade (C1).
- Godot 4.6/4.7 classes: `extension_api.json` in `third_party` is Godot 4.5, so `classes.json` may miss newer members (for example the IK3D modifier family and `pivot_offset_ratio`).
- Nothing in this plan was built, run or GPU-tested.
