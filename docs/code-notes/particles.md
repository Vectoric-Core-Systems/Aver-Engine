# Code notes: particles

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/particles/include/aver/particles/ParticleGpu.hpp

- banner: the GPU tier: pooled per-instance buffers, the simulation kernels (particles_sim.hlsl), the bitonic sort (particles_sort.hlsl) and the stats readback. Expressed purely in generic RHI like ParticleRenderer: no backend header.
- banner: a ParticleSystem gives this class plain-data GpuJobs (ParticleGpuTypes.hpp). record() runs them all in the renderer's prePass, phase by phase across every instance (Init, Begin, Spawn, ArgsUpdate, Update, Finish, Sort), so each barrier covers the whole frame once rather than once per instance.
- banner: compute runs in prePass because Vulkan forbids it inside dynamic rendering. The draw happens later, in transparentPass, from the buffers drawBuffers() names.
- banner: 32-bit atomics and groupshared only, one HLSL source for D3D12 and Vulkan.
- TrackedBuffer: a buffer and the state this class last left it in (the RHI never transitions implicitly).
- ParticleGpuPool: page buffers, one set of pages per capacity class (256 x 4^c particles, c = 0..6). A page holds several equal ranges, so a thousand small emitters share a handful of buffers. Every range's byte offset is a multiple of 256 (Vulkan's storage-buffer offset alignment). The counters and arguments blocks are padded to 64 uints.
- kCountersStride: 64 uints per instance in the counters buffer (kGpuCounterCount used). kArgsStride: 64 uints per instance in the args buffer (kArgsUints used).
- classFor: the smallest class holding `particles`, or -1 above the largest.
- init: builds the kernels and layouts. False (and the tier stays unusable) when the device has no resource factory, compute, or indirect support, or a kernel fails to compile.
- View: the camera the sort and the culling use: row-major view-projection.
- record: prePass; records every visible job. Instances with no job this frame keep their particles frozen and are not drawn. Returns the instances that ran.
- recordShifts: prePass, BEFORE record(). Applies the floating-origin shifts ParticleSystem queued (CSShift, one dispatch per instance: particle slots, trail points, pending event records). Buffers go back to the state they were in. Consumes `shifts`.
- setIndexCount: for mesh particles, the index count the draw arguments carry (default 6, the quad template).
- DrawBuffers.argsOffset: in bytes. DrawBuffers.capacity: logical capacity (particles). DrawBuffers.trailRing: trail points stored per particle.
- frame(): the frame counter, bumped by each record(). The draw reads buffers in the rest states record() leaves them in, so the renderer never has to barrier in transparentPass.
- counters(): the stats snapshot: the counters block of a handle (kGpuCounterCount uints) from the readback three frames ago.
- Instance.capacity: logical particles. Instance.readIndex: which alive list is read this frame. Instance.ranFrame: the frame record() last ran this instance. Instance.lastAliveWrite: element offset (within the instance) of the list the draw reads.
- counterCache_: CPU copy of each page's counters block, refreshed from the readback ring once per frame.
- csShift_: optional. A stale deploy without CSShift only loses the origin shift, not the tier.
- test seam section banner: "test seam (ParticleGpuParityTest)". Dump, dumpBytes, dump, probe and probeRng are the test seam.
- dumpBytes: the bytes dump() copies for `what` (0 for an unknown handle).
- dump: records a copy of one of the instance's regions into `dst` (a Readback buffer of at least dumpBytes()), leaving every buffer in the state record() expects. Returns the bytes copied.
- probe: runs CSProbe with `cb` against the instance's binding set and copies the 64 uints it writes into `dst`.
- probeRng: runs CSRngProbe. gReq float4 i (i < 32) holds (seed bits, stream bits). The 32 hashes (top 24 bits) are copied into `dst`.
- Instance.firstRan: the first frame it ran (the stats readback lags three frames behind).


## modules/particles/include/aver/particles/ParticleGpuTypes.hpp

