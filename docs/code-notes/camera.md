# Code notes: camera

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/camera/include/aver/camera/CameraRigSystem.hpp

- header: CCameraRig is registered dynamically, exactly as CMotionMatcher is, so Components.hpp and scene_abi.h stay put.
- header: attach through CameraRigSystem::attach, never world.addComponent directly: the storage arrives zero-filled and runs no constructor. Zero-filled storage is a valid Chase rig (a 0 tuning field reads as its default).
- header: no C ABI of its own. A script drives a rig by writing fields through the generic scene ABI; the framework relay (framework_camera_abi.h, provider cameraRigProvider below) carries what is not a field write.
- header: a project with no rig is untouched: with no CCameraRig, no modifier, no blend and no external hand-back pending, drive() returns {active = false} and the host uses the legacy boom pose it computed first, bit for bit.
- header, DETERMINISM: inputs are component fields, world matrices after World::flush, Jolt query results, the host's frame dt and the ordered inbox. No wall clock and no rand; ids come from counters advanced in call order.
- Options::teleportCm: kept.
- Options::externalBlendS (default 0.5 s): hand-back blend after a sequencer camera lets go. Comment removed.
- Options::defaultProbeMask: kept (the host drops the cloth and proxy layers).
- Options::reduceMotionLagFloor, holdDestroyedS: kept.
- RigFrame::possessed: player 0's pawn. RigFrame::viewNode: aver_fw_view_entity() if valid. RigFrame::externalActive: the sequencer camera holds the view this frame.
- RigResult::cut: the camera teleported this frame.
- clearViewTarget: back to automatic selection.
- explicitViewTarget: true when the view target was set explicitly (SetViewTarget) and not cleared.
- Inbox: modifier calls take effect on the stack at once; the stack only advances inside drive, so the order is the same (the arrival-order rule kept).
- rebase (LARGE_WORLD 2.2.5): left alone, the boom lags 16 km behind its target for a frame and snaps. Covers follow lag, look-ahead, last poses, the blend source, queued shakes and the debug view.
- RigState::lagPivot / prevTarget: the lagged pivot and last frame's unlagged one.
- RigState::deadFor: seconds the rig's target has been dead.
- EvalOut::first: this rig's follow state was fresh (first evaluation).
- lastFinite_: previous frame's final finite pose.
- haveSource_: current_ was decided at least once.
- cameraRigSystem(): process singleton, like anim::animSystem().
- section banners ("Reads (last drive)", "The view target") restated names; dropped.
