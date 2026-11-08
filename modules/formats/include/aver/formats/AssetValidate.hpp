#pragma once
// Asset data validation: rules registered per file extension, run over a content tree, reported as
// a flat issue list. Headless (AverAssetC validate) and in-editor (the `validate` console command and
// the Content Browser's Validate Assets) use the same validator.
//
// A rule looks at one file and appends issues. It never mutates anything and never needs a device.
// Built-in rules (see defaultRules()):
//   missing-reference    a text asset names a content file that is not there
//   ocmat-parse          a .ocmat with no OCMAT header, or whose GRAPHREF target is missing
//   ocgraph-parse        a .ocgraph that does not parse, links to unknown nodes, or repeats a node id
//   mesh-slots           a .ocmesh submesh whose material slot or index range is out of bounds
//   texture-dimensions   a PNG/JPEG/TGA whose size breaks block compression (not a multiple of 4)
//                        or mip chains (not a power of two, reported as info)
#include "aver/core/Types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::fmt {

enum class ValidateSeverity : u8 { Info, Warning, Error };

struct ValidateIssue {
    ValidateSeverity severity = ValidateSeverity::Warning;
    std::string rule;
    std::string asset;     // content-relative, forward slashes
    std::string message;
};

struct ValidateReport {
    std::vector<ValidateIssue> issues;
    u32 filesChecked = 0;
    u32 count(ValidateSeverity s) const;
};

struct ValidateContext {
    std::string contentDir;
    // Extra roots a reference may resolve against (engine-supplied built-ins such as Meshes/cube.ocmesh).
    std::vector<std::string> extraRoots;
    // True when `relPath` exists under the content dir or any extra root (case-insensitive on Windows).
    bool exists(const std::string& relPath) const;
};

using ValidateRuleFn = std::function<void(const ValidateContext&, const std::string& relPath, ValidateReport&)>;

struct ValidateRule {
    std::string name;
    std::vector<std::string> extensions;   // lower-case with the dot; empty = every file
    ValidateRuleFn run;
};

class AssetValidator {
public:
    AssetValidator();                       // starts with defaultRules()
    explicit AssetValidator(std::vector<ValidateRule> rules) : rules_(std::move(rules)) {}
    void addRule(ValidateRule r) { rules_.push_back(std::move(r)); }
    const std::vector<ValidateRule>& rules() const { return rules_; }

    // Validates one content-relative file.
    void validateFile(const ValidateContext& ctx, const std::string& relPath, ValidateReport& out) const;
    // Walks the content dir, sorted, and validates every file a rule applies to.
    ValidateReport validateAll(const ValidateContext& ctx) const;

private:
    std::vector<ValidateRule> rules_;
};

std::vector<ValidateRule> defaultRules();
const char* validateSeverityName(ValidateSeverity s);
// "[error] Materials/M.ocmat: message (rule)"
std::string formatValidateIssue(const ValidateIssue& i);

}  // namespace aver::fmt
