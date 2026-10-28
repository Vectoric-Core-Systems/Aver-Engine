#pragma once
// FabricReport.md / FabricReport.jsonl: every node, resource, property and file the converter looked at, rated.
// Schema in docs/plans/fabric/G3_SCENES.md.
#include <set>
#include <string>
#include <vector>

namespace aver::fabric {

enum class Rating { Converted, Approximated, Unsupported };

const char* ratingName(Rating r);

struct ReportEntry {
    std::string kind;       // project | setting | file | scene | node | resource | property | input | script
    std::string file;       // res:// path of the file the item came from ("" for project-wide items)
    std::string node;       // scene-relative node path, "" when not about a node
    std::string type;       // the Godot class or resource type
    std::string property;   // "" when about the whole item
    Rating rating = Rating::Converted;
    std::string reason;     // required for approximated and unsupported; a short note otherwise
    int line = 0;
};

class Report {
public:
    // Identical entries (same kind, file, node, type, property, rating and reason) are kept once.
    void add(ReportEntry e);
    void add(const char* kind, const std::string& file, const std::string& node, const std::string& type,
             const std::string& property, Rating rating, const std::string& reason, int line = 0);

    const std::vector<ReportEntry>& entries() const { return entries_; }
    size_t count(Rating r) const;
    size_t count(Rating r, const std::string& kind) const;

    bool writeJsonl(const std::string& path) const;
    bool writeMarkdown(const std::string& path, const std::string& projectName) const;

    // Reads a FabricReport.jsonl (what writeJsonl and Aver.Fabric.Convert write) back, replacing the entries held. False when the file
    // cannot be read; a line that is not a report entry is skipped.
    bool readJsonl(const std::string& path);
    void clear() { entries_.clear(); seen_.clear(); }

    // The unsupported items grouped by what they are (class and property, else the class or file kind), most frequent first.
    struct Feature {
        std::string name;       // "Environment.ssao_enabled", "AnimationPlayer", "script CS0246"
        size_t count = 0;
        std::string reason;     // the first reason seen
    };
    std::vector<Feature> topUnsupported(size_t limit) const;

private:
    std::vector<ReportEntry> entries_;
    std::set<std::string> seen_;
};

std::string jsonString(const std::string& s);   // a quoted, escaped JSON string

}  // namespace aver::fabric
