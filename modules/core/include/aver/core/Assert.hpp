#pragma once
#include "Prelude.hpp"
#include <string_view>

namespace aver::detail {
[[noreturn]] void assertFail(const char* expr, const char* file, int line, std::string_view msg);
}

// Always-on assert for now (Phase 1). A release build can later compile these out
// behind AVER_ENABLE_ASSERTS.
#define AVER_ASSERT(cond) \
    do { if (!(cond)) ::aver::detail::assertFail(#cond, __FILE__, __LINE__, {}); } while (0)

#define AVER_ASSERTM(cond, msg) \
    do { if (!(cond)) ::aver::detail::assertFail(#cond, __FILE__, __LINE__, (msg)); } while (0)
