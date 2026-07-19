#include "aver/core/Assert.hpp"
#include "aver/core/Log.hpp"

#include <cstdlib>

namespace aver::detail {

void assertFail(const char* expr, const char* file, int line, std::string_view msg) {
    if (msg.empty()) {
        AVER_ERROR("ASSERT FAILED: ({}) at {}:{}", expr, file, line);
    } else {
        AVER_ERROR("ASSERT FAILED: ({}) at {}:{} — {}", expr, file, line, msg);
    }
    AVER_DEBUGBREAK();
    std::abort();
}

} // namespace aver::detail
