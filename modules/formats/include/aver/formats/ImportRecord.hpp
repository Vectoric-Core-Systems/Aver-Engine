#pragma once
// .ocimport -- the record an import leaves beside its outputs, so the Content Browser can Reimport with
// the options it used. Line text; lines this version does not know are kept and written back, so a
// newer editor's record survives an older one touching it.
//
//   OCIMPORT 1
//   FORMAT fbx
//   SOURCE Source/Hero.fbx          project-relative when inside the project, else absolute
//   SOURCESIZE 1843201
//   SOURCEMTIME 133901234567890000
//   OPTIONS scale=100;rootMotion=auto;...
//   OUTPUT mesh Hero.ocmesh         one per file written; the path is relative to the record's folder
//   OUTPUT skeleton Hero.ocskel
//
// In the base Aver.Formats target: text and arithmetic, no ufbx.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kImportRecordVersion = 1;

struct ImportRecordOutput {
    std::string kind;    // mesh, skeleton, anim, material, texture, world
    std::string file;    // relative to the record's folder
    bool operator==(const ImportRecordOutput&) const = default;
};

struct ImportRecord {
    u32 version = kImportRecordVersion;
    std::string format;                    // "fbx"
    std::string source;                    // see above; forward slashes when relative
    u64 sourceSize = 0;                    // bytes, 0 = unknown
    i64 sourceMTime = 0;                   // file time as the writer read it, 0 = unknown
    std::string options;                   // fbxOptionsToText
    std::vector<ImportRecordOutput> outputs;
    std::vector<std::string> extraLines;   // unknown and comment lines, in file order
    bool operator==(const ImportRecord&) const = default;
};

// False with `why` (naming the line) on a malformed record. A record newer than this reader is refused.
bool parseImportRecord(std::string_view text, ImportRecord& out, std::string* why = nullptr);
// LF-terminated lines, known lines first in the order above, then extraLines.
std::string writeImportRecordText(const ImportRecord& in);

bool loadImportRecord(const std::string& path, ImportRecord& out, std::string* why = nullptr);
bool saveImportRecord(const std::string& path, const ImportRecord& in, std::string* why = nullptr);

// The record's source as an absolute path: `source` itself when it is already absolute, otherwise joined
// onto `projectDir`.
std::string importRecordSourcePath(const ImportRecord& r, const std::string& projectDir);
// "<destDir>/<stem>.ocimport"
std::string importRecordPathFor(const std::string& destDir, const std::string& stem);
// True when `file` (a name relative to the record's folder, any case, either slash) is one of the outputs.
bool importRecordHasOutput(const ImportRecord& r, std::string_view file);

} // namespace aver::fmt
