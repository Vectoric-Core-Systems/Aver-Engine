#pragma once
// The engine's two numeric vocabularies: what a PROCESS returns, and what an ABI CALL returns.
//
// WHY THIS FILE EXISTS. Neither had a definition anywhere. Every tool independently chose `return 2`
// for a usage error and `return 1` for a failure -- a convention followed by AverAssetC, ActorSweep,
// MakeRig, MakeFoliage, MakeSamples and RelodTool, written out longhand in each, and stated nowhere.
// Meanwhile scripts/gates.ps1 and scripts/verify-game.ps1 return the COUNT of failures, so `exit 2`
// means "two gates moved" from one and "you called it wrong" from another. With CI now running both
// (.github/workflows/ci.yml), that ambiguity is a build server that cannot tell a broken invocation
// from a broken renderer.
//
// And on the ABI side every entry point returns a bare 1/0, so a caller learns THAT a call failed
// and never WHY: `aver_phys_body_aabb` returning 0 is a dead handle, a null pointer, or a body with
// no shape, and nothing distinguishes them.
#include "aver/core/Types.hpp"

namespace aver {

// ---- What a process returns to whoever launched it ---------------------------------------------
//
// SMALL AND RESERVED, 0..15. Anything above that band is free for a tool to define, which is what
// keeps this from becoming a place every new tool has to edit.
//
// A COUNT IS NOT AN EXIT CODE. Returning "the number of things that went wrong" reads well from a
// terminal and is unusable from a script: it collides with every reserved meaning, and 256 failures
// is indistinguishable from success once the shell truncates to a byte. A tool with a count prints
// it and returns Failed.
enum class ExitCode : int {
    Ok          = 0,   // it did the thing
    Failed      = 1,   // it ran, and the thing did not work: tests failed, gates moved, a build broke
    Usage       = 2,   // the caller is wrong: a missing argument, an unknown flag, a bad path
    Environment = 3,   // the MACHINE is wrong: no build tree, no compiler, no GPU, a missing SDK.
                       // Distinct from Usage because it is not the caller's fault and not fixable by
                       // re-reading the help text -- CI wants to report these differently.
    Interrupted = 4,   // stopped by a signal, a timeout, or a user
};

inline const char* exitCodeName(ExitCode c) {
    switch (c) {
        case ExitCode::Ok:          return "ok";
        case ExitCode::Failed:      return "failed";
        case ExitCode::Usage:       return "usage";
        case ExitCode::Environment: return "environment";
        case ExitCode::Interrupted: return "interrupted";
    }
    return "unknown";
}

// Shorthand for `return static_cast<int>(ExitCode::X);` at the end of a main().
inline int exitCode(ExitCode c) { return static_cast<int>(c); }

// ---- What an ABI call reports when it fails ----------------------------------------------------
//
// NEGATIVE, AND NEVER RETURNED DIRECTLY FROM AN EXISTING ENTRY POINT. This is the part that has to
// be got right, because the obvious design is broken: every ABI here returns 1 for success and 0 for
// failure, and every caller -- C++ and C# alike -- writes `if (aver_phys_...)`. Returning a negative
// code from those functions would make `if (r)` TRUE on failure, silently inverting every existing
// call site without one compile error.
//
// So the codes travel a SEPARATE channel: `aver_<module>_last_error()`. Existing entry points keep
// their 1/0 contract exactly, and a caller that wants the reason asks for it. That is additive in
// the sense this repo's ABI headers already use -- scene_abi.h's "MINOR 1 adds
// aver_scene_material_name. Additive only -- no existing binding changes."
//
// Values are FROZEN once shipped, for scripting_abi.h's reason: they cross a boundary as bare
// integers, so a value inserted in the middle renumbers every one of them for any binding built
// against an older header. Append, never insert.
enum class AbiError : i32 {
    Ok             =  0,   // no error recorded since the last call that set one
    BadHandle      = -1,   // a handle that is zero, out of range, or names a destroyed object
    NullPointer    = -2,   // a required out-parameter was null
    NotInitialised = -3,   // the module's world/device/host does not exist yet
    OutOfRange     = -4,   // an index or a count past the end of what exists
    Unsupported    = -5,   // a real request this build cannot serve (a module compiled out, a
                           // backend without the capability). Distinct from InvalidArgument: the
                           // caller is not wrong, this build simply cannot do it.
    InvalidArgument = -6,  // a value that is not a handle and is not legal -- a negative extent, an
                           // empty name, a NaN
    AllocationFailed = -7, // the request was legal and the memory was not there
};

inline const char* abiErrorName(AbiError e) {
    switch (e) {
        case AbiError::Ok:               return "ok";
        case AbiError::BadHandle:        return "bad handle";
        case AbiError::NullPointer:      return "null pointer";
        case AbiError::NotInitialised:   return "not initialised";
        case AbiError::OutOfRange:       return "out of range";
        case AbiError::Unsupported:      return "unsupported";
        case AbiError::InvalidArgument:  return "invalid argument";
        case AbiError::AllocationFailed: return "allocation failed";
    }
    return "unknown";
}

// The name for a raw i32 off an ABI boundary, where the value may be anything at all.
const char* abiErrorNameOf(i32 code);

// ---- The channel the codes travel on -----------------------------------------------------------
//
// THREAD-LOCAL, because two threads failing at once must not overwrite each other's reason -- the
// physics module runs a worker pool, and a "last error" shared between threads is a lottery.
//
// PER MODULE, not per process, and that falls out of how this engine is built rather than being a
// design choice: Aver.Core is a static library linked into each ABI DLL, so each DLL gets its own
// copy of this slot. `aver_phys_last_error()` therefore reports physics failures and nothing else,
// which is what a caller wants anyway -- a scene failure appearing in the physics channel would be
// worse than no channel at all.
//
// SET ON SUCCESS TOO. A slot that is only ever written on failure is a slot that reports a stale
// reason forever after one bad call, and the first person to trust it is misled. Any guard that
// records a failure records Ok on its success path.
void setAbiError(AbiError e);
AbiError lastAbiError();

} // namespace aver
