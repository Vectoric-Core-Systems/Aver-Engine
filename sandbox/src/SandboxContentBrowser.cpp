// Editor: the Content Browser -- navigation, file operations, references, gallery, import.
// Split out of the 29,952-line SandboxApp.cpp (2026-09-16) by moving method bodies verbatim;
// the class is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#if AVER_MODULE_PBR
#include "aver/formats/ImportCook.hpp"
#endif

namespace aver {

// Converts a glTF/GLB into one .ocmesh per mesh (+ .ocskel/.ocanim if skinned) into destDir.
// Free function, not a member: shared by Content Browser Import (importModel) and --import-gltf
// (createApplication, before any SandboxApp exists) -- the loop is pure modules/formats calls.
// Returns false with *outWhy only on a hard parse failure; a clean parse writing nothing new still
// returns true with an all-zero summary -- callers decide if that counts as failure.
bool importGltfToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, GltfImportSummary& out, std::string* outWhy) {
    std::error_code dirEc;
    std::filesystem::create_directories(destDir, dirEc);

    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(src, res, {}, &why)) {
        if (outWhy) *outWhy = why;
        return false;
    }
    for (const std::string& u : res.unsupported)
        AVER_WARN("[Import] '{}' contains {} - not imported", std::filesystem::path(src).filename().string(), u);

    std::error_code ec;
    const std::string stem = std::filesystem::path(src).stem().string();

#if AVER_MODULE_PBR
    // Must run BEFORE the meshes below are written: cookImportedMaterials rewrites each mesh's
    // materialSlots to the cooked stems, and that has to land before saveOcMesh serialises a mesh
    // or the .ocmesh would name a material never written under that name (same ordering as
    // AverAssetC's cookAndRewriteSlots; see its own header comment). No-op when contentDir is empty
    // (--import-gltf's launcher-less case, since importModel always passes one) -- the cook reports
    // what it skips rather than losing it silently. maxTexture 0 = no cap, matching AverAssetC's default.
    if (!contentDir.empty() && (!res.materials.empty() || !res.images.empty())) {
        std::vector<std::string> matWarn;
        std::string matErr;
        u32 materialsWritten = 0, texturesWritten = 0;
        if (!fmt::cookImportedMaterials(res.materials, res.images, contentDir, stem, res.meshes,
                                        /*maxTexture=*/0, overwrite, &matWarn, &matErr,
                                        &materialsWritten, &texturesWritten)) {
            AVER_WARN("[Import] materials: {}", matErr);
        } else {
            out.materialsWritten = materialsWritten;
            out.texturesWritten = texturesWritten;
            for (const std::string& w : matWarn) AVER_WARN("[Import] materials: {}", w);
            if (materialsWritten || texturesWritten)
                AVER_INFO("[Import] {} -> {} material(s), {} texture(s) under {}",
                          std::filesystem::path(src).filename().string(), materialsWritten,
                          texturesWritten, contentDir);
        }
    }
#endif

    // Parallel to res.meshes: stem each mesh was written under, or empty if skipped (invalid, or name
    // taken). The scene level below reuses these rather than re-deriving the naming rule, since two
    // copies of it need only disagree once to write paths that resolve to nothing.
    std::vector<std::string> stems(res.meshes.size());
    for (usize i = 0; i < res.meshes.size(); ++i) {
        fmt::OcMeshData& m = res.meshes[i];
        if (!m.valid()) { AVER_WARN("[Import] mesh {} came out empty and was skipped", i); continue; }

        std::string base = i < res.meshNames.size() && !res.meshNames[i].empty() ? res.meshNames[i] : stem;
        if (res.meshes.size() > 1 && base == stem) base += "_" + std::to_string(i);
        for (char& c : base) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                                 c == '"' || c == '<' || c == '>' || c == '|') c = '_';

        std::string outFile = destDir + "\\" + base + ".ocmesh";
        if (std::filesystem::exists(outFile, ec)) {
            // Overwrite-aware skip: the true output name is only known here, which is why `overwrite`
            // reaches this far rather than the caller deleting a guess at what import would write.
            if (overwrite) {
                editor::moveToRecycleBin(outFile);
                AVER_INFO("[Import] '{}.ocmesh' already existed - replaced", base);
            } else {
                AVER_WARN("[Import] '{}.ocmesh' already exists - not overwritten", base);
                continue;
            }
        }
        if (!fmt::saveOcMesh(outFile, m, &why)) { AVER_WARN("[Import] {}", why); continue; }
        AVER_INFO("[Import] {} -> {} ({} verts, {} tris)", std::filesystem::path(src).filename().string(),
                  base + ".ocmesh", m.vertexCount(), m.indices.size() / 3);
        stems[i] = base;
        ++out.meshesWritten;
    }

    // Writes placements as a scene (only when there's more than one; a single PLACE record isn't
    // worth a file). Needed because the importer no longer welds node translations into vertices --
    // that used to put every imported mesh's pivot metres from itself -- so without this, a
    // multi-part import would land as correctly-centred pieces with no record of how they fit
    // together (AverAssetC learned this at the same time; same feature on the path the editor
    // actually uses).
    if (res.placements.size() > 1) {
        fmt::OcWorldData w;
        w.name = stem;
        for (const fmt::GltfPlacement& p : res.placements) {
            if (p.meshIndex < 0 || usize(p.meshIndex) >= stems.size()) continue;
            if (stems[usize(p.meshIndex)].empty()) continue;
            fmt::OcWorldPlacement op;
            // Relative to the level's own folder, the same way every other hand-authored PLACE is
            // relative to the content root.
            op.asset = stems[usize(p.meshIndex)] + ".ocmesh";
            op.x = p.position.x; op.y = p.position.y; op.z = p.position.z;
            w.placements.push_back(std::move(op));
        }
        const std::string lvl = destDir + "\\" + stem + ".ocworld";
        const bool lvlExists = std::filesystem::exists(lvl, ec);
        if (w.placements.empty()) {
            // Nothing to say; not worth a file.
        } else if (lvlExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocworld' already exists - not overwritten, so the scene layout "
                      "was not written", stem);
        } else if ((lvlExists && !editor::moveToRecycleBin(lvl)) || !fmt::saveOcworld(lvl, w, &why)) {
            AVER_WARN("[Import] could not write the scene layout: {}", why);
        } else {
            AVER_INFO("[Import] {} -> {}.ocworld ({} placement(s), the source scene's own layout)",
                      std::filesystem::path(src).filename().string(), stem, w.placements.size());
        }
    }

    // Writes the rig: previously res.skeletons/animations were dropped on the floor -- nothing ever
    // wrote them and no project could contain them -- so loadOcSkel/loadOcAnim had no caller in the
    // engine's history.
    for (usize i = 0; i < res.skeletons.size(); ++i) {
        std::string base = i < res.skeletonNames.size() && !res.skeletonNames[i].empty()
                         ? res.skeletonNames[i] : stem;
        if (res.skeletons.size() > 1) base += "_" + std::to_string(i);
        sanitiseAssetName(base);
        const std::string outFile = destDir + "\\" + base + ".ocskel";
        const bool skelExists = std::filesystem::exists(outFile, ec);
        if (skelExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocskel' already exists - not overwritten", base);
        } else if (skelExists && !editor::moveToRecycleBin(outFile)) {
            AVER_WARN("[Import] '{}.ocskel' could not be replaced", base);
        } else if (!fmt::saveOcSkel(outFile, res.skeletons[i], &why)) {
            AVER_WARN("[Import] {}", why);
        } else {
            AVER_INFO("[Import] {} -> {} ({} bone(s))", std::filesystem::path(src).filename().string(),
                      base + ".ocskel", res.skeletons[i].bones.size());
            ++out.rigsWritten;
        }
    }
    for (usize i = 0; i < res.animations.size(); ++i) {
        std::string base = i < res.animationNames.size() && !res.animationNames[i].empty()
                         ? res.animationNames[i] : (stem + "_clip" + std::to_string(i));
        sanitiseAssetName(base);
        const std::string outFile = destDir + "\\" + base + ".ocanim";
        const bool animExists = std::filesystem::exists(outFile, ec);
        if (animExists && !overwrite) {
            AVER_WARN("[Import] '{}.ocanim' already exists - not overwritten", base);
        } else if (animExists && !editor::moveToRecycleBin(outFile)) {
            AVER_WARN("[Import] '{}.ocanim' could not be replaced", base);
        } else if (!fmt::saveOcAnim(outFile, res.animations[i], &why)) {
            AVER_WARN("[Import] {}", why);
        } else {
            AVER_INFO("[Import] {} -> {} ({:.2f}s, {} track(s))",
                      std::filesystem::path(src).filename().string(), base + ".ocanim",
                      res.animations[i].duration, res.animations[i].tracks.size());
            ++out.clipsWritten;
        }
    }
    return true;
}

#if AVER_WITH_IMGUI
// Returns the mounted roots: the project's Content, and the engine's source tree where present.
std::vector<SandboxApp::CbRoot> SandboxApp::cbRoots() const {
    std::vector<CbRoot> r;
    if (project_.valid()) r.push_back({"Content", project_.contentDir(), false});
    if (!cbEngineRoot_.empty()) r.push_back({"Engine", cbEngineRoot_, true});
    return r;
}

// Enters a folder and records it in the Back/Forward history.
void SandboxApp::cbNavigate(const std::string& dir) {
    if (dir.empty() || dir == cbSelectedDir_) return;
    if (cbHistoryPos_ >= 0 && cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()))
        cbHistory_.resize(static_cast<usize>(cbHistoryPos_) + 1);
    cbHistory_.push_back(dir);
    cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
    cbSelectedDir_ = dir;
    cbSelectedFile_.clear();
    cbSelection_.clear();
    cbFilter_[0] = '\0';
}

bool SandboxApp::cbCanBack() const { return cbHistoryPos_ > 0; }

bool SandboxApp::cbCanForward() const { return cbHistoryPos_ >= 0 &&
                                   cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()); }

void SandboxApp::cbBack()    { if (cbCanBack())    { cbSelectedDir_ = cbHistory_[static_cast<usize>(--cbHistoryPos_)]; cbClearSelection(); } }

void SandboxApp::cbForward() { if (cbCanForward()) { cbSelectedDir_ = cbHistory_[static_cast<usize>(++cbHistoryPos_)]; cbClearSelection(); } }

// Returns the parent folder, or empty at a mounted root.
std::string SandboxApp::cbParentDir() const {
    for (const CbRoot& r : cbRoots())
        if (cbSelectedDir_ == r.path) return {};
    std::error_code ec;
    std::filesystem::path p = std::filesystem::path(cbSelectedDir_).parent_path();
    if (p.empty()) return {};
    const std::string up = p.string();
    for (const CbRoot& r : cbRoots())
        if (up.size() >= r.path.size() && up.compare(0, r.path.size(), r.path) == 0) return up;
    return {};
}

// True for extensions a double-click should hand to the IDE rather than the shell.
 bool SandboxApp::cbIsSourceFile(const std::string& ext) {
    static const char* kSource[] = {
        ".cs", ".cpp", ".cxx", ".cc", ".c", ".hpp", ".hxx", ".h", ".inl",
        ".hlsl", ".hlsli", ".glsl", ".json", ".xml", ".csproj", ".txt", ".md", ".ini", ".cmake"};
    for (const char* s : kSource) if (ext == s) return true;
    return false;
}

// Returns the IDE the browser opens source with: the user's choice, else the detected preference.
// STAYS INSIDE the UI block with the rest of the content browser: it touches no ImGui itself, but
// its neighbours (cbIsEditable/cbInvalidate/importModel) do, so the callers are guarded instead.
const editor::IdeInfo& SandboxApp::cbIde() const {
    const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
    if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size())) return ides[static_cast<usize>(cbIdeChoice_)];
    return editor::preferredIde();
}

// Handles a double-click: enter a folder, open an asset editor, else the IDE, else the shell.
void SandboxApp::cbOpenEntry(const std::string& full, bool isDir) {
    if (isDir) { cbNavigate(full); return; }
    const std::string ext = lowerExt(std::filesystem::path(full));
    // A level opens in the editor: without this special case it fell through to openWithShell() and
    // opened in Notepad (no AssetEditor factory is registered for .ocworld/.ocmap) -- despite the
    // Content Browser giving it its own "Level" icon and colour ~200 lines below. Deferred through
    // requestOpenLevel since this function has no Engine& to load with.
    if (editor::isLevelPath(full)) {
#if AVER_MODULE_SCENE
        if (requestOpenLevel(full, "opened from the Content Browser"))
            cbStatus_ = "Opening " + std::filesystem::path(full).stem().string();
        else
            cbStatus_ = openLevelError_;
#else
        // requestOpenLevel/openLevelError_ need AVER_MODULE_SCENE (levels only instantiate into
        // scene::World). Falling through to openWithShell() would reintroduce the Notepad bug above.
        cbStatus_ = "This build has no Scene module, so levels cannot be opened";
#endif
        return;
    }
    if (assetEditors_.open(full)) { cbStatus_ = "Opened in the asset editor"; return; }
    if (cbIsSourceFile(ext)) {
        const editor::IdeInfo& ide = cbIde();
        if (editor::openInIde(ide, full)) { cbStatus_ = "Opened in " + ide.name; return; }
        cbStatus_ = "Could not open in " + ide.name;
        return;
    }
    cbStatus_ = editor::openWithShell(full) ? "Opened" : "Nothing is registered to open that";
}

// Drops a folder's cached listing so the next frame re-reads it.
void SandboxApp::cbInvalidate(const std::string& dir) { dirCache_.erase(dir); }

// False for engine content, which the browser mounts read-only.
bool SandboxApp::cbIsEditable(const std::string& path) const { return !isEnginePath(path); }

