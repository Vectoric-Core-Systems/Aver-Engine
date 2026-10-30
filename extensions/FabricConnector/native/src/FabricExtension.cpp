// The Fabric Connector's side of the extension ABI (aver_ext.h): the one export the engine's loader calls, and the command it runs.
//
// This library links its own copy of Core, so at entry its log sink is pointed at the host (every line reaches the engine's log once, not
// twice) and the model importer the converter needs is the host's, reached through AverExtHost::importModel with plain structs.
#include "FabricConvert.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/LogConsole.hpp"
#include "aver/extensions/aver_ext.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

const AverExtHost* g_host = nullptr;

// Called under the library's log mutex: forwards the line and does not log.
void forwardLog(void*, aver::LogLevel level, std::string_view message) {
    if (!g_host || !g_host->log) return;
    const std::string line(message);
    g_host->log(int32_t(level), line.c_str());
}

// fabric::Host::importModel over the host's plain-struct importer.
aver::fabric::ModelResult importModelThroughHost(const std::string& input, const std::string& contentDir, const std::string& outSubdir,
                                                 const std::string& base) {
    aver::fabric::ModelResult out;
    if (!g_host || !g_host->importModel) { out.error = "the host offers no model importer"; return out; }
    AverExtModelArgs args;
    std::memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.input = input.c_str();
    args.contentDir = contentDir.c_str();
    args.outSubdir = outSubdir.c_str();
    args.base = base.c_str();
    AverExtModelResult res;
    std::memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    g_host->importModel(&args, &res);
    out.ok = res.ok != 0;
    out.error = res.error ? res.error : "";
    for (uint32_t i = 0; i < res.partCount; ++i) {
        const AverExtModelPart& p = res.parts[i];
        aver::fabric::ModelPart part;
        part.meshAsset = p.meshAsset ? p.meshAsset : "";
        part.name = p.name ? p.name : "";
        part.x = p.x; part.y = p.y; part.z = p.z;
        part.node = p.node;
        out.parts.push_back(std::move(part));
    }
    for (uint32_t i = 0; i < res.noteCount; ++i) out.notes.push_back(res.notes[i] ? res.notes[i] : "");
    if (g_host->freeModelResult) g_host->freeModelResult(&res);
    return out;
}

int32_t runCommand(const char* name, int32_t argc, const char* const* argv) {
    if (!name || std::strcmp(name, "fabric") != 0) return 99;
    // fabric::run takes argv as AverAssetC's main sees it: argv[1] is the command, the options follow from argv[2].
    std::vector<std::string> store;
    store.reserve(size_t(argc) + 1);
    store.push_back("AverAssetC");
    for (int32_t i = 0; i < argc; ++i) store.push_back(argv[i] ? argv[i] : "");
    std::vector<char*> ptrs;
    for (std::string& s : store) ptrs.push_back(s.data());
    ptrs.push_back(nullptr);

    aver::fabric::Host host;
    host.importModel = importModelThroughHost;
    if (g_host && g_host->extensionDir) host.extensionDir = g_host->extensionDir;
    return aver::fabric::run(int(store.size()), ptrs.data(), host);
}

void shutdownExtension() {
    aver::setLogSink(nullptr, nullptr);
    aver::setLogConsole(true);
    g_host = nullptr;
}

}  // namespace

extern "C" AVER_EXT_EXPORT int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out) {
    if (!host || !out || host->abi != AVER_EXT_ABI) return 1;
    g_host = host;
    aver::setLogSink(forwardLog, nullptr);
    aver::setLogConsole(false);
    out->runCommand = runCommand;
    out->shutdown = shutdownExtension;
    return 0;
}
