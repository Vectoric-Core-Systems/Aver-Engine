// Sweeps a real project's scripts and reports what the actor editor would make of each file.
//
// Not a unit test: it runs against whatever is on this machine, so it asserts nothing and proves
// nothing about a clean checkout. It exists to answer one question honestly -- "would this open the
// actors people actually write?" -- which no fixture can answer, because a fixture is written by
// the same person who wrote the parser.
#include "aver/formats/ActorScript.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace aver;

int main(int argc, char** argv) {
    if (argc < 2) { AVER_ERROR("usage: ActorSweep <scripts-dir>"); return 2; }
    namespace fs = std::filesystem;
    std::error_code ec;

    u32 openable = 0, skipped = 0, declined = 0;
    for (fs::recursive_directory_iterator it(argv[1], fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".cs") continue;
        const std::string p = it->path().string();
        if (p.find("\\obj\\") != std::string::npos || p.find("\\bin\\") != std::string::npos) continue;

        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        const std::string text = ss.str();

        const fmt::ActorScript s = fmt::parseActorScript(text);
        const std::vector<fmt::ActorClassInfo> classes = fmt::parseActorClasses(text);
        u32 previewable = 0;
        for (const fmt::ActorClassInfo& k : classes)
            if (k.anything() || k.kind != fmt::ActorKind::Unknown) ++previewable;
        const std::string name = it->path().filename().string();

        if (s.status == fmt::ActorParseStatus::Ok) {
            ++openable;
            AVER_INFO("  OPEN   {}  region, {} placement(s), {} class(es)", name, s.models.size(), classes.size());
        } else if (s.status == fmt::ActorParseStatus::NoRegion && previewable > 0) {
            ++openable;
            AVER_INFO("  OPEN   {}  {} class(es), {} previewable", name, classes.size(), previewable);
            for (const fmt::ActorClassInfo& k : classes)
                AVER_INFO("           {} {:<18} '{}'{}{}{}{}",
                          k.hasViewport() ? (k.drawable() ? "3D " : "3d?") : "-- ",
                          fmt::actorKindName(k.kind), k.className,
                          k.hasMesh ? (" mesh " + k.meshPath) : std::string(),
                          k.kind == fmt::ActorKind::Character ? " capsule" : "",
                          k.hasCamera ? " camera" : "", k.hasPointLight ? " light" : "");
        } else if (s.status == fmt::ActorParseStatus::NoRegion) {
            ++skipped;
            AVER_INFO("  skip   {}  declares nothing previewable", name);
        } else {
            ++declined;
            AVER_WARN("  DECLINE {}  {}", name, s.error);
        }
    }
    AVER_INFO("=== {} openable, {} skipped, {} declined ===", openable, skipped, declined);
    return 0;
}
