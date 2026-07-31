#pragma once
// The assert macros. Always on for now; a release build can later compile them out
// behind AVER_ENABLE_ASSERTS.
#include "Prelude.hpp"
#include <string_view>

namespace aver::detail {
[[noreturn]] void assertFail(const char* expr, const char* file, int line, std::string_view msg);
}

#define AVER_ASSERT(cond) \
    do { if (!(cond)) ::aver::detail::assertFail(#cond, __FILE__, __LINE__, {}); } while (0)

#define AVER_ASSERTM(cond, msg) \
    do { if (!(cond)) ::aver::detail::assertFail(#cond, __FILE__, __LINE__, (msg)); } while (0)