- banner: the constant block the GPU tier's compute kernels read (b3 of particles_sim.hlsl) and the plain-data job ParticleSystem::tick hands to ParticleGpu::record.
- banner: C++ and HLSL declare the same layout. particles_common.hlsli holds the twin. CSProbe in particles_sim.hlsl writes sentinel field values that ParticleGpuParityTest reads back (the three-mirrors lesson).
- banner: change a struct here, change the HLSL and the static_asserts together.
- kGpuModuleRows: uint4 per module.
- events buffer: two regions. The frame parity picks the one being written; a child reads the other. A region holds four header uints (death count, collide count), then kGpuEventCapacity death records, then as many collide records, each eight uints (position xyz, id, velocity xyz, unused) as float bits.
- kGpuFlagLinked: linked ribbon, slot = id % capacity, no dead list. kGpuFlagTrail: trail ribbon, push to the trail ring. kGpuFlagSubuvStart: random start frame. kGpuFlagHasEvents: an events buffer is bound (this emitter raises events or has children).
- kCtrDead: dead-list size. kCtrAlive: entries in the alive list being read this frame. kCtrAliveNext: survivors appended this frame. kCtrStatAlive: CSFinish copy for the stats readback.
- kCtrDropped: spawns refused (dead list empty) since init. kCtrEvTotal: event-driven spawns this frame (CSBegin -> CSSpawn). kCtrEvIdBase: running id counter for event-driven spawns. kCtrEvIdThis: the base this frame's CSSpawn uses.
- kArgsUints: the args buffer (uints) every instance owns: kArgsUints of them.
- kArgDraw: DrawIndexedIndirectArgs: indexCount, instanceCount, firstIndex, baseVertex, firstInstance. kArgSpawn: DispatchIndirectArgs for CSSpawn (batch 0). kArgUpdate: DispatchIndirectArgs for CSUpdate.
- GpuSortCB: the b3 block of the sort kernels (particles_sort.hlsl). viewRow: clip.w as a dot with (position, 1): view depth. aliveWrite: element offset of the survivor list in the alive buffer. sortedBase: element offset of the sorted-order region. k, j: the merge size and stride of a Global / LocalMerge pass.
- GpuSpawnRequest: one spawn request, 7 float4s. xf: emitter world matrix, row-major: rows 0..2 rotate/scale, row 3 translates.
- GpuSimHeader: twelve float4 rows, packed the way an HLSL cbuffer packs a struct (no member straddles a row). The row numbers 0 to 11 were marked beside the members.
- GpuSimHeader.alive*: element offsets into the alive buffer. evActive: this emitter consumes its parent's events. evWriteBase: this frame's event region (uint offset). sortedBase: element offset of the sorted-order region. evReadBase: the parent's region this emitter consumes.
- GpuSimCB: the whole b3 block of the simulation kernels. mod: per module, 8 parameter values (bits), packed user refs, kind/stage.
- GpuJob: one tick of one GPU emitter instance, plain data. ParticleSystem produces them and ParticleGpu::record drains them.
- GpuJob.handle: ParticleGpu::acquire's result. parent: handle whose events this instance consumes (0 none). order: recording order key; children record before parents. visible: a culled instance is neither dispatched nor drawn.
- GpuJob.center: world-space bounds centre, for culling and cross-emitter order.
- GpuJob.boundsRadius: ParticleEffect::boundsRadiusCm (0: never culled by frustum). GpuJob.cullDistance: ParticleEffect::cullDistanceCm (0: never).
- GpuJob.cb: header, user values, modules and the first <= 64 requests. GpuJob.extra: requests past the first 64 (more spawn dispatches).
- GpuShift: a floating-origin shift of one GPU instance's RESIDENT state (LARGE_WORLD 2.2.6): its particle slots, trail points and pending event records are all `delta` smaller. ParticleSystem::shiftWorld queues one per live GPU instance; ParticleGpu runs them (CSShift) before any job of the frame. Plain data, like GpuJob.
- gpuShiftParticles / gpuShiftTrail / gpuShiftEvents: CPU mirrors of CSShift's arithmetic (particles_sim.hlsl): plain f32 `p - delta` per axis on exactly the fields the kernel writes, so ParticleShiftTest can hold the HLSL to a definition with no device. Dead slots shift too (harmless). A trail point keeps its w (the particle's age at the point). An event record shifts its position and not its velocity.
- gpuShiftEvents: `events` is the instance's whole events buffer, two regions of kGpuEventRegionUints uints. Only the first min(count, kGpuEventCapacity) records of each type are live; the rest is stale and left alone.
- packGpuModules: packs `fx`'s enabled modules into the uint4 table (kGpuModuleRows rows each); returns the module count.
- static_assert messages (kept in code) name particles_common.hlsli as the layout twin.
