#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// Minimal file I/O used by the asset/format loaders (Phase 2 onward).
bool fileExists(const std::string& path);
bool readFileBytes(const std::string& path, std::vector<u8>& out);
bool readFileText(const std::string& path, std::string& out);
bool writeFileBytes(const std::string& path, const void* data, usize size);

} // namespace aver
