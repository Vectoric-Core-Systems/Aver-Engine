#pragma once
// Rewriting a C# material source from an edited pbr::MaterialDesc: replace the body of the
// [AverMaterial] class's Configure, preserve its .Comment(...) calls and indentation, and leave every
// other byte of the file alone. Only values that differ from the defaults are written.
#include "aver/formats/OcMat.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// Rewrites the Configure body of the class marked [AverMaterial("<boundName>")]. Returns false with
// `err` set, and `out` untouched, when the class, its Configure or its body cannot be parsed.
bool rewriteMaterialScript(std::string_view csText, const std::string& boundName,
                           const pbr::MaterialDesc& d, const OcMatExtras* extras,
                           std::string& out, std::string* err = nullptr);

// The fluent chain alone, for building a new file. `indent` is prepended to every line after the
// first; `comments` are emitted as .Comment(...) calls ahead of the values.
std::string materialConfigureChain(const pbr::MaterialDesc& d, const OcMatExtras* extras,
                                   const std::vector<std::string>& comments,
                                   const std::string& indent);

// A whole .cs file for a material that has none yet.
std::string newMaterialScript(const std::string& boundName, const std::string& csharpNamespace,
                              const pbr::MaterialDesc& d, const OcMatExtras* extras);

} // namespace aver::fmt
