#pragma once
// `AverAssetC fabric <godot-project-dir> <out-dir>`: a Godot 4.1+ project as a standalone Aver project.
// Contract: docs/plans/fabric/G3_SCENES.md. The scene/node rules it writes to are G3_RUNTIME.md section 1.
#include <functional>
#include <string>
#include <vector>

namespace aver::fabric {

// One mesh of an imported model file (glTF, FBX, OBJ), as the model importer wrote it.
struct ModelPart {
    std::string meshAsset;   // content-relative path of the .ocmesh
    std::string name;        // the source node's name
    double x = 0, y = 0, z = 0;   // where it sits in the model, engine centimetres
};

struct ModelResult {
    bool ok = false;
    std::string error;
    std::vector<ModelPart> parts;
    std::vector<std::string> notes;   // anything the importer dropped, for the report
};

// What the converter borrows from AverAssetC.cpp (the model importer lives in that file's anonymous namespace).
struct Host {
    // Imports `input` (an absolute path), writing meshes (with their materials, textures, skeletons and clips) under
    // `<contentDir>/<outSubdir>` and materials under `<contentDir>/Materials`.
    std::function<ModelResult(const std::string& input, const std::string& contentDir, const std::string& outSubdir,
                              const std::string& base)>
        importModel;
};

// argv as AverAssetC's main receives it (argv[1] == "fabric"). Returns the process exit code.
int run(int argc, char** argv, const Host& host);

}  // namespace aver::fabric
