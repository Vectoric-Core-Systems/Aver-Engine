#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// Minimal file I/O used by the asset/format loaders (Phase 2 onward).
// Directory containing the running executable (for locating assets next to it).
std::string executableDir();

bool fileExists(const std::string& path);
bool readFileBytes(const std::string& path, std::vector<u8>& out);
bool readFileText(const std::string& path, std::string& out);
bool writeFileBytes(const std::string& path, const void* data, usize size);

} // namespace aver
