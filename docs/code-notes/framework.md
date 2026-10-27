# Code notes: framework

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/framework/include/aver/framework/framework_origin_abi.h

- banner: the O pair is in centimetres, always a whole number of regions; Z is never shifted. The host shifts O at the top of the frame and moves every local position by -delta in the same slot. So a script that stores a `Position` across frames holds a value that is a region off after a rebase. Store an absolute (or a WorldPos) instead, or listen for "world.origin_changed".
- banner: the state lives in the host, which installs ONE provider (aver_fw_set_origin_provider), so Aver.Framework gains no link dependency on Aver.World.
- banner: with no provider, O is (0, 0), the generation is 0, the origin is not enabled, and request_shift refuses. The conversions are still exact (they are plain arithmetic about O).
- banner: event "world.origin_changed" is POSTED through aver_fw_event_post after every committed shift, with sender 0 and target 0. Payload ints: [0] dxCm, [1] dyCm (the shift: every local position changed by minus this), [2] generation after the shift, [3] originX, [4] originY (the new origin). Handlers run at the next aver_fw_events_flush, once the shift is complete.
- banner: int32_t / uint32_t / int64_t / float / double only; single game thread; 1 = success and 0 = refused unless stated.
- banner: RULE, an absolute is always f64 or a WorldPos, never a bare f32.
- provider struct: `size` is sizeof(aver_fw_origin_provider) so the struct can grow at the end; a smaller size than this header's is refused. Every member is required.
- set_origin_provider: copies the provider. NULL clears it. Returns 1 for installed or cleared, 0 for a malformed struct (the old provider stays). Kept: "0 for a malformed struct; the old provider stays."
- aver_fw_origin_get: writes O (x, y) in centimetres to outXyCm[2]. Returns 1 when a provider answered, 0 (and writes 0, 0) with none.
- to_local: the subtraction is in f64; the one rounding is the narrowing to f32.
- to_abs: the add is in f64, exact for every f32 and every O.
- WorldPos: chunk is i32 x3, 16 m chunks, region 0 centred on the origin; local offset in [0, 1600) cm. Also: a WorldPos gives 1.2 micrometres precision at any distance.
- request_shift: a console and test seam. Asks for a shift of whole regions at the next frame's slot S0, the same place a camera-driven shift runs, with the same deferral rules. Refused (0) unless the project states WORLD.ALLOWSHIFTREQUEST 1, the origin is not enabled, there is no provider, or both numbers are zero. 1 means queued, not done: watch the generation. A shift the deferral rules refuse (a level loading, a gesture, a flat streamed level) is dropped, not retried. Under the default follow-camera policy the next frame rebases back to the camera, so this is for tests and tools that also move the camera, or run the origin in the Manual policy (the editor's world.policy).
- OWNER CHECK: the request_shift sentence reads "Refused (0) unless ... the origin is not enabled ...". That wording looks like it may be a mistake (a disabled origin would be refused, not allowed). Kept verbatim in the code comment; please confirm the intended wording.
- generation member: 0 at start, +1 per committed shift or level reset.
- enabled member: 1 when the origin can shift. get member: writes O.x, O.y; returns 1. request_shift member: 1 = queued for the next frame's slot S0.
