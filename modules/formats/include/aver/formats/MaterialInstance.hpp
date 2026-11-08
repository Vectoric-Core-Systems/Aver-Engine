#pragma once
// Material instances and material functions (UE5 parity), the pure-data half: no renderer, no device.
//
//  .ocmati   an instance: PARENT (a .ocmat or another .ocmati), named graph-parameter overrides, and verbatim
//            .ocmat records (PARAM/TEX/...) applied over the parent. Resolution walks the chain, detects cycles
//            and merges child over parent.
//  parameters  ScalarParameter / VectorParameter nodes in a DOMAIN material graph (`name=` on the node line, the
//            default on the output pin). An instance's values replace the defaults before the graph compiles.
//  functions  a `DOMAIN materialfunction` .ocgraph with FunctionInput / FunctionOutput nodes (`name=`), called from a
//            material graph by a CallFunction node (`function=<content path>`); inlined before compile.
#include "aver/formats/OcGraph.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aver::fmt {

inline constexpr int kOcMatiVersion = 1;

struct OcMatInstance {
    std::string name;     // optional; empty = the file stem
    std::string parent;   // content-relative path of a .ocmat or .ocmati
    // name -> literal ("0.5" for a scalar, "1,0.2,0.1,1" for a vector), in file order.
    std::vector<std::pair<std::string, std::string>> params;
    // .ocmat records applied after the parent's text, verbatim, in file order.
    std::vector<std::string> records;
};

bool parseOcmati(std::string_view text, OcMatInstance& out, std::string* err = nullptr);
std::string writeOcmati(const OcMatInstance& m);
bool loadOcmati(const std::string& path, OcMatInstance& out, std::string* err = nullptr);
bool saveOcmati(const std::string& path, const OcMatInstance& m, std::string* err = nullptr);

bool isMaterialInstancePath(std::string_view path);   // ends in .ocmati, case-insensitive

// Reads the text of a content-relative file.
using MaterialTextReader = std::function<bool(const std::string& contentPath, std::string& text)>;

inline constexpr usize kMaxInstanceChain = 32;

struct ResolvedMaterialInstance {
    std::string basePath;                 // the .ocmat at the root of the chain
    std::string baseText;
    std::vector<std::string> records;     // root-first, so a child's record wins
    std::vector<std::pair<std::string, std::string>> params;   // merged, child wins, sorted by name
    std::vector<std::string> chain;       // the instance first, then each parent, base last
};

// Follows PARENT links from `path` (an .ocmati) to the .ocmat at the root. False and *err on a cycle, a chain
// longer than kMaxInstanceChain, a missing file or a missing PARENT.
bool resolveMaterialInstance(const std::string& path, const MaterialTextReader& read,
                             ResolvedMaterialInstance& out, std::string* err = nullptr);

// The .ocmat text an instance stands for: the base text with the records appended.
std::string composeInstanceOcmat(const ResolvedMaterialInstance& r);

// ---- graph parameters ----

struct MaterialGraphParameter {
    std::string name;
    bool isVector = false;       // VectorParameter (float4) or ScalarParameter (float)
    std::string defaultValue;
};

std::vector<MaterialGraphParameter> listMaterialGraphParameters(const OcGraphData& g);

// Replaces the default of every parameter node whose name an override names. A name no node carries is
// appended to *unknown (not an error: the parent graph may have lost a parameter). False and *err for a value
// that is not a number list of the right width.
bool applyMaterialGraphParameters(OcGraphData& g, const std::vector<std::pair<std::string, std::string>>& params,
                                  std::vector<std::string>* unknown = nullptr, std::string* err = nullptr);

// A stable text for a parameter set, for registry keys: sorted `name=value;`.
std::string materialParameterKey(std::vector<std::pair<std::string, std::string>> params);

// ---- material functions ----

using MaterialGraphReader = std::function<bool(const std::string& contentPath, OcGraphData& out, std::string* err)>;

inline constexpr usize kMaxFunctionDepth = 16;

// Replaces every CallFunction node in `g` with the body of the function it names, recursively. False and *err
// for a cycle (naming the chain), a missing function, a call to an output the function lacks, or more than
// kMaxFunctionDepth levels.
bool inlineMaterialFunctions(OcGraphData& g, const MaterialGraphReader& read, std::string* err = nullptr);

} // namespace aver::fmt
