#include "aver/core/Hash.hpp"

namespace aver {

u64 fnv1a64(const void* data, usize len) {
    const auto* p = static_cast<const unsigned char*>(data);
    u64 h = kFnv1a64OffsetBasis;
    for (usize i = 0; i < len; ++i) {
        h ^= static_cast<u64>(p[i]);
        h *= kFnv1a64Prime;
    }
    return h;
}

u64 fnv1a64(std::string_view s) {
    return fnv1a64(s.data(), s.size());
}

} // namespace aver
