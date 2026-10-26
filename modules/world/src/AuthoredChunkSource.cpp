#include "aver/world/AuthoredChunkSource.hpp"

#include "aver/core/Log.hpp"

namespace aver::world {

bool AuthoredChunkSource::open(const std::string& cookDir, std::string* why) {
    close();
    dir_ = cookDir;
    std::string err;
    if (!src_.open(dir_ + "/" + kCookIndexName, dir_, &err)) {
        if (why) *why = err.empty() ? "could not open the cooked index in " + dir_ : err;
        return false;
    }
    if (src_.index().kind != kIndexKindCooked) {
        if (why) *why = dir_ + ": the index is persisted overrides, not a cooked authored level";
        return false;
    }
    open_ = true;
    return true;
}

void AuthoredChunkSource::close() {
    open_ = false;
    dir_.clear();
    src_ = RegionChunkSource{};
}

} // namespace aver::world
