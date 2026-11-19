// The engine seams offered as services (aver_ext_services.h, AverExtSeamService). The spec table is generated from
// scripts/abi-services.lock into SeamServices.gen.cpp; registerSeamServices (Extensions.hpp) resolves it at run time.
#pragma once

#include "aver/core/Types.hpp"

namespace aver::ext::detail {

struct SeamServiceSpec {
    const char* name;               // "aver.nav"
    u32 version;
    const char* dlls;               // ';'-separated CMake target names: the library that exports the functions
    const char* const* functions;   // in slot order
    usize count;
};

extern const SeamServiceSpec kSeamServices[];
extern const usize kSeamServiceCount;

}  // namespace aver::ext::detail
