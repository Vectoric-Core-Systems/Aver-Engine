#pragma once
// The engine's name and version, injected by CMake, and the comparison a `.ocproject`
// ENGINE line is checked with.
#include "aver/core/Types.hpp"

#include <string_view>

#ifndef AVER_ENGINE_VERSION
#define AVER_ENGINE_VERSION "0.0.0"   // only reachable if the module is built outside our CMake
#endif

namespace aver {

inline constexpr std::string_view kEngineName = "Aver";
inline constexpr std::string_view kEngineVersion = AVER_ENGINE_VERSION;

// Compares two dotted version strings ("0.1.0", "1.2"). Returns <0, 0 or >0 like strcmp.
// Missing trailing components and non-numeric junk both read as 0.
inline int compareVersions(std::string_view a, std::string_view b) {
    usize ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        u32 va = 0, vb = 0;
        while (ia < a.size() && a[ia] >= '0' && a[ia] <= '9') va = va * 10 + u32(a[ia++] - '0');
        while (ib < b.size() && b[ib] >= '0' && b[ib] <= '9') vb = vb * 10 + u32(b[ib++] - '0');
        if (va != vb) return va < vb ? -1 : 1;
        while (ia < a.size() && a[ia] != '.') ++ia;
        while (ib < b.size() && b[ib] != '.') ++ib;
        if (ia < a.size()) ++ia;
        if (ib < b.size()) ++ib;
    }
    return 0;
}

} // namespace aver
