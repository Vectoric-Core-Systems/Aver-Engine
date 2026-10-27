# Code notes: softbody.scene

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp

- file banner: the system builds a cage from the entity's mesh (Aver.SoftBody: weld, beams, bends, masses, pins), makes the Jolt body through aver_phys_sb_*, mirrors simulated positions back, runs the damage step (dents are in-place writes to the body's edge rest lengths; tears rebuild the body under the same handle and split islands into debris pieces).
- file banner: components are registered dynamically, the CMotionMatcher pattern; Components.hpp ids and the scene ABI do not change. CSoftBody (id 13) is the marker the draw seam keys on and the legacy jiggle path (the marker part is kept on registerComponents).
- file banner: thick breaks (back skin and walls as derived draw geometry), fields (push, hurt, release, kill or hold soft bodies), break effects (particles and sounds bound in .ocsoft), saved damage (the `softbody` $State channel), proxy cages for meshes over the particle cap, and cheaper debris (reveal handshake, baking of settled pieces, distance cadence) are specified in docs/plans/wave10/SOFTBODY_DEPTH.md; the pure parts live in Aver.SoftBody.
- file banner, DETERMINISM: inputs are the asset bytes and preset, the ordered inbox (damage, explode, tear, repair, release, wind, budget, pulses), entity transforms and palettes per tick, and the fixed step count. Piece and event ids come from counters advanced in loop order. Fields run pulses (arrival order), then component fields (ascending entity id), then transient fields (ascending handle), with time from the step count. Cross-platform determinism is not claimed (kept in banner).
- attach comment: scales of 0 read as 1 (the zero-filled state means the useful default) (kept in shortened form).
- kStateProxy: simulated on a cooked proxy cage.
- kStatePieceShift: bits 8+ hold the live piece count.
- CCloth::outState / CDestructible::outState bits: header line removed.
- CCloth.reachScale: 0 reads 1 (scales skinned reach).
- CCloth.outMaxStrain and outState: both marked OUT.
- CDestructible.outDamage was "OUT: broken beams / total, 0..1" (kept, OUT prefix dropped).
- CSoftField header: "A placed soft-body field ... unless kFieldOnce, which applies strength and energy once as a pulse."; the `// "CSoftField"` tag line.
- CSoftField.outTouched: OUT, pieces in reach on the last tick.
- Options.maxLivePieces: debris pieces beyond the roots. maxRebuildsPerFrame: tear pipelines and entry builds. maxDrawBuildsPerFrame: resident builds and draw-mesh epochs on the draw side (Aver.Render.SoftBody). bakeEnabled: settled debris becomes a plain static mesh. bakeAfterS: asleep this long before it is baked.
- Options.hotSeconds: kept; watchStrain comment "or while its largest strain exceeds this" kept in merged form.
- setEffectsEnabled: "The master switch for break effects (particles and sounds). Immediate."
- SoftBodyState: "Per-entity query for graphs and scripts."
- PieceView.world: world cm per draw vertex. indices: visible triangles, into the draw vertices. meshId: the entity's authored mesh. origin: "(the entity has an identity-rotation CLocal at it)" dropped. centroid: shrink target, world cm. scale01: despawn shrink toward the centroid. frozen: last packed vertices are still current. empty: nothing to draw (every triangle gone). uv: "2 per draw vertex; null means every vertex is authored. Read for kGeneratedVertex entries" (the second sentence is kept in merged form).
- HitInfo.kind: DepositKind. HitInfo.maxStrain: "now, not at the hit".
- DentInfo: "Permanent set and live debris of one entity."
- PaletteFn: the skinning palette of an entity (model x inverse-bind per joint), or null.
- ProxyFn: starts (on = true) or stops a Kinematic ragdoll as the entity's collision proxy.
- BoneIndexFn: a bone name's index in the skeleton an entity is skinned to, or -1 (for bone-weight paint rules).
- PathFn: turns an asset id into a path (the same function the hosts hand AnimSystem).
- registerComponents: "(idempotent) and returns CCloth's type id."
- attachCloth: "(one body per entity root)".
- attachSoftField: "A field needs nothing else on the entity"; the "Defaults" sentence reads as "Radial sphere of 100 cm, all targets" (kept).
- setPathResolver: nothing dropped beyond wording.
- setEffectsEnabled: "The master switch for break effects (particles and sounds). Immediate." (wording kept as the header of the setter's reader).
- explode: returns entities in reach right now.
- cutPreview: how many unbroken beams a cut of this radius would break right now.
- setAsleep: holds (true) or releases (false) every piece of the entity asleep, the way a Sleep field does.
- repairAll: repairs every entry; returns how many.
- pulseField / startField / impact / spawnFieldEntity: kept (shortened).
- setPressure: "Sets the pressure (kPa at rest volume) of a closed cloth; applied at the next tick with the other ops." (the "closed cloth" qualifier and the applied-with-other-ops note are dropped from the surviving text; the false-cases sentence is kept).
- entityOfBody: "The cloth or destructible entity a physics body belongs to, kInvalidEntity when none." (dropped).
- softAsset: "The parsed .ocsoft behind an id through the same sources the cloth path uses, or null." (dropped).
- notifyEntity: "A graph event at an entity (the notify sink the host installed); no-op without one." (dropped).
- meshData: "or null" (dropped).
- windVelocity: "The world wind as last applied (cm/s), for the debug overlay." (dropped).
- rebase: subtracts deltaCm from every world position the system caches: each entry's lastWorld (else one hard re-skin through teleportCm), the cage particles, piece origin / centroid / bounds / draw arrays, queued hit points and field frames. The skin palette needs nothing: invBuildWorld is entity-local. (Kept shortened; the teleportCm re-skin and the invBuildWorld note are dropped.)
- Piece.capEntity: a child of `entity` (kept).
- Piece.empty: no triangles left to draw.
- Piece.needsRebuild: a beam broke since the body was built.
- Piece.needsRead: positions must be read at least once (a fresh body).
- Piece.fieldHeld: a Sleep field (or setAsleep) holds it inactive.
- Piece.capsHidden: beyond caps.lodCm: drawn without caps.
- Piece.needsPack: positions changed since the last published view.
- Piece.particles: cage particle ids, in Jolt vertex order. beams / triangles: cage ids.
- Piece.renderVerts: entry binding render vertex per front draw vertex.
- Piece draw arrays: the first sentence about [front | back skin | walls] and the interior-material split is kept; the rest was a prior note.
- Piece.drawIndices: visible triangles into draw vertices (caps appended). sourceVertex: draw vertex -> authored mesh vertex or kGeneratedVertex. drawUv: 2 per draw vertex. normals: smooth normals of the front draw vertices. capWorld: the interior-material cap entity's arrays. posBuf / velBuf: readback scratch (3 per vertex).
- Entry.legacy: a plain CSoftBody (dropped; the kept Entry comments are the rest).
- Entry.renderSrc: binding render vertex -> authored mesh vertex.
- Entry.proxyOn: this entry holds a share of its skeleton entity's collision proxy.
- Entry.capsLogged: the over-limit warning was logged.
- Entry.forceAsleep: setAsleep(true).
- Entry.logged: beam -> already in the log.
- Entry.groupApplied: the ragdoll collision group was set.
- Entry group labels "proxy cage" and "saved damage" removed.
- applyField: applies one field to every entry and returns the pieces in reach; `once` makes it a pulse (strength and energy applied once).
- holdAsleep: holds a piece asleep for a Sleep field (or setAsleep) this tick.
- Members: byBody_ body handle -> (entry entity, piece id); proxyUsers_ skeleton entity -> cloth users; stepClock_ fixed steps seen (damageEvalEveryNSteps); tickCount_ ticks seen (draw cadence, the Tuning HUD's tick); fieldTick_ advances once per field step (Sleep holds); fields_ ascending handle; spawnedLife_ spawnFieldEntity seconds left.
- Section markers removed (file-by-file map of the private methods): SoftBodySystem.cpp (lifecycle, publish, emit, readback, layers); SoftBodyBuild.cpp (buildPieceBody, initialPiece, fillPieceDraw, inflatables, ragdoll group); SoftBodyDamage.cpp (inbox, impacts, deposits, plastic step, debris lifecycle, release, repair); SoftBodyTear.cpp (tearPipeline, planTear, commitTear, tear log, killPiece); SoftBodyFields.cpp (fieldStep, applyField, releaseAnchorsIn, inReach, holdAsleep); SoftBodyCaps.cpp (thick breaks, cap entities, baking, draw cadence); SoftBodyFx.cpp (emitFx, flushFx); SoftBodySave.cpp (restoreBuild); SoftBodyProxy.cpp (buildProxy, proxyPositions); SoftBodyCloth.cpp (clothStep, applyLodAndFreeze, setCharacterProxy).
- Section headers "components", "options, events, views", "host seams", "inbox", "fields", "saved damage", "inflatables and the seams SoftShapeSystem uses (docs/plans/wave11/SOFTBODY_HAIR.md)", "outputs": removed as headings. The wave11 SOFTBODY_HAIR.md reference for the inflatable seams is recorded here.
- softBodyProvider / softFieldProvider: the framework relay providers (framework_softbody_abi.h, framework_softfield_abi.h); declared here so the system need not link the framework; the composition root hands them across with aver_fw_set_softbody_provider / aver_fw_set_softfield_provider (install example kept).
- softBodySystem(): process singleton, like anim::animSystem() (kept).


## modules/softbody.scene/include/aver/softbody/SoftShapeSystem.hpp

- file banner: a COMPANION of SoftBodySystem, in the same module and namespace, not a second path to the same thing. It reads the host for the mesh and asset sources, the view position, the notify sink and the cloth bodies that grab / pressure / buoyancy also reach.
- file banner: the entities do NOT carry CSoftBody (the marker SoftBodySystem adopts as a legacy soft body). A rope with no renderer gets one with the engine cube as placeholder mesh, whose own draw the published tube replaces.
- file banner: the shape system draws through the unchanged Aver.Render.SoftBody.
- file banner: frame order was "aver_phys_step, World::flush, softBodySystem().tick, softShapeSystem().tick, the draw scene's update" (kept in shortened form).
- file banner: the bodies are built at the first tick that has what they need (a mesh), within a per-frame budget; positions are read back each tick; the tick ends by refreshing the host's view list.
- file banner, DETERMINISM: state is a pure function of the components, the asset bytes, the entity transforms, the relay calls in arrival order and the fixed step count.
- file banner, attach: zero-filled storage means the useful state: a scale of 0 reads 1, a radius of 0 reads the asset's (the zero-filled, no-constructor warning is kept in shortened form).
- kRopeNoStartPin: the start end is free. kRopeNoEndPin: the end end is free.
- kShapeCountShift: CSoftRope: bits 8+ hold the segment count.
- CSoftVolume header: ": jelly". preset: SoftPreset (Jelly) when there is no asset. gravityScale and dampingScale: 0 reads 1. outVolumeRatio: OUT, current / rest volume. outMaxStretch: OUT, the largest lattice edge strain. outState: OUT, kShape* bits. outNodes: OUT, lattice node count.
- CSoftRope header: ": rope and cable". outLengthCm: OUT, the current polyline length. outStrain: OUT, (current - rest) / rest. outState: OUT, kShape* bits, segment count from bit 8.
- SoftSensorEvent: a soft body entering or leaving a sensor; `sensor` is 0 when the sensor body belongs to no rigid-body component.
- class header: registerComponents "(idempotent) and returns CSoftVolume's type id"; createRope "(the relay's createrope). 0 on failure".
- install: binds the world and publishes this system's views through the host; tick() calls it when needed.
- tick: once per frame after the physics step, World::flush and the host system's tick.
- isShape: entities the system built a body for; false for a stale or failed one.
- bodyOf: the physics handle behind an entity's soft body (jelly, rope or cloth), 0 when none.
- setPressure: jelly (and cloth, through the host).
- cutRope: the cut returns `other` at once; bodies are rebuilt at the next tick (the "at once" timing is dropped; "Bodies rebuild at the next tick" is kept).
- drainSensorEvents: sensor events of the last tick, oldest first; they are consumed.
- ropeAnchors: which vertex indices of a rope are anchored (first, last); false when not attached.
- rebase: subtracts deltaCm from every cached world position: lattice and rope nodes, render and tube vertices, centroids, anchor targets, cut seeds (the call-site sentence is kept).
- VolumeEntry.edgeRest: rest length of each lattice edge, world cm (the stretch readout).
- Private method groups: volume methods live in SoftShapeVolume.cpp and rope methods in SoftShapeRope.cpp (section markers removed).
- softBodySystem-style provider: the framework relay provider's install line is kept; "Declared here so the system need not link the framework" is dropped.