// A free "<stem>[N].<ext>" in the selected folder, or empty if the folder is read-only or a
// thousand names are taken. NEVER returns a path that exists: an existing NewGraph.ocgraph may be
// somebody's work in progress, and silently replacing it is the one outcome this must not have.
std::filesystem::path SandboxApp::cbFreeAssetPath(const char* stem, const char* ext) {
    if (!cbIsEditable(cbSelectedDir_)) { cbStatus_ = cbImportBlockedReason(cbSelectedDir_); return {}; }
    std::error_code ec;
    for (int n = 0; n < 1000; ++n) {
        const std::string name = std::string(stem) + (n == 0 ? "" : std::to_string(n)) + ext;
        std::filesystem::path candidate = std::filesystem::path(cbSelectedDir_) / name;
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    cbStatus_ = std::string("Could not find a free name for a new ") + stem;
    return {};
}

// The tail every successful create runs: refresh the folder cache, select the new file, open its
// editor (selection was the missing part; cbCreateSoundGraph used to leave the highlight stale).
void SandboxApp::cbAdoptNewAsset(const std::filesystem::path& target, bool openEditor) {
    cbInvalidate(cbSelectedDir_);
    cbSelectedFile_ = target.string();
    if (openEditor) assetEditors_.open(target.string());
    cbStatus_ = "Created " + target.filename().string();
}

// Writes a new material SOURCE file -- an [AverMaterial] C# class -- into the selected folder.
// fmt::newMaterialScript existed, covered by MaterialTest, with zero production callers until this
// menu item -- docs/EDITOR.md said so outright ("There is no New Material menu item, although
// fmt::newMaterialScript exists and will write the whole file"). No editor is opened: the .cs
// compiles to .ocmat via Compile C#, and the only editor that claims a .cs is the Actor editor,
// which needs an [AverActor] class this file lacks. Guarded by AVER_MODULE_PBR: the starter
// pbr::MaterialDesc pulls in aver/pbr/Material.hpp via OcMat.hpp (MaterialScript.hpp), unreachable
// otherwise.
#if AVER_MODULE_PBR
void SandboxApp::cbCreateMaterial() {
    const std::filesystem::path target = cbFreeAssetPath("NewMaterial", ".cs");
    if (target.empty()) return;

    // The bound name is what a mesh names, and the M_ prefix is the tree's convention --
    // newMaterialScript strips it again for the class name.
    const std::string stem  = target.stem().string();
    const std::string bound = stem.rfind("M_", 0) == 0 ? stem : "M_" + stem;
    // Through csharpNamespaceFor: a project name is a folder name and may contain spaces --
    // pasting one straight after `namespace ` ("My Game") produced C# that could not compile.
    const std::string ns    = (project_.valid() && !project_.name.empty())
                                  ? editor::csharpNamespaceFor(project_.name) + ".Materials"
                                  : std::string("Materials");

    pbr::MaterialDesc d;   // engine defaults: the starter is plain, not a guess at intent
    const std::string text = fmt::newMaterialScript(bound, ns, d, nullptr);

    std::string why;
    if (!editor::writeNewFile(target.string(), text, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new material failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target, /*openEditor=*/false);
    cbStatus_ = "Created " + target.filename().string() + " (" + bound + ") - Compile C# to build its .ocmat";
}
#endif

// Writes a starter .ocsnd into the selected folder and opens it. See the Add menu's own comment
// for why this exists at all.
void SandboxApp::cbCreateSoundGraph() {
    const std::filesystem::path target = cbFreeAssetPath("NewSound", ".ocsnd");
    if (target.empty()) return;
    std::string why;
    if (!fmt::saveOcSound(target.string(), editor::snStarterGraph(), &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new sound graph failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_PARTICLES
void SandboxApp::cbCreateParticleEffect() {
    const std::filesystem::path target = cbFreeAssetPath("NewParticle", ".ocparticle");
    if (target.empty()) return;
    fmt::OcParticleExtras extras;
    const particles::ParticleEffect fx = editor::pxStarterEffect(&extras);
    std::string why;
    if (!fmt::saveOcparticle(target.string(), fx, &extras, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new particle effect failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

#endif
#endif

#if AVER_WITH_IMGUI
// Writes a starter .ocfoliage (one foliage type) and opens it. Same shape as cbCreateParticleEffect
// above, but unconditional: editor::foliageStarterType/fmt::saveOcFoliage are reachable in every
// module configuration (see OcFoliage.hpp on why foliage needs neither Aver.Scene nor Aver.Landscape).
void SandboxApp::cbCreateFoliageType() {
    const std::filesystem::path target = cbFreeAssetPath("NewFoliageType", ".ocfoliage");
    if (target.empty()) return;
    fmt::OcFoliageData starter = editor::foliageStarterType();
#if AVER_MODULE_SCENE
    // Judgment call: defaults the starter's mesh to one already loaded in the level (rather than
    // the format's own placeholder, Meshes/cube.ocmesh, which most projects don't have on disk).
    // meshPathById_ is exactly the meshes already resolved into the scene, so a type created this
    // way is paintable immediately -- the point of this button. Lowest path wins for determinism
    // (not "whichever the unordered_map iterates first"); the trade is the chosen mesh may not be
    // what the author meant to scatter, acceptable since it's a starting point -- meshPath can be
    // repointed from the tab this still opens.
    if (!meshPathById_.empty()) {
        std::string best;
        for (const auto& kv : meshPathById_)
            if (best.empty() || kv.second < best) best = kv.second;
        starter.meshPath = best;
    }
#endif
    std::string why;
    if (!fmt::saveOcFoliage(target.string(), starter, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new foliage type failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
#if AVER_MODULE_LANDSCAPE
    // Immediate, not lazy: refreshFoliagePalette() is otherwise only called from
    // loadProjectMeshes() and setEditorMode() -- without this, a type created while the Foliage
    // panel is already open would not appear until one of those ran again.
    refreshFoliagePalette();
#endif
}

// Writes a starter .ocinput (Input Scheme) and opens it. Unconditional, like cbCreateFoliageType
// above (see OcInput.hpp's "NO ENGINE DEPENDENCY, DELIBERATELY"). Text via fmt::writeOcinput --
// OcInput has no POD save* wrapper; writeNewFile refuses to touch an existing file, matching
// cbCreateNodeGraph below.
void SandboxApp::cbCreateInputScheme() {
    const std::filesystem::path target = cbFreeAssetPath("NewInputScheme", ".ocinput");
    if (target.empty()) return;
    const std::string text = fmt::writeOcinput(editor::inputSchemeStarterData());

    std::string why;
    if (!editor::writeNewFile(target.string(), text, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new input scheme failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

// Writes a starter .ocgraph (Aver Node visual-scripting graph) and opens it. Bytes come from
// editor::graphStarterText so a test can parse exactly what this writes; text, not fmt::saveOcgraph,
// because the C++ OcGraphData struct doesn't model the CLASS record. writeNewFile refuses to overwrite.
void SandboxApp::cbCreateNodeGraph() {
    const std::filesystem::path target = cbFreeAssetPath("NewGraph", ".ocgraph");
    if (target.empty()) return;
    const std::string text = editor::graphStarterText(target.stem().string());

    std::string why;
    if (!editor::writeNewFile(target.string(), text, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new graph failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

// Writes a starter .ocbt -- a Synapse behaviour tree -- and opens it. The tree comes from
// editor::btStarterTree, which must be a VALID tree, not an empty file: BtEditor refuses a file it
// cannot load, so an invalid starter fails one step after the moment that looks like success.
void SandboxApp::cbCreateBehaviourTree() {
    const std::filesystem::path target = cbFreeAssetPath("NewBehaviour", ".ocbt");
    if (target.empty()) return;

    const fmt::OcBtData bt = editor::btStarterTree();

    // Checked here rather than trusted: valid() is the same predicate the loader applies, so a
    // starter that fails it would be written and then refused by the editor that just opened it.
    if (!bt.valid()) {
        cbStatus_ = "Internal error: the starter behaviour tree is not valid";
        AVER_ERROR("[Editor] starter .ocbt failed OcBtData::valid()");
        return;
    }

    std::string why;
    if (!fmt::saveOcBt(target.string(), bt, &why)) {
        cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
        AVER_ERROR("[Editor] new behaviour tree failed: {}", why);
        return;
    }
    cbAdoptNewAsset(target);
}

// Explains why `dir` cannot be imported/created into, or empty if it can.
std::string SandboxApp::cbImportBlockedReason(const std::string& dir) const {
    if (cbIsEditable(dir)) return {};
    const std::string name = std::filesystem::path(dir).filename().string();
    return "'" + (name.empty() ? dir : name) + "' is engine content and is read-only.";
}

// Renames a file or folder and follows the rename in the selection and the history.
// The one drag payload a Content Browser item sends: everything selected, unfiltered (see the
// viewport drop target for why there's only one). A drag starting outside the selection carries
// just that item, since grabbing an unselected file is unambiguously about that file.
std::string SandboxApp::cbMoveDragPayloadFor(const std::string& dragged) const {
    if (!cbIsSelected(dragged) || cbSelection_.size() <= 1) return dragged;
    std::string blob;
    for (const std::string& p : cbSelection_) {
        if (!blob.empty()) blob += char(10);
        blob += p;
    }
    return blob.empty() ? dragged : blob;
}

// True when `path` is `dir` itself or lives somewhere beneath it.
 bool SandboxApp::cbIsUnder(const std::string& path, const std::string& dir) {
    std::error_code ec;
    const auto a = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);
    if (ec) return false;
    const auto b = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec) return false;
    auto ai = a.begin();
    auto bi = b.begin();
    for (; ai != a.end(); ++ai, ++bi) {
        if (bi == b.end() || *ai != *bi) return false;
    }
    return true;
}

// A drop target on a folder, offered only when the drop would mean something.
// Peeks before it opens (same idiom as drawOutlinerDropTarget, to refuse a cycle-making reparent
// before ever lighting up): declines the folder items already live in, a folder dragged onto
// itself, and a folder dragged onto its own descendant.
void SandboxApp::cbFolderDropTarget(const std::string& folderPath) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (!peek || !peek->IsDataType(kCbMoveDragDropType) || !peek->Data) return;
    if (!cbIsEditable(folderPath)) return;   // engine content is read-only
    const std::vector<std::string> srcs =
        editor::splitDropPayload(std::string(static_cast<const char*>(peek->Data)));
    if (srcs.empty()) return;

    bool anyUseful = false;
    for (const std::string& sp : srcs) {
        std::error_code ec;
        const std::filesystem::path p(sp);
        if (p.parent_path().string() == folderPath) continue;                     // already here
        if (std::filesystem::is_directory(p, ec) && cbIsUnder(folderPath, sp)) return;  // into itself
        anyUseful = true;
    }
    if (!anyUseful) return;

    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kCbMoveDragDropType)) {
        if (pl->Data) {
            // Latched, not acted on: a modal can't open mid-drag (ImGui gesture vs popup), so this
            // records the request and cbFileOpModals opens it next frame. Same shape as
            // cbWantRename_ and outlinerDeleteRequest_.
            cbMoveSources_ = editor::splitDropPayload(std::string(static_cast<const char*>(pl->Data)));
            cbMoveDest_ = folderPath;
            cbWantMoveOrCopy_ = true;
        }
    }
    ImGui::EndDragDropTarget();
}

// Drops any dragged path that lives inside another dragged folder. A directory move/copy carries
// its contents, so a selection with both would move the child twice -- once with its parent, then
// again from a path that no longer exists, reporting a false failure.
std::vector<std::string> SandboxApp::cbPruneNested(const std::vector<std::string>& in) const {
    std::vector<std::string> out;
    for (const std::string& p : in) {
        bool nested = false;
        for (const std::string& q : in) {
            if (p == q) continue;
            std::error_code ec;
            if (std::filesystem::is_directory(std::filesystem::path(q), ec) && cbIsUnder(p, q)) {
                nested = true;
                break;
            }
        }
        if (!nested) out.push_back(p);
    }
    return out;
}

// Copies one entry into `destDir`. Refuses a collision rather than overwriting, matching
// cbRenameEntry and importAsset -- clobbering somebody's file is not a thing to do quietly.
bool SandboxApp::cbCopyEntryTo(const std::string& src, const std::string& destDir) {
    std::error_code ec;
    const std::filesystem::path s(src);
    const std::filesystem::path d = std::filesystem::path(destDir) / s.filename();
    if (std::filesystem::exists(d, ec)) {
        cbStatus_ = "'" + s.filename().string() + "' is already in that folder";
        return false;
    }
    if (std::filesystem::is_directory(s, ec))
        std::filesystem::copy(s, d, std::filesystem::copy_options::recursive, ec);
    else
        std::filesystem::copy_file(s, d, ec);
    if (ec) { cbStatus_ = "Copy failed: " + ec.message(); return false; }
    cbInvalidate(destDir);
    return true;
}

// Moves one entry into `destDir`, and repairs what pointed at its old path.
bool SandboxApp::cbMoveEntryTo(const std::string& src, const std::string& destDir) {
    std::error_code ec;
    const std::filesystem::path s(src);
    const std::filesystem::path d = std::filesystem::path(destDir) / s.filename();
    if (!cbIsEditable(src)) { cbStatus_ = "Engine content cannot be moved"; return false; }
    if (std::filesystem::exists(d, ec)) {
        cbStatus_ = "'" + s.filename().string() + "' is already in that folder";
        return false;
    }
    std::filesystem::rename(s, d, ec);
    if (ec) { cbStatus_ = "Move failed: " + ec.message(); return false; }
    // Both folders invalidated: unlike rename/duplicate/create (which stay put), a move empties one
    // and fills another -- forgetting the source leaves a ghost tile that opens nothing.
    cbInvalidate(s.parent_path().string());
    cbInvalidate(destDir);
    // The same bookkeeping cbRenameEntry does, for the same reason: here the path IS the identity.
    if (cbSelectedDir_ == src)  cbSelectedDir_ = d.string();
    if (cbSelectedFile_ == src) cbSelectedFile_ = d.string();
    for (std::string& sp : cbSelection_) if (sp == src) sp = d.string();
    cbRewriteHistory(src, d.string());
    return true;
}

// Content-relative, forward-slashed: the form a level's PLACE record and content_'s meshes key on.
// Returns the input unchanged when it is not under the content root (no level can be naming it).
// Who references this asset -- closes a foot-gun: every reference is a content-relative path, and
// ObjectIds are fnv1a64 of exactly that string (GameContent.cpp), so deleting or renaming an asset
// silently breaks every reference to it -- and the delete confirm said only "It goes to the recycle
// bin, so it can be restored", true but answering a different question than the one an author needs
// answered. A scan, not an index (deliberate limit): a real registry -- built at project load, kept
// current by the content watcher -- would be a subsystem; this walks the text assets on demand,
// once, when something is about to be destroyed, which is the one moment a directory walk over
// thousands of files is obviously the right trade. Text formats only: .ocworld/.ocmap (MESH/MATERIAL),
// .ocmat (TEX, GRAPHREF), .ocgraph (mesh=, material=, effect=, path=), .ocproject (STARTMAP and
// friends); a binary .ocmesh names nothing. Cannot see a reference built at runtime from a computed
// string, or one held only as a hashed ObjectId with no path on disk -- so this reports "found N",
// not "there are exactly N".
std::vector<std::string> SandboxApp::cbFindReferencesTo(const std::string& absPath) const {
    std::vector<std::string> out;
    if (!project_.valid()) return out;
    const std::string needle = cbRelativeToContent(absPath);
    if (needle.empty() || needle == absPath) return out;   // outside the content root

    // Compared case-insensitively and with both separators: a hand-authored .ocgraph may say
    // Meshes\Cube.ocmesh where the browser reports Meshes/Cube.ocmesh; a miss reads as no references.
    const std::string want = editor::normaliseForRefScan(needle);

    std::error_code ec;
    const std::string content = project_.contentDir();
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string p = it->path().string();
        if (p == absPath) continue;                       // a file does not reference itself
        const std::string ext = lowerExt(it->path());
        if (ext != ".ocworld" && ext != ".ocmap" && ext != ".ocmat" &&
            ext != ".ocgraph" && ext != ".ocproject") continue;
        std::string text;
        if (!readFileText(p, text)) continue;
        // Anchored, not a bare find(): `Meshes/Cube.ocmesh` is a substring of `PropMeshes/Cube.ocmesh`
        // and `Sub/Meshes/Cube.ocmesh`, both a different file -- a plain substring test reported
        // referrers of an unrelated asset. Noisy for a warning, destructive for a rewrite; the
        // boundary rule lives in a tested header (AssetRefScan.hpp).
        const std::string hay = editor::normaliseForRefScan(text);
        if (editor::referencesAsset(hay, want)) out.push_back(cbRelativeToContent(p));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

SandboxApp::RefRewriteReport SandboxApp::cbRewriteReferences(const std::string& oldAbs, const std::string& newAbs) {
    RefRewriteReport rep;
    if (!project_.valid()) return rep;
    const std::string oldRel = cbRelativeToContent(oldAbs);
    const std::string newRel = cbRelativeToContent(newAbs);
    if (oldRel.empty() || newRel.empty() || oldRel == oldAbs || newRel == newAbs) return rep;
    if (oldRel == newRel) return rep;

    // Scans for the old path -- the same set of files the warning listed, so what the dialog
    // promised is exactly what gets edited. The rename has already happened on disk by this point.
    for (const std::string& refRel : cbFindReferencesTo(oldAbs)) {
        const std::string abs = project_.contentDir() + "\\" + refRel;
        std::string text;
        if (!readFileText(abs, text)) { ++rep.filesFailed; continue; }
        const editor::RefRewrite r = editor::rewriteAssetRefs(text, oldRel, newRel);
        if (r.count == 0) continue;                    // matched the scan, changed nothing: leave it alone
        if (!writeFileTextAtomic(abs, r.text)) { ++rep.filesFailed; continue; }
        ++rep.filesChanged;
        rep.refsRewritten += r.count;
        AVER_INFO("[Editor] repointed {} reference(s) in '{}' from '{}' to '{}'",
                  r.count, refRel, oldRel, newRel);
    }
    return rep;
}

std::string SandboxApp::cbRelativeToContent(const std::string& abs) const {
    if (!project_.valid()) return abs;
    std::error_code ec;
    const auto rel = std::filesystem::relative(std::filesystem::path(abs),
                                               std::filesystem::path(project_.contentDir()), ec);
    if (ec || rel.empty()) return abs;
    std::string out = rel.generic_string();
    return out;
}

// Run after a copy or a move, whichever way it went.
// A mesh reload is needed for BOTH, easy to miss by assuming only a move matters: content_'s
// meshes are keyed by fnv1a64 of the content-relative path, so a copy creates a new unregistered id
// (tile draws the type glyph, drag finds no mesh); a move invalidates the old id the same way.
void SandboxApp::cbAfterMoveOrCopy(const std::vector<std::string>& srcs) {
    for (const std::string& sp : srcs) {
        const std::string ext = lowerExt(std::filesystem::path(sp));
        if (ext == ".ocmesh" || ext == ".ocparticle") { wantMeshReload_ = true; break; }
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::path(sp), ec)) { wantMeshReload_ = true; break; }
    }
    cbMoveSources_.clear();
    cbMoveDest_.clear();
}

void SandboxApp::cbRenameEntry(const std::string& from, const std::string& newName, bool repointRefs) {
    if (newName.empty()) return;
    std::error_code ec;
    const std::filesystem::path src(from);
    const std::filesystem::path dst = src.parent_path() / newName;
    if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + newName + "' already exists"; return; }
    std::filesystem::rename(src, dst, ec);
    if (ec) { cbStatus_ = "Rename failed: " + ec.message(); return; }

    // AFTER the filesystem rename, never before: if the rename fails there is nothing to repoint,
    // and repointing first would leave every referrer naming a file that does not exist yet.
    RefRewriteReport rep;
    if (repointRefs) rep = cbRewriteReferences(from, dst.string());
    cbInvalidate(src.parent_path().string());
    if (cbSelectedDir_ == from)  { cbSelectedDir_ = dst.string(); }
    if (cbSelectedFile_ == from) { cbSelectedFile_ = dst.string(); }
    for (std::string& sp : cbSelection_) if (sp == from) sp = dst.string();
    cbRewriteHistory(from, dst.string());

    // The same id invalidation a move causes (rename IS a move within a folder): content_'s meshes
    // are keyed by fnv1a64 of the content-relative path, so renaming retires the old id and creates
    // one nothing has registered -- previously this reloaded nothing, so the renamed mesh kept drawing
    // under its old id until the next project open, and a fresh drag of it found no mesh at all.
    // Directories included: renaming one changes every path beneath it.
    {
        const std::string ext = lowerExt(dst);
        std::error_code dec;
        if (ext == ".ocmesh" || ext == ".ocparticle" || std::filesystem::is_directory(dst, dec))
            wantMeshReload_ = true;
    }

    // Reports partial failure separately from success: N separate writes, not transactional, so
    // "3 of 4" is a state the author needs to see.
    if (!repointRefs) {
        cbStatus_ = "Renamed to " + newName;
    } else if (rep.filesFailed) {
        cbStatus_ = "Renamed to " + newName + " -- repointed " + std::to_string(rep.refsRewritten) +
                    " reference(s) in " + std::to_string(rep.filesChanged) + " file(s), but " +
                    std::to_string(rep.filesFailed) + " file(s) could NOT be updated";
        AVER_ERROR("[Editor] {} file(s) referencing '{}' could not be rewritten -- they still name "
                   "the old path", rep.filesFailed, from);
    } else if (rep.filesChanged) {
        cbStatus_ = "Renamed to " + newName + " -- repointed " + std::to_string(rep.refsRewritten) +
                    " reference(s) in " + std::to_string(rep.filesChanged) + " file(s)";
    } else {
        cbStatus_ = "Renamed to " + newName;
    }
}

// Rewrites history entries under `from` to `to`, dropping them when `to` is empty.
void SandboxApp::cbRewriteHistory(const std::string& from, const std::string& to) {
    std::vector<std::string> kept;
    kept.reserve(cbHistory_.size());
    const std::string current = cbHistoryPos_ >= 0 && cbHistoryPos_ < static_cast<int>(cbHistory_.size())
                              ? cbHistory_[static_cast<usize>(cbHistoryPos_)] : std::string();
    std::string newCurrent = current;
    for (const std::string& h : cbHistory_) {
        const bool under = h.size() >= from.size() && h.compare(0, from.size(), from) == 0 &&
                           (h.size() == from.size() || h[from.size()] == '\\' || h[from.size()] == '/');
        std::string next = h;
        if (under) {
            if (to.empty()) { if (h == current) newCurrent.clear(); continue; }
            next = to + h.substr(from.size());
        }
        if (h == current) newCurrent = next;
        if (kept.empty() || kept.back() != next) kept.push_back(next);
    }
    cbHistory_.swap(kept);
    cbHistoryPos_ = -1;
    for (usize i = 0; i < cbHistory_.size(); ++i)
        if (cbHistory_[i] == newCurrent) { cbHistoryPos_ = static_cast<int>(i); break; }
    if (cbHistoryPos_ < 0 && !cbHistory_.empty()) cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
}

// Copies a file or folder alongside itself as "<name>2", "<name>3", ...
void SandboxApp::cbDuplicateEntry(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path src(path);
    const std::string stem = src.stem().string(), ext = src.extension().string();
    std::filesystem::path dst;
    for (int n = 2; n < 1000; ++n) {
        dst = src.parent_path() / (stem + std::to_string(n) + ext);
        if (!std::filesystem::exists(dst, ec)) break;
    }
    if (std::filesystem::is_directory(src, ec))
        std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
    else
        std::filesystem::copy_file(src, dst, ec);
    if (ec) { cbStatus_ = "Duplicate failed: " + ec.message(); return; }
    cbInvalidate(src.parent_path().string());
    cbStatus_ = "Duplicated as " + dst.filename().string();
}

// Moves a file or folder to the recycle bin and drops it from the selection and history.
void SandboxApp::cbDeleteEntry(const std::string& path) {
    const std::filesystem::path src(path);
    const std::string parent = src.parent_path().string();
    if (!editor::moveToRecycleBin(path)) { cbStatus_ = "Could not delete " + src.filename().string(); return; }
    cbInvalidate(parent);
    if (cbSelectedFile_ == path) cbSelectedFile_.clear();
    cbSelection_.erase(std::remove(cbSelection_.begin(), cbSelection_.end(), path),
                       cbSelection_.end());
    if (cbSelectedDir_ == path) cbSelectedDir_ = parent;
    cbRewriteHistory(path, std::string());
    cbStatus_ = "Moved " + src.filename().string() + " to the recycle bin";
}

// Creates a folder under parent. Refuses engine content.
void SandboxApp::cbCreateFolder(const std::string& parent, const std::string& name) {
    if (name.empty()) return;
    if (!cbIsEditable(parent)) { cbStatus_ = "Engine content is read-only"; return; }
    std::error_code ec;
    const std::filesystem::path dst = std::filesystem::path(parent) / name;
    if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + name + "' already exists"; return; }
    std::filesystem::create_directory(dst, ec);
    if (ec) { cbStatus_ = "Could not create folder: " + ec.message(); return; }
    cbInvalidate(parent);
    cbStatus_ = "Created " + name;
}

// Draws the right-click menu for one Content Browser entry.
void SandboxApp::cbItemContextMenu(const std::string& full, const std::string& name, bool isDir) {
    if (!ImGui::BeginPopupContextItem("##cbitemctx")) return;
    const bool editable = cbIsEditable(full);
    ImGui::TextDisabled("%s", name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(isDir ? "Open" : "Open in editor", "Double-click")) cbOpenEntry(full, isDir);
    if (!isDir) {
        if (ImGui::BeginMenu("Open With")) {
            for (usize i = 0; i < editor::detectedIdes().size(); ++i) {
                const editor::IdeInfo& ide = editor::detectedIdes()[i];
                if (ImGui::MenuItem(ide.name.c_str()))
                    cbStatus_ = editor::openInIde(ide, full) ? "Opened in " + ide.name
                                                            : "Could not open in " + ide.name;
            }
            ImGui::EndMenu();
        }
    }
    if (ImGui::MenuItem("Show in Explorer")) editor::revealInFileManager(full);
    if (ImGui::MenuItem("Copy Path")) { ImGui::SetClipboardText(full.c_str()); cbStatus_ = "Path copied"; }
    // Find References on demand: the scan already existed but could only be reached from the
    // delete/rename confirms, so "what uses this?" was askable only after deciding to remove it.
    if (!isDir && ImGui::MenuItem("Find References")) {
        refPanelAsset_ = full;
        refPanelResults_ = cbFindReferencesTo(full);
        refPanelScanned_ = true;
        showReferences_ = true;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!editable);
    if (ImGui::MenuItem("Rename", "F2")) {
        cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantRename_ = true;
        std::snprintf(cbRenameBuf_, sizeof cbRenameBuf_, "%s", name.c_str());
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantDuplicate_ = true; }
    if (ImGui::MenuItem("Delete", "Del")) { cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantDelete_ = true; }
    ImGui::EndDisabled();
    if (!editable) ImGui::TextDisabled("Engine content is read-only here.");
    ImGui::EndPopup();
}

// True for a path inside the Engine root -- what earns the Module folder icon.
bool SandboxApp::isEnginePath(const std::string& p) const {
    return !cbEngineRoot_.empty() && p.size() >= cbEngineRoot_.size() &&
           p.compare(0, cbEngineRoot_.size(), cbEngineRoot_) == 0;
}

// Draws the Content Browser: navigation, Add and Import, and a folder tree beside a file view.
void SandboxApp::drawContentBrowser() {
    const std::vector<CbRoot> roots = cbRoots();
    if (roots.empty()) {
        ImGui::TextDisabled("No project loaded - nothing is mounted.");
        ImGui::TextDisabled("Create or open a project (File menu) to browse its Content folder.");
        return;
    }
    if (cbSelectedDir_.empty()) cbNavigate(roots.front().path);
    if (!drawerStartSub_.empty()) {
        // Canonicalised so separators and case match what the tree builds from directory_iterator.
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path target = fs::canonical(fs::path(roots.front().path) / drawerStartSub_, ec);
        if (ec) AVER_WARN("[Sandbox] --drawer content:{}: no such folder under Content", drawerStartSub_);
        else    cbNavigate(target.string());
        drawerStartSub_.clear();
    }
    ImGui::BeginDisabled(!cbCanBack());
    if (ImGui::ArrowButton("##cbback", ImGuiDir_Left)) cbBack();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back");
    ImGui::SameLine(0.0f, 2.0f*dpi_);
    ImGui::BeginDisabled(!cbCanForward());
    if (ImGui::ArrowButton("##cbfwd", ImGuiDir_Right)) cbForward();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward");
    ImGui::SameLine(0.0f, 2.0f*dpi_);
    const std::string parent = cbParentDir();
    ImGui::BeginDisabled(parent.empty());
    if (ImGui::ArrowButton("##cbup", ImGuiDir_Up)) cbNavigate(parent);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(parent.empty() ? "Already at the root" : "Up one folder");
    ImGui::SameLine();

    if (ImGui::Button("+ Add")) ImGui::OpenPopup("cbAddMenu");
    if (ImGui::BeginPopup("cbAddMenu")) {
        ImGui::BeginDisabled(!cbIsEditable(cbSelectedDir_));
        if (ImGui::MenuItem("New Folder")) { cbWantNewFolder_ = true; cbNewFolderBuf_[0] = '\0'; }
        ImGui::EndDisabled();
        ImGui::Separator();
        // These four ignore the selected folder (says so, rather than letting "+ Add" imply "add
        // here"): scripts land in the project's Scripts folder, C++ in modules\. The C++ pair needs
        // no project -- it writes into the engine tree -- so it stays enabled when the C# pair isn't.
        ImGui::TextDisabled("  Written to a fixed location, not this folder");
        ImGui::BeginDisabled(!project_.valid());
        if (ImGui::MenuItem("New C# Script...")) tools_.openNewCsScript();
        uiReg_.track("cb.add.csScript");
        if (ImGui::MenuItem("New C# Class..."))  tools_.openNewCsClass();
        uiReg_.track("cb.add.csClass");
        ImGui::EndDisabled();
        if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Open or create a project first - a script belongs to one.");
        ImGui::Separator();
        if (ImGui::MenuItem("New C++ Module...")) tools_.openNewCppModule();
        uiReg_.track("cb.add.cppModule");
        if (ImGui::MenuItem("New C++ Class..."))  tools_.openNewCppClass();
        uiReg_.track("cb.add.cppClass");
        ImGui::Separator();
        // A sound graph must be creatable from here or its editor is unreachable: .ocsnd was the
        // first format the editor could edit but nothing could produce -- the "built through every
        // layer, read by nothing" defect again. Same argument for .ocgraph and .ocbt: both had a
        // working editor tab and icon but no way to be brought into existence.
        ImGui::BeginDisabled(!cbIsEditable(cbSelectedDir_));
        if (ImGui::MenuItem("New Aver Node Graph")) cbCreateNodeGraph();
        uiReg_.track("cb.add.nodeGraph");
        if (ImGui::MenuItem("New Behaviour Tree"))  cbCreateBehaviourTree();
        uiReg_.track("cb.add.behaviourTree");
        if (ImGui::MenuItem("New Sound Graph"))     cbCreateSoundGraph();
        uiReg_.track("cb.add.soundGraph");
#if AVER_MODULE_PARTICLES
        if (ImGui::MenuItem("New Particle Effect")) cbCreateParticleEffect();
        uiReg_.track("cb.add.particleEffect");
#endif
        if (ImGui::MenuItem("New Foliage Type"))    cbCreateFoliageType();
        uiReg_.track("cb.add.foliageType");
        if (ImGui::MenuItem("New Input Scheme"))    cbCreateInputScheme();
        uiReg_.track("cb.add.inputScheme");
#if AVER_MODULE_PBR
        // cbCreateMaterial doesn't exist with the material system compiled out (see its guard
        // above), so this menu entry is guarded the same way.
        if (ImGui::MenuItem("New Material"))        cbCreateMaterial();
        uiReg_.track("cb.add.material");
#endif
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
        const std::string reason = cbImportBlockedReason(cbSelectedDir_);
        ImGui::BeginDisabled(!reason.empty());
        if (ImGui::Button("Import...")) cbWantImport_ = true;
        ImGui::EndDisabled();
        if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", reason.c_str());
    }
    drawImportModal();

    // View controls, right-aligned: Tiles/List, and the tile zoom when tiles are showing.
    {
        auto viewTab = [&](const char* label, bool active) {
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const bool hit = ImGui::Button(label);
            if (active) ImGui::PopStyleColor();
            return hit;
        };
        const f32 controls = cbGallery_ ? 250.0f : 130.0f;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::fmax(80.0f*dpi_,
            ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - (controls + 14.0f)*dpi_));
        ImGui::InputTextWithHint("##cbsearch",
                                 cbSearchDeep_ ? "Search this folder and below..."
                                               : "Search this folder...",
                                 cbFilter_, sizeof(cbFilter_));
        ImGui::SameLine();
        // Off by default: the search box keeps meaning what it always meant until asked for more.
        // Hint text changes with it, since silently searching more than the visible folder confuses.
        ImGui::Checkbox("Subfolders", &cbSearchDeep_);
        uiReg_.track("contentBrowser.searchDeep");

        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - controls*dpi_));
        if (viewTab("Tiles", cbGallery_)) cbGallery_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gallery view");
        ImGui::SameLine(0.0f, 2.0f*dpi_);
        if (viewTab("List", !cbGallery_)) cbGallery_ = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("List view");
        if (cbGallery_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f*dpi_);
            ImGui::SliderFloat("##cbzoom", &cbTileSize_, 56.0f, 168.0f, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile size");
        }
    }
    ImGui::Separator();

    const f32 footerH = ImGui::GetTextLineHeightWithSpacing() + 6.0f*dpi_;
    ImGui::BeginChild("cbTree", ImVec2(220.0f * dpi_, -footerH), true);
    for (const CbRoot& r : roots) {
        ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!r.engine) rootFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (cbSelectedDir_ == r.path) rootFlags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushID(r.label);
        const bool open = ImGui::TreeNodeEx(r.label, rootFlags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cbNavigate(r.path);
        cbFolderDropTarget(r.path);
        if (open) { drawFolderTree(r.path); ImGui::TreePop(); }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("cbFiles", ImVec2(0, -footerH), true);
    drawFolderFiles(cbSelectedDir_);
    if (ImGui::BeginPopupContextWindow("##cbbgctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        const std::string reason = cbImportBlockedReason(cbSelectedDir_);
        ImGui::BeginDisabled(!reason.empty());
        if (ImGui::MenuItem("New Folder")) { cbWantNewFolder_ = true; cbNewFolderBuf_[0] = '\0'; }
        if (ImGui::MenuItem("Import...")) cbWantImport_ = true;
        ImGui::EndDisabled();
        if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", reason.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) editor::revealInFileManager(cbSelectedDir_);
        if (ImGui::MenuItem("Copy Path")) { ImGui::SetClipboardText(cbSelectedDir_.c_str()); cbStatus_ = "Path copied"; }
        if (ImGui::MenuItem("Refresh")) cbInvalidate(cbSelectedDir_);
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    cbFooter();
    cbShortcuts();
    cbFileOpModals();
}

// Draws the browser footer: the folder's counts, the selection, and the last operation's outcome.
void SandboxApp::cbFooter() {
    const DirListing& l = dirListing(cbSelectedDir_);
    const usize files = l.entries.size() - l.dirCount;
    ImGui::Text("%zu folder%s, %zu file%s", l.dirCount, l.dirCount == 1 ? "" : "s",
                files, files == 1 ? "" : "s");
    if (!cbSelectedFile_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", std::filesystem::path(cbSelectedFile_).filename().string().c_str());
    }
    if (!cbStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", cbStatus_.c_str());
    }
}

// Runs Enter / F2 / Delete / Ctrl+D on the browser selection, suppressed while a popup is up.
void SandboxApp::cbShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    const ImGuiIO& io = ImGui::GetIO();
    // CTRL+A COMES FIRST, because it is the one key here that has to work with NOTHING
    // selected -- which is exactly the state the guard below returns on.
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        cbSelection_.clear();
        for (const std::string& p : cbShownPaths_) cbSelection_.push_back(p);
        if (!cbSelection_.empty() && !cbIsSelected(cbSelectedFile_)) cbSelectedFile_ = cbSelection_.front();
        cbStatus_ = std::to_string(cbSelection_.size()) + " item(s) selected";
        return;
    }
    if (io.WantTextInput || io.WantCaptureKeyboard || cbSelectedFile_.empty()) return;
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(cbSelectedFile_, ec);
    const bool editable = cbIsEditable(cbSelectedFile_);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) cbOpenEntry(cbSelectedFile_, isDir);
    if (editable && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantRename_ = true;
        std::snprintf(cbRenameBuf_, sizeof cbRenameBuf_, "%s",
                      std::filesystem::path(cbSelectedFile_).filename().string().c_str());
    }
    if (editable && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantDelete_ = true;
    }
    if (editable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantDuplicate_ = true;
    }
}

// Runs the deferred file operations and their dialogs: duplicate, rename, delete, new folder, import.
void SandboxApp::cbFileOpModals() {
    if (cbWantDuplicate_) { cbWantDuplicate_ = false; cbDuplicateEntry(cbContextPath_); }

    if (cbWantRename_)    { ImGui::OpenPopup("cbRename");    cbWantRename_ = false; }
    if (cbWantDelete_)    { ImGui::OpenPopup("cbDelete");    cbWantDelete_ = false; }
    if (cbWantNewFolder_) { ImGui::OpenPopup("cbNewFolder"); cbWantNewFolder_ = false; }
    if (cbWantImport_)    { ImGui::OpenPopup("Import Asset"); cbWantImport_ = false; }

    if (cbWantMoveOrCopy_) { ImGui::OpenPopup("cbMoveOrCopy"); cbWantMoveOrCopy_ = false; }

    // Copy Here / Move Here / Cancel, same as Unreal and every file manager. Deliberately not a
    // silent move: a drag is easy to do by accident, and copy vs move is the difference between a
    // duplicate and a broken reference.
    if (ImGui::BeginPopupModal("cbMoveOrCopy", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::vector<std::string> srcs = cbPruneNested(cbMoveSources_);
        ImGui::TextDisabled("To  %s", std::filesystem::path(cbMoveDest_).filename().string().c_str());
        ImGui::Separator();

        // The names, capped: a hundred-file drag should not grow a modal taller than the screen.
        const usize kShow = 12;
        for (usize i = 0; i < srcs.size() && i < kShow; ++i)
            ImGui::BulletText("%s", std::filesystem::path(srcs[i]).filename().string().c_str());
        if (srcs.size() > kShow)
            ImGui::TextDisabled("   ...and %d more", static_cast<int>(srcs.size() - kShow));

        // What a move would cost, checked in memory only: a level hashes a placement's asset by its
        // content-relative path, so moving a placed mesh leaves it pointing at nothing (stops drawing,
        // no error). Rewriting every level in the project isn't cheap, so this says so instead of
        // pretending. Copy needs none of this: the original stays put.
        int referenced = 0;
#if AVER_MODULE_SCENE
        {
            scene::World& w = scene::World::instance();
            for (const std::string& sp : srcs) {
                // The id is the hash of the content-relative path (see loadProjectMeshes, which
                // registers every mesh under exactly this). Hashing here rather than a reverse map
                // keeps the two in step: if the keying changes, this breaks loudly, not silently.
                const u64 id = fnv1a64(std::string_view(cbRelativeToContent(sp)));
                if (content_.meshFor(id) == 0) continue;
                const u32 n = w.count();
                for (u32 i = 0; i < n; ++i) {
                    const scene::Entity ent = w.at(i);
                    if (!w.valid(ent)) continue;
                    const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                    if (mr && mr->mesh == id) { ++referenced; break; }
                }
            }
        }
#endif
        if (referenced > 0) {
            ImGui::Separator();
            ImGui::TextWrapped("Moving will break %d placement(s) in the open level - a level names "
                               "an asset by its path. Copy is safe.", referenced);
        }

        ImGui::Separator();
        if (ImGui::Button("Copy Here", ImVec2(120.0f * dpi_, 0))) {
            int ok = 0;
            for (const std::string& sp : srcs) if (cbCopyEntryTo(sp, cbMoveDest_)) ++ok;
            if (ok) cbStatus_ = "Copied " + std::to_string(ok) + " item(s)";
            cbAfterMoveOrCopy(srcs);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Here", ImVec2(120.0f * dpi_, 0))) {
            int ok = 0;
            for (const std::string& sp : srcs) if (cbMoveEntryTo(sp, cbMoveDest_)) ++ok;
            if (ok) cbStatus_ = "Moved " + std::to_string(ok) + " item(s)";
            cbAfterMoveOrCopy(srcs);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * dpi_, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cbMoveSources_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("cbRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Rename %s", cbContextIsDir_ ? "folder" : "file");
        ImGui::SetNextItemWidth(360.0f*dpi_);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submit = ImGui::InputText("##cbrenametxt", cbRenameBuf_, sizeof cbRenameBuf_,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = cbRenameBuf_[0] != '\0' && !std::strpbrk(cbRenameBuf_, "\\/:*?\"<>|");
        if (!valid && cbRenameBuf_[0] != '\0') ImGui::TextColored(ImVec4(0.95f,0.5f,0.45f,1), "That name is not a legal filename.");

        // Renaming breaks every reference -- the worse half of the defect the delete confirm just
        // gained a warning for: an ObjectId is fnv1a64 of the content-relative path, so a rename
        // changes the asset's id while every file naming the old path keeps naming it, and
        // cbRenameEntry used to be just a filesystem rename plus selection bookkeeping -- nothing
        // scanned, nothing rewritten, nothing said. Warns, does not rewrite: fixing referrers means
        // editing other people's files from inside a rename dialog, which wants its own
        // change/undo/test -- the honest half that can land now.
        if (!cbContextIsDir_) {
            if (ImGui::IsWindowAppearing()) cbRenameRefs_ = cbFindReferencesTo(cbContextPath_);
            if (!cbRenameRefs_.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                                   ICON_WARNING " %zu file(s) reference this asset by its current path:",
                                   cbRenameRefs_.size());
                const usize shown = cbRenameRefs_.size() < 8 ? cbRenameRefs_.size() : usize(8);
                for (usize i = 0; i < shown; ++i) ImGui::BulletText("%s", cbRenameRefs_[i].c_str());
                if (cbRenameRefs_.size() > shown)
                    ImGui::TextDisabled("   ...and %zu more", cbRenameRefs_.size() - shown);
                // Opt-in (replacing "renaming will not update them"), default ON: repointing is what
                // an author wants nearly every time, but it edits other people's files, and a tool
                // that does that with no way to decline is one people stop trusting.
                ImGui::Checkbox("Update them to the new name", &cbRenameRepoint_);
                if (cbRenameRepoint_) {
                    // What it still can't fix, said here rather than discovered later: real
                    // reference kinds no text rewrite can reach.
                    ImGui::TextDisabled("Rewrites path references in .ocworld/.ocmap/.ocmat/.ocgraph/.ocproject.");
                    ImGui::TextDisabled("Cannot fix: ids stored as a hash with no path (.ocmat {guid:...},");
                    ImGui::TextDisabled("save games), C# that builds a path in code, or binary asset tables.");
                } else {
                    ImGui::TextDisabled("They will stop resolving.");
                }
                ImGui::Separator();
            }
        }

        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Rename") || (submit && valid)) {
            if (!cbRenameRefs_.empty() && !cbRenameRepoint_)
                AVER_WARN("[Editor] renamed '{}' while {} file(s) still reference its old path",
                          cbContextPath_, cbRenameRefs_.size());
            cbRenameEntry(cbContextPath_, cbRenameBuf_, cbRenameRepoint_ && !cbRenameRefs_.empty());
            cbRenameRefs_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { cbRenameRefs_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("cbDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Multi-delete: when the active item is part of a larger selection, Delete acts on the
        // whole selection like Move/Copy, pruned with cbPruneNested (else a folder in the selection
        // would try to delete its own children a second time). A lone selection reduces to just cbContextPath_.
        const std::vector<std::string> targets =
            (cbSelection_.size() > 1 && cbIsSelected(cbContextPath_))
                ? cbPruneNested(cbSelection_) : std::vector<std::string>{cbContextPath_};

        if (targets.size() > 1) {
            ImGui::Text("Delete %zu items?", targets.size());
            const usize kShow = 12;
            for (usize i = 0; i < targets.size() && i < kShow; ++i)
                ImGui::BulletText("%s", std::filesystem::path(targets[i]).filename().string().c_str());
            if (targets.size() > kShow)
                ImGui::TextDisabled("   ...and %zu more", targets.size() - kShow);
        } else {
            ImGui::TextUnformatted(cbContextIsDir_
                ? "Delete this folder and everything in it?"
                : "Delete this file?");
            ImGui::TextDisabled("%s", cbContextPath_.c_str());
        }
        ImGui::TextDisabled("It goes to the recycle bin, so it can be restored.");

        // What will break, named before deletion rather than after: scanned once on
        // IsWindowAppearing, not per frame (walks the content tree), and aggregated across every
        // target in the selection, not just cbContextPath_.
        if (ImGui::IsWindowAppearing()) {
            cbDeleteRefs_.clear();
            for (const std::string& t : targets) {
                std::error_code dec;
                if (std::filesystem::is_directory(t, dec)) continue;
                const std::vector<std::string> refs = cbFindReferencesTo(t);
                cbDeleteRefs_.insert(cbDeleteRefs_.end(), refs.begin(), refs.end());
            }
            std::sort(cbDeleteRefs_.begin(), cbDeleteRefs_.end());
            cbDeleteRefs_.erase(std::unique(cbDeleteRefs_.begin(), cbDeleteRefs_.end()), cbDeleteRefs_.end());
        }
        if (!cbDeleteRefs_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                               ICON_WARNING " %zu file(s) reference what would be deleted:", cbDeleteRefs_.size());
            // Capped, because a shared material can be named by hundreds of levels and a
            // modal that grows past the screen cannot be dismissed.
            const usize shown = cbDeleteRefs_.size() < 12 ? cbDeleteRefs_.size() : usize(12);
            for (usize i = 0; i < shown; ++i) ImGui::BulletText("%s", cbDeleteRefs_[i].c_str());
            if (cbDeleteRefs_.size() > shown)
                ImGui::TextDisabled("   ...and %zu more", cbDeleteRefs_.size() - shown);
            ImGui::TextDisabled("References are stored as paths, so deleting this breaks them.");
            ImGui::Separator();
        }
        if (ImGui::Button("Delete")) {
            for (const std::string& t : targets) cbDeleteEntry(t);
            cbDeleteRefs_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { cbDeleteRefs_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("cbNewFolder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("New folder in %s", cbSelectedDir_.c_str());
        ImGui::SetNextItemWidth(360.0f*dpi_);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submit = ImGui::InputText("##cbnewfoldertxt", cbNewFolderBuf_, sizeof cbNewFolderBuf_,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = cbNewFolderBuf_[0] != '\0' && !std::strpbrk(cbNewFolderBuf_, "\\/:*?\"<>|");
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create") || (submit && valid)) {
            cbCreateFolder(cbSelectedDir_, cbNewFolderBuf_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Draws the selected folder as clickable ancestor segments.
void SandboxApp::drawBreadcrumb(const std::string& dir) {
    const std::vector<CbRoot> roots = cbRoots();
    const CbRoot* owner = nullptr;
    for (const CbRoot& r : roots)
        if (dir.size() >= r.path.size() && dir.compare(0, r.path.size(), r.path) == 0) { owner = &r; break; }
    if (!owner) { ImGui::TextDisabled("%s", dir.c_str()); return; }

    std::string acc = owner->path;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f*dpi_, 1.0f*dpi_));
    if (ImGui::SmallButton(owner->label)) cbNavigate(acc);
    std::string tail = dir.substr(owner->path.size());
    usize i = 0;
    while (i < tail.size()) {
        while (i < tail.size() && (tail[i] == '\\' || tail[i] == '/')) ++i;
        const usize start = i;
        while (i < tail.size() && tail[i] != '\\' && tail[i] != '/') ++i;
        if (i == start) break;
        const std::string seg = tail.substr(start, i - start);
        acc += "\\" + seg;
        ImGui::SameLine(0.0f, 2.0f*dpi_); ImGui::TextDisabled(">"); ImGui::SameLine(0.0f, 2.0f*dpi_);
        ImGui::PushID(static_cast<int>(start));
        if (ImGui::SmallButton(seg.c_str())) cbNavigate(acc);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
}

// Which family a .ocgraph presents as -- Material, Gameplay or Unknown (GraphAssetPresentation.hpp)
// -- read from its DOMAIN record, cached against the file's mtime like fileIconTile()'s .cs
// classification above. A read failure (the file vanished since the scan, or isn't parseable at
// all) reads as Gameplay, not Unknown: the same safe default an absent DOMAIN record gets, since a
// transient I/O failure claiming "not mine" would be worse than "ordinary graph" for one cache cycle.
editor::GraphAssetFamily SandboxApp::graphAssetFamilyFor(const std::string& path) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (auto it = graphDomainCache_.find(path);
        it != graphDomainCache_.end() && !ec && it->second.first == mtime) return it->second.second;

    fmt::OcGraphData g;
    editor::GraphAssetFamily family = editor::GraphAssetFamily::Gameplay;
    if (fmt::loadOcgraph(path, g)) family = editor::graphAssetPresentationFor(fmt::ocGraphDomainOf(g)).family;
    if (!ec) graphDomainCache_[path] = {mtime, family};
    return family;
}

// Returns a directory's sorted listing, cached for 20 frames.
const DirListing& SandboxApp::dirListing(const std::string& dir) {
    DirListing& c = dirCache_[dir];
    if (frameNo_ - c.stamp < 20) return c;
    c.stamp = frameNo_;
    c.entries.clear(); c.dirCount = 0;
    std::error_code ec;
    try {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            DirEntry ent;
            ent.path  = it->path();
            ent.isDir = it->is_directory(fec);
            ent.full  = ent.path.string();
            ent.name  = ent.path.filename().string();
            if (ent.isDir) {
                std::error_code mec;
                ent.module = isEnginePath(ent.full) ||
                             std::filesystem::exists(ent.path / "CMakeLists.txt", mec);
                ++c.dirCount;
            } else {
                const std::string lext = lowerExt(ent.path);
                ent.tile = fileIconTile(ent.full, ent.name, lext);
                // Resolved even when a sprite tile exists, because the DETAILS STRIP wants the
                // type's name whether or not the grid drew art for it.
                ent.kind = -1;
                if (assetKindFor(lext)) {
                    // Stored as the table index so DirEntry stays a plain value type: a raw pointer
                    // into a function-local static works today but breaks if the table ever moves.
                    ent.kind = 1;
                    ent.kindExt = lext;
                }
                // A material graph is not a generic graph -- see GraphAssetPresentation.hpp for
                // why Gameplay/Material/Unknown must not collapse into one bucket. Read HERE,
                // once per 20-frame listing refresh, not per frame: see graphAssetFamilyFor().
                if (lext == ".ocgraph") ent.graphFamily = graphAssetFamilyFor(ent.full);
            }
            c.entries.push_back(std::move(ent));
        }
    } catch (const std::exception&) { }
    // Folders first, then files, each alphabetical: the views split the groups by dirCount alone.
    std::sort(c.entries.begin(), c.entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return a.full < b.full;
    });
    return c;
}

// Draws a folder's subfolders as tree nodes, recursing into the ones that are open.
void SandboxApp::drawFolderTree(const std::string& dir) {
    std::vector<std::pair<std::string, std::string>> subs;   // (full, name)
    {
        const DirListing& l = dirListing(dir);
        subs.reserve(l.dirCount);
        for (const DirEntry& e : l.entries) { if (!e.isDir) break; subs.emplace_back(e.full, e.name); }
    }
    for (const auto& [full, name] : subs) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (cbSelectedDir_ == full) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cbNavigate(full);
        // The tree is the other half of the gesture: dragging to a folder you can SEE but are not
        // currently inside is the whole point, and the grid only shows the current folder.
        cbFolderDropTarget(full);
        if (open) { drawFolderTree(full); ImGui::TreePop(); }
    }
}

// `graphFamily` matters only for ext == ".ocgraph" and defaults to Gameplay (the ordinary "Graph"
// row below), so callers with no domain to offer (the existence check in dirListing(), any other
// extension) get today's behaviour unchanged. Material/Unknown are handled before the table lookup
// since the table is keyed by extension alone -- one row per key, so it can't hold three different
// ".ocgraph" presentations.
 const SandboxApp::AssetKind* SandboxApp::assetKindFor(const std::string& ext,
                                      editor::GraphAssetFamily graphFamily) {
    if (ext == ".ocgraph") {
        // Material's own icon+tint (ICON_TUNE, Unreal's Material green): a material graph IS a
        // material to anyone browsing Content, not a link-icon graph that happens to be green.
        if (graphFamily == editor::GraphAssetFamily::Material) {
            static const AssetKind kMaterialGraph{ICON_TUNE, IM_COL32(64, 192, 64, 255),
                                                   "Material Graph"};
            return &kMaterialGraph;
        }
        // NEITHER Gameplay's blue "Graph" NOR Material's green -- a DOMAIN this build does not
        // recognise must not be presented as either. See GraphAssetPresentation.hpp.
        if (graphFamily == editor::GraphAssetFamily::Unknown) {
            static const AssetKind kUnknownGraph{ICON_WARNING, IM_COL32(200, 160, 40, 255),
                                                  "Unknown Graph"};
            return &kUnknownGraph;
        }
    }
    // Coloured the way Unreal colours its Content Browser, since that coding is already known
    // (FAssetTypeActions_*::GetTypeColor):
    //     Static Mesh cyan, Skeletal Mesh/Skeleton pink, Animation lime, Material green,
    //     Texture red, Sound blue, World amber, Blueprint blue.
    // Where this engine has no Unreal equivalent (F#, C#, HLSL, navmesh, behaviour trees), colour
    // sits in the nearest Unreal family. A skinned .ocmesh takes pink, not cyan (cardAccent()).
    static const struct { const char* ext; AssetKind k; } kTable[] = {
        {".ocmesh",     {ICON_TERRAIN,    IM_COL32(  0, 255, 255, 255), "Static Mesh"}},
        {".ocworld",    {ICON_TERRAIN,    IM_COL32(255, 156,   0, 255), "Level"}},
        {".ocmap",      {ICON_TERRAIN,    IM_COL32(255, 156,   0, 255), "Level"}},
        {".ocskel",     {ICON_TREE,       IM_COL32(241, 163, 241, 255), "Skeleton"}},
        {".ocanim",     {ICON_PLAY,       IM_COL32(181, 230,  29, 255), "Animation"}},
        {".ocmat",      {ICON_TUNE,       IM_COL32( 64, 192,  64, 255), "Material"}},
        {".ocparticle", {ICON_ADD,        IM_COL32(  0, 200, 180, 255), "Particles"}},
        {".ocfoliage",  {ICON_TERRAIN,    IM_COL32( 60, 200,  90, 255), "Foliage Type"}},
        // No Unreal equivalent (its Enhanced Input plugin's icon isn't licensed to copy):
        // ICON_SETTINGS since binding a key is a settings choice, same as .fsproj/.csproj below,
        // though coloured apart from them.
        {".ocinput",    {ICON_SETTINGS,   IM_COL32(230, 200,  60, 255), "Input Scheme"}},
        {".ocsnd",      {ICON_WAVE,       IM_COL32(  0, 175, 255, 255), "Sound Graph"}},
        {".ocaudio",    {ICON_AUDIO,      IM_COL32(  0, 175, 255, 255), "Audio"}},
        {".wav",        {ICON_AUDIO,      IM_COL32(  0, 175, 255, 255), "Audio"}},
        {".ogg",        {ICON_AUDIO,      IM_COL32(  0, 175, 255, 255), "Audio"}},
        {".ocbt",       {ICON_TREE,       IM_COL32( 63, 126, 255, 255), "Behaviour Tree"}},
        {".ocgraph",    {ICON_LINK,       IM_COL32( 63, 126, 255, 255), "Graph"}},
        {".ocnav",      {ICON_TERRAIN,    IM_COL32(150, 200, 120, 255), "Navigation"}},
        // A soft-body vehicle cage (nodes/beams/panels), NOT a skeletal mesh -- see OcBeam.hpp.
        // Unreal has no equivalent, so this borrows the destruction family's amber-red.
        {".ocbeam",     {ICON_TREE,       IM_COL32(214, 122,  64, 255), "Soft-Body Cage"}},
        {".hlsl",       {ICON_BUILD,      IM_COL32(140, 200, 220, 255), "Shader"}},
        {".png",        {ICON_VISIBILITY, IM_COL32(192,  64,  64, 255), "Texture"}},
        {".jpg",        {ICON_VISIBILITY, IM_COL32(192,  64,  64, 255), "Texture"}},
        {".tga",        {ICON_VISIBILITY, IM_COL32(192,  64,  64, 255), "Texture"}},
        {".gltf",       {ICON_TERRAIN,    IM_COL32(190, 160, 120, 255), "glTF"}},
        {".glb",        {ICON_TERRAIN,    IM_COL32(190, 160, 120, 255), "glTF"}},
        {".json",       {ICON_FILE,       IM_COL32(160, 164, 172, 255), "Data"}},
        {".md",         {ICON_FILE,       IM_COL32(160, 164, 172, 255), "Notes"}},
        {".fs",         {ICON_EDIT,       IM_COL32(120, 150, 210, 255), "F#"}},
        {".fsproj",     {ICON_SETTINGS,   IM_COL32(120, 150, 210, 255), "F# Project"}},
        {".csproj",     {ICON_SETTINGS,   IM_COL32(120, 150, 210, 255), "C# Project"}},
        // These four have real sprite art and never reach typedGlyph, but the card's TYPE BAR
        // still needs their colour -- which is the whole reason the table now covers them.
        {".cs",         {ICON_EDIT,       IM_COL32(149, 117, 205, 255), "C# Script"}},
        {".cpp",        {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Source"}},
        {".cxx",        {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Source"}},
        {".cc",         {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Source"}},
        {".hpp",        {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Header"}},
        {".hxx",        {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Header"}},
        {".h",          {ICON_BUILD,      IM_COL32(100, 149, 237, 255), "C++ Header"}},
    };
    for (const auto& row : kTable) if (ext == row.ext) return &row.k;
    return nullptr;
}

// True when this entry is a .ocmesh carrying skin weights -- a SKELETAL mesh, coloured
// differently from a static one. Keyed by the SAME fnv1a64(relative path) id loadProjectMeshes
// assigned, so anything unloaded reads as static rather than guessing.
bool SandboxApp::isSkinnedMeshEntry(const DirEntry& e) const {
#if AVER_MODULE_SCENE
    if (e.isDir || e.kindExt != ".ocmesh" || skinnedMeshIds_.empty()) return false;
    const std::string content = project_.contentDir();
    if (content.empty()) return false;
    std::error_code ec;
    std::string rel = std::filesystem::relative(e.path, content, ec).string();
    if (ec || rel.empty()) return false;
    for (char& c : rel) if (c == '\\') c = '/';
    return skinnedMeshIds_.count(fnv1a64(std::string_view(rel))) != 0;
#else
    // skinnedMeshIds_ needs AVER_MODULE_SCENE (filled by loadProjectMeshes as it places meshes),
    // so with the module off nothing was ever recorded; every entry reads as a plain static mesh.
    (void)e;
    return false;
#endif
}

// The colour of one card's type bar. Null-safe over every entry a listing can contain.
// `skinned` is the one thing the extension alone can't answer: a skeletal mesh is a .ocmesh with
// skin weights, not a separate file type -- loadProjectMeshes already read kOcMeshHasSkin out of
// the header for this (skinnedMeshIds_).
 ImU32 SandboxApp::cardAccent(const std::string& ext, bool isDir, bool skinned,
                         editor::GraphAssetFamily graphFamily) {
    if (isDir) return IM_COL32(150, 156, 166, 255);          // neutral: a folder has no type
    if (skinned) return IM_COL32(241, 163, 241, 255);        // Skeletal Mesh, per the table above
    if (const AssetKind* k = assetKindFor(ext, graphFamily)) return k->tint;
    return IM_COL32(130, 136, 146, 255);                     // an unrecognised file
}

 int SandboxApp::assetIconTile(const std::string& ext) {
    if (ext == ".ocanim") return 0;
    if (ext == ".ocskel") return 1;
    if (ext == ".ocmesh") return 2;
    if (ext == ".ocgraph") return 3;
    return -1;
}

// Returns a file's sprite tile. 0..3 index the file sheet (C# Script / C# Class / C++ Class /
// C++ Module); kAssetTileBase + n indexes the asset sheet; -1 is neither. A .cs is classified by
// peeking at its head and cached against the file's modification time.
// THE ASSET CHECK COMES FIRST, and has to: below it sits `if (ext != ".cs") return -1;`.
int SandboxApp::fileIconTile(const std::string& path, const std::string& name, const std::string& ext) {
    if (const int a = assetIconTile(ext); a >= 0) return kAssetTileBase + a;
    if (name == "CMakeLists.txt") return 3;
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".hpp" || ext == ".hxx" || ext == ".h")
        return 2;
    if (ext != ".cs") return -1;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (auto it = fileIconCache_.find(path);
        it != fileIconCache_.end() && !ec && it->second.first == mtime) return it->second.second;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 1;
    int tile = 1;
    char head[8192];
    in.read(head, sizeof(head));
    const std::string body(head, static_cast<size_t>(in.gcount()));
    static const char* kScriptMarkers[] = {
        "AverBehaviour", "AverActor", "AverPawn", "AverCharacter", "AverPlayerController",
        "AverGameMode", "AverGameInstance", "[AverClass", "[AverGameMode"};
    for (const char* m : kScriptMarkers) if (body.find(m) != std::string::npos) { tile = 0; break; }
    if (!ec) fileIconCache_[path] = {mtime, tile};
    return tile;
}

// Draws a folder icon: a body with a raised tab.
 void SandboxApp::folderGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 w = s, h = s * 0.76f;
    const f32 x0 = c.x - w*0.5f, y0 = c.y - h*0.5f, tabH = h * 0.17f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w*0.44f, y0 + tabH*2.4f), col, s*0.06f);
    dl->AddRectFilled(ImVec2(x0, y0 + tabH), ImVec2(x0 + w, y0 + h), col, s*0.07f);
}

// Draws a generic document icon: a page with its corner turned.
 void SandboxApp::fileGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 w = s * 0.74f, h = s;
    const f32 x0 = c.x - w*0.5f, y0 = c.y - h*0.5f, fold = w * 0.34f;
    dl->PathLineTo(ImVec2(x0, y0));
    dl->PathLineTo(ImVec2(x0 + w - fold, y0));
    dl->PathLineTo(ImVec2(x0 + w, y0 + fold));
    dl->PathLineTo(ImVec2(x0 + w, y0 + h));
    dl->PathLineTo(ImVec2(x0, y0 + h));
    dl->PathFillConvex(col);
    dl->AddTriangleFilled(ImVec2(x0 + w - fold, y0), ImVec2(x0 + w, y0 + fold),
                          ImVec2(x0 + w - fold, y0 + fold), IM_COL32(0, 0, 0, 80));
}

// Blits one tile of an N-tile sheet, fitted inside an s-by-s box at its own aspect.
 void SandboxApp::blitTile(ImDrawList* dl, u64 tex, ImVec2 centre, f32 s, f32 aspect, int tile, int tiles) {
    const f32 w = aspect >= 1.0f ? s : s * aspect;
    const f32 h = aspect >= 1.0f ? s / aspect : s;
    dl->AddImage(static_cast<ImTextureID>(tex),
                 ImVec2(centre.x - w*0.5f, centre.y - h*0.5f),
                 ImVec2(centre.x + w*0.5f, centre.y + h*0.5f),
                 ImVec2(static_cast<f32>(tile) / tiles, 0.0f),
                 ImVec2(static_cast<f32>(tile + 1) / tiles, 1.0f));
}

// Draws one entry's icon, from the sprite sheet where there is one and the drawn glyph otherwise:
// a soft rounded plate in the type's colour with its Material Icon centred on it. The plate gives
// the grid a consistent silhouette -- glyphs alone have wildly different visual weight.
void SandboxApp::typedGlyph(ImDrawList* dl, ImVec2 c, f32 s, const AssetKind& k) {
    const f32 half = s * 0.42f;
    const ImU32 plate = (k.tint & 0x00FFFFFFu) | (ImU32(38) << IM_COL32_A_SHIFT);
    const ImU32 edge  = (k.tint & 0x00FFFFFFu) | (ImU32(90) << IM_COL32_A_SHIFT);
    dl->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half),
                      plate, s * 0.16f);
    dl->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half),
                edge, s * 0.16f, 0, 1.0f);
    // Sized off the plate rather than the font, so the glyph fills the tile at every zoom level
    // the size slider offers.
    ImFont* font = ImGui::GetFont();
    const f32 px = s * 0.46f;
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, k.icon);
    dl->AddText(font, px, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), k.tint, k.icon);
}

void SandboxApp::drawEntryIcon(ImDrawList* dl, ImVec2 centre, f32 s, bool isDir, int tile, bool module,
                   const std::string& kindExt,
                   editor::GraphAssetFamily graphFamily) {
    if (isDir) {
        if (folderIconsUiId_) blitTile(dl, folderIconsUiId_, centre, s, folderIconAspect_, module ? 1 : 0, 2);
        else                  folderGlyph(dl, centre, s, IM_COL32(232, 187, 92, 255));
        return;
    }
    // The asset sheet's one "graph" tile is Gameplay's picture (no separate Material/Unknown Graph
    // art), so a non-Gameplay .ocgraph skips it rather than pairing a Material-green bar with a
    // plain blue graph icon -- falls to the domain-aware typed glyph below instead.
    const bool skipGenericGraphSprite =
        kindExt == ".ocgraph" && graphFamily != editor::GraphAssetFamily::Gameplay;
    if (!skipGenericGraphSprite && tile >= kAssetTileBase && assetIconsUiId_) {
        blitTile(dl, assetIconsUiId_, centre, s, assetIconAspect_, tile - kAssetTileBase, kAssetIconTiles);
        return;
    }
    if (!skipGenericGraphSprite && tile >= 0 && tile < kAssetTileBase && fileIconsUiId_) {
        blitTile(dl, fileIconsUiId_, centre, s, fileIconAspect_, tile, kFileIconTiles);
        return;
    }
    // The typed glyph, before the anonymous page. This is the line that makes twenty different
    // asset types stop looking like twenty identical documents.
    if (!kindExt.empty())
        if (const AssetKind* k = assetKindFor(kindExt, graphFamily)) { typedGlyph(dl, centre, s, *k); return; }
    fileGlyph(dl, centre, s, IM_COL32(150, 154, 162, 255));
}

// Returns a path's extension, lower-cased.
 std::string SandboxApp::lowerExt(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

// Every extension the Content Browser will let a user drag into the level -- the single gate both
// grid and list view check before BeginDragDropSource, so a new placeable type changes here once.
// spawnFromAssetDrop must accept every extension this says yes to, and nothing else.
 bool SandboxApp::isPlaceableAssetExt(const std::string& ext) {
    if (ext == ".ocmesh") return true;
#if AVER_MODULE_PARTICLES
    if (ext == ".ocparticle") return true;
#endif
    return false;
}

// ---- Content Browser multi-selection ---------------------------------------------------------
// cbSelectedFile_ remains THE active one: what the footer names, what Enter opens, what F2 renames,
// what a shift-range measures from. cbSelection_ is the set and always contains cbSelectedFile_
// when anything is selected, so single-item actions (rename, duplicate) need no special case.
bool SandboxApp::cbIsSelected(const std::string& path) const {
    return std::find(cbSelection_.begin(), cbSelection_.end(), path) != cbSelection_.end();
}

// Applies one click to the selection, with the modifier rules every file browser has trained
// people to expect: plain replaces, Ctrl toggles one, Shift takes the range from the active item.
// The range is over `shown`, not the folder, deliberately: `shown` is what the search box left on
// screen, and shift-selecting across a filter would grab files the person cannot see.
void SandboxApp::cbClickSelect(const std::vector<const DirEntry*>& shown, int index) {
    if (index < 0 || index >= static_cast<int>(shown.size())) return;
    const std::string& path = shown[static_cast<usize>(index)]->full;
    const ImGuiIO& io = ImGui::GetIO();

    if (io.KeyShift && !cbSelectedFile_.empty()) {
        int anchor = -1;
        for (int i = 0; i < static_cast<int>(shown.size()); ++i)
            if (shown[static_cast<usize>(i)]->full == cbSelectedFile_) { anchor = i; break; }
        if (anchor >= 0) {
            const int lo = anchor < index ? anchor : index;
            const int hi = anchor < index ? index : anchor;
            cbSelection_.clear();
            for (int i = lo; i <= hi; ++i) cbSelection_.push_back(shown[static_cast<usize>(i)]->full);
            // The ACTIVE item does not move on a shift-click, so shift-clicking again from the
            // same anchor grows and shrinks the range rather than walking it.
            return;
        }
    }
    if (io.KeyCtrl) {
        const auto it = std::find(cbSelection_.begin(), cbSelection_.end(), path);
        if (it != cbSelection_.end()) {
            cbSelection_.erase(it);
            // Ctrl-clicking the active item away has to hand "active" to something still selected,
            // or the footer names a file that is no longer part of the selection.
            if (cbSelectedFile_ == path)
                cbSelectedFile_ = cbSelection_.empty() ? std::string() : cbSelection_.back();
            return;
        }
        cbSelection_.push_back(path);
        cbSelectedFile_ = path;
        return;
    }
    cbSelection_.assign(1, path);
    cbSelectedFile_ = path;
}

// Ctrl+A: everything the current folder is showing, which is what the filter left visible.
void SandboxApp::cbSelectAll(const std::vector<const DirEntry*>& shown) {
    cbSelection_.clear();
    for (const DirEntry* e : shown) cbSelection_.push_back(e->full);
    if (!cbSelection_.empty() && !cbIsSelected(cbSelectedFile_)) cbSelectedFile_ = cbSelection_.front();
}

void SandboxApp::cbClearSelection() { cbSelection_.clear(); cbSelectedFile_.clear(); }

// What the drag preview says: the one name, or how many are coming.
// Counts cbMoveDragPayloadFor's payload: newline-separated paths (a path can't contain one), the
// whole selection, unfiltered. No separate "placeable assets only" payload -- ImGui carries one
// payload per drag, so each drop target filters what it can use (viewport keeps .ocmesh/.ocparticle).
 std::string SandboxApp::cbDragLabel(const std::string& name, const std::string& blob) {
    const usize n = static_cast<usize>(std::count(blob.begin(), blob.end(), '\n')) + 1;
    return n <= 1 ? name : std::to_string(n) + " assets";
}

 bool SandboxApp::containsNoCase(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const usize n = std::strlen(needle);
    if (hay.size() < n) return false;
    for (usize i = 0; i + n <= hay.size(); ++i) {
        usize j = 0;
        while (j < n && std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                        std::tolower(static_cast<unsigned char>(needle[j]))) ++j;
        if (j == n) return true;
    }
    return false;
}

// Trims a name to at most `lines` wrapped lines, ending in an ellipsis when it does not fit.
 std::string SandboxApp::fitLabel(const std::string& name, f32 wrap, int lines) {
    const f32 maxH = ImGui::GetTextLineHeight() * lines + 1.0f;
    if (ImGui::CalcTextSize(name.c_str(), nullptr, false, wrap).y <= maxH) return name;
    std::string s = name;
    while (s.size() > 1) {
        s.pop_back();
        const std::string t = s + "...";
        if (ImGui::CalcTextSize(t.c_str(), nullptr, false, wrap).y <= maxH) return t;
    }
    return name;
}

// The revision-control corner mark, shared by the gallery and the list (one status can't be a dot
// in one view and something else in the other). A dark backing ring, not a bare dot: it lands on a
// pale thumbnail, dark empty card, or coloured type plate, where a flat dot would disappear into
// half of those. Colour isn't the only carrier: a conflict also gets a second ring, and every mark
// has a tooltip naming the status in words -- six statuses by hue alone would be unreadable for many users.
static void rcStatusDot(ImDrawList* dl, ImVec2 c, f32 r, ImU32 col, bool conflicted) {
    dl->AddCircleFilled(c, r + 1.0f, IM_COL32(14, 15, 18, 200));
    dl->AddCircleFilled(c, r, col);
    if (conflicted) dl->AddCircle(c, r + 2.5f, col, 0, ImMax(1.0f, r * 0.35f));
}

// Draws the gallery view: a wrapped, row-clipped grid of icon tiles.
void SandboxApp::drawFolderGallery(const std::vector<const DirEntry*>& shown) {
    const f32 tile   = cbTileSize_ * dpi_;
    const f32 pad    = 8.0f * dpi_;
    const f32 labelH = ImGui::GetTextLineHeight() * 2.0f + 4.0f * dpi_;   // two lines: names wrap
    const f32 cellW  = tile, cellH = tile + labelH;
    int perRow = static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (cellW + pad));
    if (perRow < 1) perRow = 1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int rows = (static_cast<int>(shown.size()) + perRow - 1) / perRow;
    ImGuiListClipper clipper;
    clipper.Begin(rows, cellH + pad);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int col = 0; col < perRow; ++col) {
                const int idx = r * perRow + col;
                if (idx >= static_cast<int>(shown.size())) break;
                const DirEntry& e = *shown[idx];
                if (col) ImGui::SameLine(0.0f, pad);
                ImGui::PushID(idx);
                const ImVec2 o = ImGui::GetCursorScreenPos();
                // Transparent, drawn over: Selectable paints its highlight immediately, so a card
                // fill emitted after would bury it. Kept for hit-testing/nav/drag/context menu only;
                // selection/hover are painted with the card instead.
                ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(0, 0, 0, 0));
                const bool selected = cbIsSelected(e.full);
                if (ImGui::Selectable("##cell", selected,
                                      ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellW, cellH))) {
                    cbClickSelect(shown, idx);
                    // A double-click OPENS, and must not also leave a range behind: opening a
                    // folder replaces the listing the selection indexes into.
                    if (ImGui::IsMouseDoubleClicked(0) || (e.isDir && !cbDoubleClickEnter_)) {
                        cbSelection_.assign(1, e.full);
                        cbSelectedFile_ = e.full;
                        cbOpenEntry(e.full, e.isDir);
                    }
                }
                ImGui::PopStyleColor(3);
                const bool hot = ImGui::IsItemHovered();
                // What git says about this entry, read from the latch SandboxShell.cpp's
                // revisionControlTick() maintains -- a sorted-vector lookup, never a process (runs
                // once per visible card per frame, and must never itself ask git anything). Empty =
                // git has nothing to say (matches HEAD and the index).
                editor::FileStatus rcSt = editor::FileStatus::Unmodified;
                const bool rcHas = rcMarkFor(e.full, e.isDir, rcSt);
                // Every entry is draggable, folders included: gating on "placeable in the viewport"
                // was right for the viewport but wrong for the browser (a folder couldn't be
                // dragged, silently dropped from a selection). One payload, the whole selection:
                // ImGui has no "two payloads on one drag" -- a second SetDragDropPayload overwrites
                // the first's type, so the old asset payload was replaced by the move payload and
                // every viewport drop did nothing. Each target now takes this one payload and keeps
                // what it understands: the viewport places .ocmesh/.ocparticle from it, a folder
                // moves all of it.
                if (ImGui::BeginDragDropSource()) {
                    const std::string moveBlob = cbMoveDragPayloadFor(e.full);
                    ImGui::SetDragDropPayload(kCbMoveDragDropType, moveBlob.c_str(), moveBlob.size() + 1);
                    // The label is emitted unconditionally now: it is the drag's only visual, and
                    // a folder drag with no preview looks like nothing is happening.
                    ImGui::TextUnformatted(cbDragLabel(e.name, moveBlob).c_str());
                    ImGui::EndDragDropSource();
                }
                // A FOLDER TILE ACCEPTS A DROP. Registered right after the Selectable so it binds
                // to that widget's rect -- the full card -- rather than to whatever is drawn next.
                if (e.isDir) cbFolderDropTarget(e.full);
                if (ImGui::IsItemHovered()) {
                    // The status in words, beside the name: the dot is a glance, this is the answer
                    // (a folder's mark summarises what's under it, not a claim about the folder itself).
                    if (rcHas)
                        ImGui::SetTooltip("%s\ngit: %s%s", e.name.c_str(),
                                          e.isDir ? "something under here is " : "",
                                          editor::statusName(rcSt));
                    else
                        ImGui::SetTooltip("%s", e.name.c_str());
                }
                cbItemContextMenu(e.full, e.name, e.isDir);
                // ---- the card ----
                // Unreal's tile in three pieces: a panel so the grid reads as objects, a preview
                // square, and a bar along the bottom edge in the type's colour; the name sits on a
                // slightly darker strip so long names don't look like they belong to the tile beneath.
                const f32 round  = 3.0f * dpi_;
                const f32 barH   = ImMax(2.0f, 3.0f * dpi_);
                const f32 prevH  = tile - barH;                 // the preview square, above the bar
                const ImVec2 cardMin = o, cardMax(o.x + cellW, o.y + cellH);
                const ImU32 accent = cardAccent(e.kindExt.empty() ? lowerExt(e.path) : e.kindExt,
                                                e.isDir, isSkinnedMeshEntry(e), e.graphFamily);

                dl->AddRectFilled(cardMin, cardMax,
                                  ImGui::GetColorU32(selected ? ImGuiCol_Header
                                                              : (hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg)),
                                  round);
                // The label strip, a touch darker than the preview so the two read as separate.
                dl->AddRectFilled(ImVec2(o.x, o.y + tile), cardMax, IM_COL32(0, 0, 0, 46), round,
                                  ImDrawFlags_RoundCornersBottom);
                // THE TYPE BAR. Folders get none -- a folder has no asset type, and Unreal draws
                // none either.
                if (!e.isDir)
                    dl->AddRectFilled(ImVec2(o.x, o.y + prevH), ImVec2(o.x + cellW, o.y + tile), accent);
                dl->AddRect(cardMin, cardMax,
                            selected ? accent : ImGui::GetColorU32(ImGuiCol_Border),
                            round, 0, selected ? 2.0f : 1.0f);

                const ImVec2 iconCentre(o.x + cellW*0.5f, o.y + prevH*0.5f);
                bool drewThumb = false;
#if AVER_MODULE_SCENE
                // A real rendered thumbnail, mesh assets only, inside the clipper's visible range
                // (a folder of thousands never queues more than a screenful). content_'s meshes are
                // looked up, never loaded -- loadProjectMeshes() already uploads every .ocmesh on
                // project open, so a miss means "not loaded" (reload), not a stalling synchronous read.
                if (!e.isDir && e.kindExt == ".ocmesh" && thumbnails_.ready()) {
                    const std::string content = project_.contentDir();
                    std::error_code relEc;
                    std::string rel = content.empty() ? std::string()
                                      : std::filesystem::relative(e.path, content, relEc).string();
                    if (!content.empty() && !relEc && !rel.empty()) {
                        for (char& c : rel) if (c == '\\') c = '/';
                        // The same id space content_'s meshes/index key on (see loadProjectMeshes()):
                        // a mesh this browser can already place is exactly the set this can thumbnail.
                        const u64 meshId = fnv1a64(std::string_view(rel));
                        if (const rhi::MeshHandle meshHandle = content_.meshFor(meshId)) {
                            u32 thumbMaterial = 0;
#if AVER_MODULE_PBR
                            // The mesh's own slot-0 material, looked up only until the tile exists.
                            if (!thumbnails_.textureId(meshId)) {
                                const std::string& slot0 = content_.meshSlot0Name(meshId);
                                if (!slot0.empty()) thumbMaterial = content_.materialForSurface(slot0);
                            }
#endif
                            thumbnails_.request(meshId, meshHandle, thumbMaterial);
                            if (const u64 tex = thumbnails_.textureId(meshId)) {
                                // Whole-texture, square (kThumbnailPx is fixed on both axes): blitTile
                                // with one tile of one, nearly filling the preview -- the content, not a badge.
                                blitTile(dl, tex, iconCentre, prevH*0.92f, 1.0f, 0, 1);
                                drewThumb = true;
                            }
                        }
                    }
                }
#endif
                // A real decoded thumbnail, texture files -- the CPU-side twin of the mesh branch
                // above: there's nothing to render, so ThumbnailCache decodes/downscales the image
                // itself instead of driving the preview (shares entries_/budget/eviction with the
                // mesh path; see its own header). Needs no AVER_MODULE_SCENE guard: it never touches
                // content_'s meshes.
                if (!drewThumb && !e.isDir && thumbnails_.ready() && isTextureSource(e.full)) {
                    thumbnails_.requestTexture(e.full);
                    if (const u64 tex = thumbnails_.textureIdForPath(e.full)) {
                        // Same whole-texture blit as the mesh branch: the cache always letterboxes
                        // into a kThumbnailPx square, so the aspect here is always 1.0 too.
                        blitTile(dl, tex, iconCentre, prevH*0.92f, 1.0f, 0, 1);
                        drewThumb = true;
                    }
                }
                if (!drewThumb)
                    drawEntryIcon(dl, iconCentre, prevH*0.58f, e.isDir, e.tile, e.module, e.kindExt,
                                  e.graphFamily);
                const f32 wrap = cellW - 6.0f*dpi_;
                const std::string label = fitLabel(e.name, wrap, 2);
                const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                const f32 tx = ts.x <= wrap ? o.x + (cellW - ts.x)*0.5f : o.x + 3.0f*dpi_;
                const f32 ty = o.y + tile + 2.0f*dpi_;
                const ImVec4 clip(o.x, o.y + tile, o.x + cellW, o.y + cellH);
                dl->AddText(nullptr, 0.0f, ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text),
                            label.c_str(), nullptr, wrap, &clip);

                // ---- the revision-control mark ----
                // Top-left, sized so it can't reach the icon: drawEntryIcon's glyph sits in a
                // prevH*0.58 box centred on the preview, so its left edge is ~0.21*cellW in from the
                // card. The dot's outer edge (2dp + 2r) stays short of that across the zoom slider's
                // 56..168dp range via the 0.075 factor and 7dp cap (binds above ~93dp). Drawn last
                // so nothing painted after buries it, same as the card over Selectable. The 3dp floor
                // guards against cbTileSize_ read straight from editor.ini (contentBrowser.tileSize)
                // with no clamp to the slider's range (a stored size of 10 would give a one-pixel
                // dot) -- at every size the slider itself offers, the proportional value wins.
                // Overlaps a rendered thumbnail's top-left corner (the one unavoidable spot): that
                // corner is letterboxed background, not part of the asset.
                if (rcHas) {
                    const f32 dotR = ImMin(ImMax(cellW * 0.075f, 3.0f * dpi_), 7.0f * dpi_);
                    const f32 inset = 2.0f * dpi_ + dotR;
                    rcStatusDot(dl, ImVec2(o.x + inset, o.y + inset), dotR, rcStatusColour(rcSt),
                                rcSt == editor::FileStatus::Conflicted);
                }
                ImGui::PopID();
            }
        }
    }
    clipper.End();
}

// Collects every FILE at or under `dir` whose name matches the search box. Pointers into dirCache_
// are safe here: it's a std::unordered_map with node-allocated values, so inserting folders during
// this walk rehashes the map but doesn't move the DirListing objects -- pointers into their entry
// vectors stay valid (a folder already listed this frame returns from cache untouched, per
// dirListing's 20-frame stamp). Files only: a matching folder would be a row that navigates rather
// than opens, mixed with rows that open -- two different meanings for one gesture.
void SandboxApp::cbGatherDeepMatches(const std::string& dir, std::vector<const DirEntry*>& out, int depth) {
    // A depth cap rather than a visited set: the content tree is a tree, and the one thing that
    // could make it not one is a directory symlink, which this bounds instead of chasing.
    constexpr int kMaxDepth = 16;
    if (depth > kMaxDepth) return;
    const DirListing& l = dirListing(dir);
    for (const DirEntry& e : l.entries) {
        if (e.isDir) cbGatherDeepMatches(e.full, out, depth + 1);
        else if (containsNoCase(e.name, cbFilter_)) out.push_back(&e);
    }
}

// Draws the breadcrumb and the folder's filtered entries, as tiles or as a list.
void SandboxApp::drawFolderFiles(std::string dir) {   // by value: a click below reassigns cbSelectedDir_
    drawBreadcrumb(dir);
    ImGui::Separator();

    const DirListing& listing = dirListing(dir);
    std::vector<const DirEntry*> shown;
    shown.reserve(listing.entries.size());
    if (cbFilter_[0] != '\0' && cbSearchDeep_) {
        // Searching subfolders too: previously the box only looked at the open folder, so finding
        // an asset meant already knowing which folder it was in. Only while a filter is typed: an
        // empty box would flatten the whole tree into one folder view and lose the hierarchy.
        cbGatherDeepMatches(dir, shown, 0);
    } else {
        for (const DirEntry& e : listing.entries)
            if (containsNoCase(e.name, cbFilter_)) shown.push_back(&e);
    }

    if (shown.empty()) {
        ImGui::TextDisabled(listing.entries.empty() ? "(this folder is empty)"
                                                    : "(nothing here matches the search)");
        return;
    }
    // What Ctrl+A selects, captured here since the key handler runs outside this function:
    // `shown` is what the search box left visible -- selecting filtered-away files would surprise
    // the moment the box is cleared.
    cbShownPaths_.clear();
    cbShownPaths_.reserve(shown.size());
    for (const DirEntry* p : shown) cbShownPaths_.push_back(p->full);

    if (cbGallery_) { drawFolderGallery(shown); return; }

    const f32 h = ImGui::GetTextLineHeight() * 1.3f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()), h);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const DirEntry& e = *shown[i];
            ImGui::PushID(i);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            // The row's trailing edge, taken BEFORE the widgets move the cursor: the mark is drawn
            // right-aligned there. See its own comment below for why it is not on the icon.
            const f32 rowRight = o.x + ImGui::GetContentRegionAvail().x;
            editor::FileStatus rcSt = editor::FileStatus::Unmodified;
            const bool rcHas = rcMarkFor(e.full, e.isDir, rcSt);
            ImGui::Dummy(ImVec2(h * 0.78f, h));
            ImGui::SameLine();
            drawEntryIcon(dl, ImVec2(o.x + h*0.39f, o.y + h*0.5f), h*0.82f, e.isDir, e.tile, e.module,
                          e.kindExt, e.graphFamily);
            if (ImGui::Selectable(e.name.c_str(), cbIsSelected(e.full),
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                cbClickSelect(shown, i);
                if (ImGui::IsMouseDoubleClicked(0) || (e.isDir && !cbDoubleClickEnter_)) {
                    cbSelection_.assign(1, e.full);
                    cbSelectedFile_ = e.full;
                    cbOpenEntry(e.full, e.isDir);
                }
            }
            if (rcHas && ImGui::IsItemHovered())
                ImGui::SetTooltip("git: %s%s", e.isDir ? "something under here is " : "",
                                  editor::statusName(rcSt));
            // One payload, the whole selection (see the gallery's note): folders drag too, and a
            // second SetDragDropPayload would overwrite the first -- exactly how viewport drops broke.
            if (ImGui::BeginDragDropSource()) {
                const std::string moveBlob = cbMoveDragPayloadFor(e.full);
                ImGui::SetDragDropPayload(kCbMoveDragDropType, moveBlob.c_str(), moveBlob.size() + 1);
                ImGui::TextUnformatted(cbDragLabel(e.name, moveBlob).c_str());
                ImGui::EndDragDropSource();
            }
            if (e.isDir) cbFolderDropTarget(e.full);
            cbItemContextMenu(e.full, e.name, e.isDir);
            // ---- the revision-control mark ----
            // Right-aligned in the row, not on the icon (opposite of the gallery, since the row is
            // the opposite shape): a list icon is about one text line across, so a corner badge
            // would cover a quarter of it. The row's trailing edge is free space a list has and a
            // card doesn't; the leading Dummy is the icon's own box and the glyph fills it. The
            // trade, stated rather than hidden: a long name reaching the drawer's edge runs under
            // the dot -- the alternative was reserving a column's width from every row for a mark
            // most rows lack.
            if (rcHas) {
                const f32 dotR = ImMin(ImMax(h * 0.20f, 3.0f * dpi_), 6.0f * dpi_);
                rcStatusDot(dl, ImVec2(rowRight - dotR - 2.0f * dpi_, o.y + h * 0.5f), dotR,
                            rcStatusColour(rcSt), rcSt == editor::FileStatus::Conflicted);
            }
            ImGui::PopID();
        }
    }
    clipper.End();
}

