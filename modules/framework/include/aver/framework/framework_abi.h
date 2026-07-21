#ifndef AVER_FRAMEWORK_ABI_H
#define AVER_FRAMEWORK_ABI_H

/* Gameplay framework C ABI — GameInstance, GameMode, actors, pawns and controllers.
 *
 * Same plain-C idiom as scene_abi.h and pbr_abi.h, and under the same two extra rules: nothing but
 * int32_t / int64_t / float / const char* crosses this boundary, and there are no function
 * pointers, void*, structs or enums in it. The dispatch tables the framework calls THROUGH live in
 * framework_hooks.h, which is not a P/Invoke surface and which no managed code ever marshals.
 *
 * WHY THIS IS A SEPARATE MODULE FROM Aver.Scene, and not simply more of it:
 *
 * modules/scene/README.md commits the world to being "render/physics-agnostic (no UObject)". A
 * gameplay framework is precisely the vocabulary that constraint excludes — actor, pawn, possess,
 * begin play. Keeping it in a second module means `scene_abi.h` can be read end to end without
 * meeting any of those words, and "use Aver.Scene without the framework" is a question answered by
 * a link line rather than by discipline.
 *
 * The arrow points DOWN: Aver.Framework links Aver.Scene, never the reverse. Gameplay stands on
 * storage. The framework therefore sweeps its instance lists with the scene's own validity check
 * rather than asking the scene for a destroy callback, because a callback would be an edge pointing
 * back up.
 *
 * NOTE ON THE BUILD SHAPE: this is a SHARED library that links another SHARED library, which no
 * other module in this tree does. The rule that shape is tested against is the one
 * modules/render.pbr/CMakeLists.txt states — no RHI type may sit behind a P/Invoke DLL. The full
 * transitive closure here is {Core, Assets, Scene}, with no RHI anywhere behind it, so the property
 * the rule protects holds. Collapsing the two into one DLL would satisfy the letter of "depends on
 * Core only" and destroy the separation above.
 *
 * Error convention, matching pbr_abi.h: 1 on success, 0 on a rejected request.
 */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_FW_BUILD)
#    define AVER_FW_ABI __declspec(dllexport)
#  else
#    define AVER_FW_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_FW_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version, as (major << 16) | minor — same contract as AVER_SCENE_ABI_VERSION, and versioned
 * INDEPENDENTLY of it. The framework's surface will move while the scene's is still settling, and a
 * single shared number would force a lockstep neither module needs. */
#define AVER_FW_ABI_VERSION_MAJOR 1
#define AVER_FW_ABI_VERSION_MINOR 0
#define AVER_FW_ABI_VERSION \
    ((AVER_FW_ABI_VERSION_MAJOR << 16) | AVER_FW_ABI_VERSION_MINOR)

AVER_FW_ABI int32_t aver_fw_abi_version(void);

/* The version of Aver.Scene this framework binary was BUILT against.
 *
 * Reported separately from aver_fw_abi_version because the two DLLs ship as separate files and can
 * be replaced independently. A framework built against scene major 1 loaded beside a scene major 2
 * is a mismatch the loader will not catch — the import lib resolves by NAME, and every name still
 * exists. Comparing this against aver_scene_abi_version() at bootstrap turns that into a message. */
AVER_FW_ABI int32_t aver_fw_scene_abi_version(void);

/* 1 when the Aver.Scene DLL actually LOADED reports the same major as this binary compiled against,
 * 0 when it does not. Unlike the two functions above it calls across the DLL boundary, which is the
 * only way to learn what is really loaded rather than what a header said at compile time.
 *
 * Keep it that way. If this ever stops calling into Aver.Scene the check silently becomes a
 * tautology, and — as the first build of this module demonstrated — the framework then imports
 * nothing from the scene at all and `dumpbin /dependents` shows no edge between them. */
AVER_FW_ABI int32_t aver_fw_scene_abi_matches(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_FRAMEWORK_ABI_H */
