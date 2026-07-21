#ifndef AVER_SCENE_ABI_H
#define AVER_SCENE_ABI_H

/* Scene C ABI — the stable surface the C# scripting layer binds to via P/Invoke.
 *
 * Deliberately plain C, in the same idiom as pbr_abi.h and voxi_abi.h: only int32_t / int64_t /
 * float / const char* cross the boundary, so the same header works for C, C++, C# DllImport and any
 * other FFI. Vectors are returned through a float* OUT-PARAM, never as a small struct by value —
 * the by-value-small-struct return convention differs between compilers and marshallers, and
 * getting it wrong corrupts the return register rather than failing to link.
 *
 * TWO rules this header is under that pbr_abi.h is not:
 *
 * 1. NO GAMEPLAY VOCABULARY. Not `actor`, not `pawn`, not `spawn`, not `possess`, not `play`. This
 *    module stores entities and components; what a component MEANS is the framework's business.
 *    The rule is enforced on a link line — Aver.Scene does not link Aver.Framework — but it is
 *    stated here because a header is where the temptation arrives first.
 *
 * 2. NO FUNCTION POINTERS, no void*, no structs and no enums. Everything here is marshalled by C#,
 *    and a blittable surface with no callbacks is one the bindings cannot get subtly wrong.
 *    Dispatch tables live in the framework's framework_hooks.h, which no managed code ever reads.
 *
 * Error convention, matching pbr_abi.h: setters return 1 on success, 0 if the request was rejected
 * (stale handle, bad field, or a value out of range). Getters return the current value, or a
 * documented neutral value for a stale handle.
 */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_SCENE_BUILD)
#    define AVER_SCENE_ABI __declspec(dllexport)
#  else
#    define AVER_SCENE_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_SCENE_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version, as (major << 16) | minor.
 *
 * MAJOR changes when an existing entry point changes shape or meaning; a managed binding compiled
 * against a different major must refuse to run. MINOR changes when entry points are only ADDED, so
 * an older binding still works against a newer engine and checks `minor >= what it needs`.
 *
 * This is a THIRD version boundary, distinct from the two scripting_abi.h already documents (the
 * host<->bridge contract, and the Aver.Scripting assembly version). It is separate because this DLL
 * can be replaced without either of those moving, and a silent mismatch here is a wrong pointer
 * rather than a missing method. */
#define AVER_SCENE_ABI_VERSION_MAJOR 1
#define AVER_SCENE_ABI_VERSION_MINOR 0
#define AVER_SCENE_ABI_VERSION \
    ((AVER_SCENE_ABI_VERSION_MAJOR << 16) | AVER_SCENE_ABI_VERSION_MINOR)

/* Returns AVER_SCENE_ABI_VERSION as the DLL was BUILT with, which is the whole point: the caller
 * compares it against the constant IT was built with, and the two disagree exactly when a stale
 * binary is present. */
AVER_SCENE_ABI int32_t aver_scene_abi_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_SCENE_ABI_H */
