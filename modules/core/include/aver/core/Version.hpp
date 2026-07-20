#pragma once
// The engine's identity, as a project manifest sees it.
//
// A `.ocproject` declares `ENGINE <name> <minVersion>` so the engine can refuse a project that
// needs a newer build (docs/PROJECTS.md). That check has to compare against something, and the
// version was previously known only to CMake — printed at configure time and nowhere else. It is
// injected here so the one number lives in `project(... VERSION ...)` and nothing hard-codes a
// copy that can drift.
#include "aver/core/Types.hpp"

#include <string_view>

#ifndef AVER_ENGINE_VERSION
#define AVER_ENGINE_VERSION "0.0.0"   // only reachable if the module is built outside our CMake
#endif

namespace aver {

inline constexpr std::string_view kEngineName = "Aver";
inline constexpr std::string_view kEngineVersion = AVER_ENGINE_VERSION;

// Compare two dotted version strings ("0.1.0", "1.2"). Returns <0, 0 or >0 like strcmp.
// Missing trailing components read as 0, so "0.1" == "0.1.0"; non-numeric junk reads as 0
// rather than failing, because a manifest is untrusted text and a parse error here would
// otherwise have to become a load error for a field that is only advisory.
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
