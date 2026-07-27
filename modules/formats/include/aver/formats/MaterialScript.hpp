#pragma once
// Rewriting a C# material source from an edited material.
//
// The Details panel edits a pbr::MaterialDesc. With C# as the source of a surface, saving that edit
// has to go back to the .cs file -- writing the .ocmat instead would write a build artefact that the
// next compile overwrites, which is a change that appears to work and then silently vanishes.
//
// This is the same machine docs/DESIGNER_REWRITE.md describes for actors, at the scale of one
// method: find the [AverMaterial] class, replace the body of its Configure with the current values,
// and leave every other byte of the file alone.
//
// WHAT IT PRESERVES, and why each matters:
//   - everything outside Configure: usings, namespace, other classes, XML doc comments, attributes
//   - the .Comment(...) calls inside it, which are authored PROSE and not derivable from a
//     MaterialDesc. Losing them on the first save would be losing the reasoning behind the numbers,
//     which is the part of a material file worth having.
//   - the file's existing indentation of the chain, so a save does not reformat somebody's file.
//
// It writes only values that DIFFER FROM THE DEFAULTS. A material that sets nothing produces an
// almost empty Configure rather than eighteen lines restating the defaults, which is what keeps a
// generated-then-edited file readable enough to keep editing.
#include "aver/formats/OcMat.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// Rewrites the Configure body of the class marked [AverMaterial("<boundName>")].
//
// Returns false with `err` set when the class is absent, its Configure cannot be found, or the body
// is a shape this does not understand -- and in every one of those cases `out` is left untouched. A
// rewriter that half-succeeds on a file it did not parse is worse than one that declines.
bool rewriteMaterialScript(std::string_view csText, const std::string& boundName,
                           const pbr::MaterialDesc& d, const OcMatExtras* extras,
                           std::string& out, std::string* err = nullptr);

// The fluent chain alone, for a caller building a NEW file rather than editing one. `indent` is
// prepended to every line after the first; `comments` are emitted as .Comment(...) calls ahead of
// the values.
std::string materialConfigureChain(const pbr::MaterialDesc& d, const OcMatExtras* extras,
                                   const std::vector<std::string>& comments,
                                   const std::string& indent);

// A whole .cs file for a material that has none yet -- what "new material" produces.
std::string newMaterialScript(const std::string& boundName, const std::string& csharpNamespace,
                              const pbr::MaterialDesc& d, const OcMatExtras* extras);

} // namespace aver::fmt
