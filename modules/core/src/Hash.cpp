// FNV-1a 64-bit.
#include "aver/core/Hash.hpp"

namespace aver {

// Hashes len bytes.
u64 fnv1a64(const void* data, usize len) {
    const auto* p = static_cast<const unsigned char*>(data);
    u64 h = kFnv1a64OffsetBasis;
    for (usize i = 0; i < len; ++i) {
        h ^= static_cast<u64>(p[i]);
        h *= kFnv1a64Prime;
    }
    return h;
}

// Hashes a string's bytes.
u64 fnv1a64(std::string_view s) {
    return fnv1a64(s.data(), s.size());
}

} // namespace aver