// Draws the Import modal: a source path and the destination folder.
// Must be a modal: "Browse..." calls openFileDialog, a native Win32 dialog that blocks this thread
// and moves OS focus away. A plain BeginPopup doesn't survive that reliably (imgui.h:850/2725);
// BeginPopupModal "cannot be closed by user" (imgui.h:855), so it's still there when the dialog
// returns. ProjectBrowser.cpp's own openFileDialog-from-popup is a BeginPopupModal too, not
// precedent for a plain popup.
void SandboxApp::drawImportModal() {
    if (!ImGui::BeginPopupModal("Import Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    // The Overwrite checkbox's own state, local rather than a member (means nothing once closed);
    // IsWindowAppearing() resets it for the next open, like cbRenameRepoint_'s sibling checkboxes.
    static bool s_overwrite = false;
    if (ImGui::IsWindowAppearing()) s_overwrite = false;
    ImGui::TextUnformatted("Import an asset into the selected folder.");
    ImGui::TextDisabled(".gltf/.glb become .ocmesh; .wav/.mp3/.m4a/.flac become .ocaudio.");
    ImGui::TextDisabled("Anything else is copied as-is.");
    ImGui::SetNextItemWidth(420.0f * dpi_);
    ImGui::InputText("Source file", importPath_, sizeof(importPath_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string start = importPath_[0] != '\0'
            ? std::filesystem::path(importPath_).parent_path().string() : std::string();
#if AVER_HAVE_AUDIO_IMPORT
        const char* kFilterLabel = "Importable assets (*.gltf, *.glb, *.wav, *.mp3, *.m4a, *.flac)";
        const char* kFilterSpec  = "*.gltf;*.glb;*.wav;*.mp3;*.m4a;*.flac";
#else
        const char* kFilterLabel = "Importable assets (*.gltf, *.glb)";
        const char* kFilterSpec  = "*.gltf;*.glb";
#endif
        std::string picked;
        if (openFileDialog("Import asset", kFilterLabel, kFilterSpec,
                           directoryExists(start) ? start : std::string(), picked)) {
            if (picked.size() < sizeof(importPath_)) {
                std::snprintf(importPath_, sizeof(importPath_), "%s", picked.c_str());
            } else {
                AVER_WARN("[Import] picked path is {} chars, longer than the {}-char field - not applied",
                          picked.size(), sizeof(importPath_) - 1);
                cbStatus_ = "Picked path is too long - not applied";
            }
        }
    }
    const std::string dest = cbSelectedDir_.empty() ? project_.contentDir() : cbSelectedDir_;
    const std::string blocked = cbImportBlockedReason(dest);
    if (blocked.empty()) {
        ImGui::Text("Into: %s", dest.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "Into: %s - %s", dest.c_str(), blocked.c_str());
    }

    // Reimport/overwrite: importAsset's collision check (and importGltfToDir's per-mesh one for a
    // model) looks at the file this import would actually become, not the source's own name. This
    // predicts the same name only to decide whether the checkbox below is worth showing -- a hint,
    // not the enforcement, which happens where each file's true name is computed (importGltfToDir's
    // own loop).
    bool wouldCollide = false;
    if (blocked.empty() && importPath_[0] != '\0') {
        std::error_code cec;
        const std::filesystem::path srcPath(importPath_);
        std::string ext = srcPath.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".gltf" || ext == ".glb") {
            wouldCollide = std::filesystem::exists(dest + "\\" + srcPath.stem().string() + ".ocmesh", cec);
        }
#if AVER_HAVE_AUDIO_IMPORT
        else if (fmt::isImportableAudio(importPath_)) {
            wouldCollide = std::filesystem::exists(
                dest + "\\" + std::filesystem::path(fmt::ocAudioPathFor(importPath_)).filename().string(), cec);
        }
#endif
        else {
            wouldCollide = std::filesystem::exists(dest + "\\" + srcPath.filename().string(), cec);
        }
    }
    if (wouldCollide) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                           ICON_WARNING " This would land on a file already here.");
        ImGui::Checkbox("Overwrite (goes to the recycle bin first)", &s_overwrite);
        ImGui::Separator();
    }

    ImGui::BeginDisabled(importPath_[0] == '\0' || !blocked.empty());
    if (ImGui::Button("Import")) {
        importAsset(importPath_, dest, wouldCollide && s_overwrite);
        importPath_[0] = '\0';
        s_overwrite = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Imports one asset into destDir: models and audio are converted, everything else is copied.
// UI-side: calls cbIsEditable/cbInvalidate/importModel/importAudio, all the content browser's.
// Its one non-browser caller (--import's deferred handshake in onInit) is guarded instead.
// One-line outcomes also go to a NOTIFICATION: cbStatus_ is the Content Browser's footer, visible
// only while that drawer is open, but an import can be started by a drag from Explorer with the
// drawer shut. Several messages said "see the Output Log" while naming a panel the user had no way
// to open; offerLog fixes that. Named for the shape, not the caller: started as import-only, now
// also the navmesh bake and GI cache flush.
void SandboxApp::notifyOutcome(editor::NotifySeverity sev, std::string title, std::string body,
                  bool offerLog) {
    editor::Notification n;
    n.severity = sev;
    n.title = std::move(title);
    n.body  = std::move(body);
    n.ttlSec = sev == editor::NotifySeverity::Error ? 10.0 : 6.0;
    if (offerLog) {
        n.actions[0] = editor::NotifyAction::ShowOutputLog;
        n.actionLabels[0] = "Show in Output Log";
    }
    editor::notifications().push(std::move(n));
}

// "Content" rather than ".", which is what cbRelativeToContent returns for the Content root
// itself -- a body reading "into ." is worse than no body at all.
std::string SandboxApp::importDestLabel(const std::string& absDir) const {
    const std::string rel = cbRelativeToContent(absDir);
    if (rel.empty() || rel == ".") return "Content";
    return "Content/" + rel;
}

void SandboxApp::importAsset(const std::string& src, const std::string& destDir, bool overwrite) {
    std::error_code ec;
    if (!cbIsEditable(destDir)) {
        AVER_WARN("[Import] '{}' is engine content and is read-only", destDir);
        cbStatus_ = "Engine content is read-only";
        notifyOutcome(editor::NotifySeverity::Warning, "Import refused",
                     "Engine content is read-only.");
        return;
    }
    // THESE TWO REPORTED NOTHING TO THE UI AT ALL -- not even into cbStatus_. A drag-and-drop of
    // a file that had moved, or of one already imported, simply appeared to do nothing.
    if (!std::filesystem::exists(src, ec)) {
        AVER_WARN("[Import] source not found: {}", src);
        notifyOutcome(editor::NotifySeverity::Warning, "Import failed", "Source not found: " + src);
        return;
    }
    const std::string name = std::filesystem::path(src).filename().string();
    const std::string dest = destDir + "\\" + name;
    if (std::filesystem::exists(dest, ec)) {
        // Overwrite, when the Import dialog offered it: this check matches the source's own name,
        // meaningful for a plain copy where `dest` IS the final output. A converted model/audio
        // file rarely collides here (the source itself is never copied); its overwrite handling
        // sits deeper (importGltfToDir's loop, importAudio's own check).
        if (overwrite) {
            cbDeleteEntry(dest);
        } else {
            AVER_WARN("[Import] '{}' already exists in {} - not overwritten; rename the source or remove it first", name, destDir);
            notifyOutcome(editor::NotifySeverity::Warning, "Already imported",
                         name + " exists here already and was not overwritten.");
            return;
        }
    }
    const std::string ext = std::filesystem::path(src).extension().string();
    std::string lower;
    for (const char c : ext) lower.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
    if (lower == ".gltf" || lower == ".glb") { importModel(src, destDir, overwrite); return; }
#if AVER_HAVE_AUDIO_IMPORT
    if (fmt::isImportableAudio(src)) { importAudio(src, destDir, overwrite); return; }
#endif

    std::filesystem::copy_file(src, dest, ec);
    if (ec) { AVER_WARN("[Import] failed to copy '{}' -> '{}': {}", src, dest, ec.message());
              cbStatus_ = "Import failed - see the Output Log";
              notifyOutcome(editor::NotifySeverity::Error, "Import failed", ec.message(), true);
              return; }
    AVER_INFO("[Import] imported '{}' into {}", name, destDir);
    cbStatus_ = "Imported " + name;
    // Content-relative, not absolute: unreadable at this width, and the reader already knows
    // which project is open. cbRelativeToContent falls back to absolute only when there is no project.
    notifyOutcome(editor::NotifySeverity::Success, "Imported " + name,
                 "into " + importDestLabel(destDir));
    cbInvalidate(destDir);
}

#endif

#if AVER_WITH_IMGUI
#if AVER_HAVE_AUDIO_IMPORT
// Decodes .wav / .mp3 / .m4a / .flac into one .ocaudio in destDir. The source is not copied.
void SandboxApp::importAudio(const std::string& src, const std::string& destDir, bool overwrite) {
    audio::SoundData data;
    const fmt::AudioImportResult r = fmt::audioImportFile(src, data);
    if (!r.ok) {
        AVER_WARN("[Import] {} could not be decoded: {}",
                  std::filesystem::path(src).filename().string(), r.error);
        cbStatus_ = "Import failed - see the Output Log";
        notifyOutcome(editor::NotifySeverity::Error,
                     "Could not decode " + std::filesystem::path(src).filename().string(),
                     r.error, true);
        return;
    }

    const std::string outName = std::filesystem::path(fmt::ocAudioPathFor(src)).filename().string();
    const std::string out = destDir + "\\" + outName;
    std::error_code ec;
    if (std::filesystem::exists(out, ec)) {
        // Overwrite: same choice importAsset's top check honours (see its comment) -- the checkpoint
        // that matters for audio, since outName (.ocaudio) is never what that check compares against.
        if (overwrite) {
            cbDeleteEntry(out);
        } else {
            AVER_WARN("[Import] '{}' already exists in {} - not overwritten", outName, destDir);
            cbStatus_ = "Already imported";
            notifyOutcome(editor::NotifySeverity::Warning, "Already imported",
                         outName + " exists here already and was not overwritten.");
            return;
        }
    }

    std::string why;
    if (!fmt::saveOcAudio(out, data, std::filesystem::path(src).filename().string(), &why)) {
        AVER_WARN("[Import] could not write '{}': {}", out, why);
        cbStatus_ = "Import failed - see the Output Log";
        notifyOutcome(editor::NotifySeverity::Error, "Could not write " + outName, why, true);
        return;
    }

    const f64 seconds = data.sampleRate > 0
                      ? static_cast<f64>(data.samples.size()) /
                        static_cast<f64>(data.channels ? data.channels : 1) /
                        static_cast<f64>(data.sampleRate) : 0.0;
    AVER_INFO("[Import] '{}' -> {} ({} via {}, {} ch, {} Hz, {:.2f} s)",
              std::filesystem::path(src).filename().string(), outName,
              data.samples.size(), r.decoder, data.channels, data.sampleRate, seconds);
    cbStatus_ = "Imported " + outName;
    notifyOutcome(editor::NotifySeverity::Success, "Imported " + outName,
                 std::to_string(data.channels) + " ch, " +
                     std::to_string(data.sampleRate) + " Hz");
    cbInvalidate(destDir);
}

#endif
#endif

#if AVER_WITH_IMGUI
// Converts a glTF/GLB into one .ocmesh per mesh in destDir, and registers them for this session.
// The actual conversion is importGltfToDir (file scope, above the class) so that the same logic
// is also reachable from the --import-gltf CLI flag, which has no SandboxApp to call a member on.
void SandboxApp::importModel(const std::string& src, const std::string& destDir, bool overwrite) {
    GltfImportSummary sum;
    std::string why;
    const std::string srcName = std::filesystem::path(src).filename().string();
    if (!importGltfToDir(src, destDir, project_.contentDir(), overwrite, sum, &why)) {
        AVER_WARN("[Import] {}", why);
        cbStatus_ = "Import failed - see the Output Log";
        notifyOutcome(editor::NotifySeverity::Error, "Could not import " + srcName, why, true);
        return;
    }
    if (sum.meshesWritten == 0 && sum.rigsWritten == 0 && sum.clipsWritten == 0) {
        cbStatus_ = "Import produced nothing - see the Output Log";
        notifyOutcome(editor::NotifySeverity::Warning, "Nothing imported from " + srcName,
                     "The file parsed but contained no meshes, skeletons or clips.", true);
        return;
    }
    std::string counts = std::to_string(sum.meshesWritten) + " mesh(es), " +
                         std::to_string(sum.rigsWritten) + " skeleton(s) and " +
                         std::to_string(sum.clipsWritten) + " clip(s)";
    if (sum.materialsWritten || sum.texturesWritten)
        counts += ", with " + std::to_string(sum.materialsWritten) + " material(s) and " +
                  std::to_string(sum.texturesWritten) + " texture(s)";
    cbStatus_ = "Imported " + counts + " from " + srcName;
    // The outcome of a slow operation, which is precisely what a notification is for.
    notifyOutcome(editor::NotifySeverity::Success, "Imported " + srcName, counts);
    cbInvalidate(destDir);
    wantMeshReload_ = true;
}

// Imports every path a drag from outside the editor dropped, into the Content Browser's current
// folder -- one call per path through importAsset, the same funnel the Import... dialog and a
// browser-internal drag go through, so a drop gets identical dispatch/overwrite/notifications.
// Folders are skipped with a note rather than imported recursively: nothing here decides how a
// dropped directory's contents should be laid out, and silently flattening it would surprise.
// Unsupported extensions aren't filtered here either -- importAsset already decides what it can
// do with a file (convert, copy, or refuse).
void SandboxApp::importDroppedFiles(const std::vector<std::string>& paths) {
    const std::string destDir = cbSelectedDir_.empty() ? project_.contentDir() : cbSelectedDir_;
    for (const std::string& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            AVER_WARN("[Import] '{}' is a folder - drop its files individually", std::filesystem::path(p).filename().string());
            notifyOutcome(editor::NotifySeverity::Warning, "Folder not imported",
                         std::filesystem::path(p).filename().string() + " is a folder - drop its files individually.");
            continue;
        }
        importAsset(p, destDir);
    }
}

#endif

} // namespace aver
