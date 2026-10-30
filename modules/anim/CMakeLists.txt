# Aver.Anim — sampling .ocanim clips into poses, and posing a skeleton.
#
# CORE AND FORMATS ONLY, and that is the module's claim about itself rather than an accident: posing
# is arithmetic on a bone hierarchy, so all of it is decidable with no GPU and no device. Skinning a
# MESH with these matrices is somebody else's job, one tier up.
#
# .ocskel and .ocanim have existed and round-tripped since the format work, and until this module
# nothing in the engine read them. This is their first consumer.
aver_add_module(Aver.Anim
  SOURCES
    src/Pose.cpp
    src/AnimSampler.cpp
    src/Ik.cpp
    # Blend spaces, state machines and their .ocblend/.ocasm assets: arithmetic and JSON only.
    src/BlendSpace.cpp
    src/AnimStateMachine.cpp
    src/AnimGraphAsset.cpp
    # Montages, root motion and the slot/layered/additive pipeline (docs/ANIM_MONTAGES.md): pose arithmetic only.
    src/RootMotion.cpp
    src/Montage.cpp
    src/AnimPipeline.cpp
    # Motion matching (docs/ANIM_MOTION_MATCHING.md): pose database, search, trajectory prediction, inertialization.
    src/MotionAsset.cpp
    src/PoseDatabase.cpp
    src/PoseSearch.cpp
    src/Trajectory.cpp
    src/Inertializer.cpp
    src/MotionMatcher.cpp
    # IK solvers, the .ocikrig asset and its pure rig stack, and the retargeter (docs/plans/wave5/IK_RETARGETING.md).
    src/IkSolvers.cpp
    src/IkRigAsset.cpp
    src/IkRigSolver.cpp
    src/Retarget.cpp
    src/RetargetAsset.cpp
    src/RetargetAutoMap.cpp
    # [wave9/morph begin] blend-shape CPU reference, the .ocface face rig and the .ocvis lip-sync tracks (docs/plans/wave9/MORPH_TARGETS.md).
    src/Morph.cpp
    src/FaceRig.cpp
    src/Viseme.cpp
    # [wave9/morph end]
    # Property-track animation (an animation-player model): sampling and the player.
    src/PropAnim.cpp
    src/PropPlayer.cpp
  DEPS
    Aver.Core
    Aver.Formats
)
