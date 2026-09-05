// See ErrorCodes.hpp for why the two vocabularies are separate and why AbiError never travels on an
// existing entry point's return value.
#include "aver/core/ErrorCodes.hpp"

namespace aver {
namespace {
// See the header: thread-local so concurrent failures do not overwrite each other, and one copy per
// DLL because Aver.Core is linked statically into each.
thread_local AbiError g_lastAbiError = AbiError::Ok;
} // namespace

void setAbiError(AbiError e) { g_lastAbiError = e; }
AbiError lastAbiError() { return g_lastAbiError; }


// A RAW i32, not an AbiError, because this is what a value that crossed the boundary actually is: a
// binding built against a newer header can hand back a code this build has never heard of, and
// switching over the enum would be undefined behaviour on that value. Names what it knows and says
// so honestly otherwise.
const char* abiErrorNameOf(i32 code) {
    switch (code) {
        case  0: return "ok";
        case -1: return "bad handle";
        case -2: return "null pointer";
        case -3: return "not initialised";
        case -4: return "out of range";
        case -5: return "unsupported";
        case -6: return "invalid argument";
        case -7: return "allocation failed";
        default: break;
    }
    // A POSITIVE value is not an error at all -- it is somebody passing a SUCCESS return in here,
    // which is worth naming differently from a code this build does not know.
    return code > 0 ? "not an error (positive)" : "unknown error code";
}

} // namespace aver
