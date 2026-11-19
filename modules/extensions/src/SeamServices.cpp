#include "SeamServices.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/extensions/Extensions.hpp"
#include "aver/extensions/aver_ext_services.h"
#include "aver/platform/FileSystem.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aver::ext {

namespace {

std::string sharedFileName(const std::string& target) {
#if defined(_WIN32)
    return target + ".dll";
#elif defined(__APPLE__)
    return "lib" + target + ".dylib";
#else
    return "lib" + target + ".so";
#endif
}

std::vector<std::string> splitDlls(const char* list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = list; *p; ++p) {
        if (*p == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += *p;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::mutex g_mu;
std::deque<std::vector<unsigned char>> g_tables;   // AverExtSeamService storage; registered tables must outlive the process's extensions

}  // namespace

usize registerSeamServices(const std::string& exeDir) {
    const std::string dir = exeDir.empty() ? executableDir() : exeDir;
    std::lock_guard<std::mutex> lock(g_mu);
    ServiceRegistry& reg = ServiceRegistry::instance();
    usize added = 0;
    for (usize s = 0; s < detail::kSeamServiceCount; ++s) {
        const detail::SeamServiceSpec& spec = detail::kSeamServices[s];
        if (reg.find(spec.name, spec.version)) { ++added; continue; }
        std::vector<void*> libs;
        for (const std::string& t : splitDlls(spec.dlls))
            if (void* lib = loadLibrary((dir + "/" + sharedFileName(t)).c_str())) libs.push_back(lib);
        if (libs.empty()) {
            AVER_INFO("[Extensions] service {} v{} is not offered: {} is not in this build", spec.name, spec.version, spec.dlls);
            continue;
        }
        const usize header = offsetof(AverExtSeamService, fn);
        std::vector<unsigned char> buf(header + spec.count * sizeof(void*));
        auto* table = reinterpret_cast<AverExtSeamService*>(buf.data());
        table->size = static_cast<uint32_t>(buf.size());
        table->count = static_cast<uint32_t>(spec.count);
        auto** slots = reinterpret_cast<const void**>(buf.data() + header);
        std::string missing;
        usize nMissing = 0;
        for (usize i = 0; i < spec.count; ++i) {
            void* fn = nullptr;
            for (void* lib : libs)
                if ((fn = librarySymbol(lib, spec.functions[i]))) break;
            slots[i] = fn;
            if (!fn && nMissing++ < 3) missing += std::string(missing.empty() ? "" : ", ") + spec.functions[i];
        }
        if (nMissing) {
            AVER_WARN("[Extensions] service {} v{} is not offered: {} of its {} functions are not exported ({}{})", spec.name, spec.version, nMissing,
                      spec.count, missing, nMissing > 3 ? ", ..." : "");
            continue;
        }
        g_tables.push_back(std::move(buf));
        reg.add(spec.name, spec.version, g_tables.back().data());
        ++added;
    }
    return added;
}

}  // namespace aver::ext
