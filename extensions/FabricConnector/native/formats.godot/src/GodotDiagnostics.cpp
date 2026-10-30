#include "aver/formats/godot/GodotDiagnostics.hpp"

namespace aver::fmt::godot {

std::string GodotDiag::str() const {
    std::string s = file.empty() ? std::string("<godot>") : file;
    if (line > 0) {
        s += ':';
        s += std::to_string(line);
    }
    s += severity == GodotSeverity::Error ? ": error: " : severity == GodotSeverity::Warning ? ": warning: " : ": note: ";
    s += message;
    return s;
}

void GodotDiagnostics::add(GodotSeverity sev, std::string_view file, int line, std::string_view code, std::string_view message) {
    GodotDiag d;
    d.severity = sev;
    d.file = std::string(file);
    d.line = line;
    d.code = std::string(code);
    d.message = std::string(message);
    items.push_back(std::move(d));
}

usize GodotDiagnostics::errorCount() const {
    usize n = 0;
    for (const GodotDiag& d : items) n += d.severity == GodotSeverity::Error;
    return n;
}

usize GodotDiagnostics::warningCount() const {
    usize n = 0;
    for (const GodotDiag& d : items) n += d.severity == GodotSeverity::Warning;
    return n;
}

std::string GodotDiagnostics::format() const {
    std::string out;
    for (const GodotDiag& d : items) {
        out += d.str();
        out += '\n';
    }
    return out;
}

}  // namespace aver::fmt::godot
