#pragma once
// Diagnostics for the Godot text readers: every problem carries file:line, a severity and a short
// machine-readable code, so the converter can fold them into FabricReport. Nothing here throws.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot {

enum class GodotSeverity : u8 { Info, Warning, Error };

struct GodotDiag {
    GodotSeverity severity = GodotSeverity::Error;
    std::string file;      // as given to the reader (res:// path or disk path)
    int line = 0;          // 1-based; 0 when the problem is about the whole file
    std::string code;      // e.g. "godot.parse", "godot.binary_resource"
    std::string message;

    // "file:line: error: message"
    std::string str() const;
};

struct GodotDiagnostics {
    std::vector<GodotDiag> items;

    void add(GodotSeverity sev, std::string_view file, int line, std::string_view code, std::string_view message);
    void error(std::string_view file, int line, std::string_view message, std::string_view code = "godot.parse") {
        add(GodotSeverity::Error, file, line, code, message);
    }
    void warn(std::string_view file, int line, std::string_view message, std::string_view code = "godot.warn") {
        add(GodotSeverity::Warning, file, line, code, message);
    }
    void info(std::string_view file, int line, std::string_view message, std::string_view code = "godot.info") {
        add(GodotSeverity::Info, file, line, code, message);
    }

    usize errorCount() const;
    usize warningCount() const;
    bool hasErrors() const { return errorCount() != 0; }
    // Every item, one per line.
    std::string format() const;
};

}  // namespace aver::fmt::godot
