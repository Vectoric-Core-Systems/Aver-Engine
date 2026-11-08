#pragma once
// The pure halves of the Place Actors, Message Log and Undo History panels (editor polish step 9).
// ImGui-free so EditorPanelModelsTest links Aver.Core only; the drawing lives in SandboxToolPanels.cpp.
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

inline std::string lowerAscii(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
// `needleLower` must already be lower case.
inline bool containsNoCase(std::string_view hay, std::string_view needleLower) {
    return needleLower.empty() || lowerAscii(hay).find(needleLower) != std::string::npos;
}

// ---- Place Actors ----------------------------------------------------------------------------
// One spawnable thing. `kind` is what the spawn dispatch switches on and what a drag carries.
struct PlaceItem {
    std::string kind;       // "cube", "sphere", "prim:<mesh path>", "light.point", "light.spot", "light.rect", "decal", "playerstart", "postvolume"
    std::string label;
    std::string category;   // "Basic", "Shapes", "Lights", "Volumes", "Effects", "Gameplay"
    std::string icon;       // an ICON_* literal, "" for none
};

// Items whose label or category contains `query` (case-insensitive) and, when `category` is
// non-empty, belong to it. Order is kept.
inline std::vector<PlaceItem> filterPlaceItems(const std::vector<PlaceItem>& all, std::string_view query,
                                               std::string_view category) {
    const std::string q = lowerAscii(query);
    std::vector<PlaceItem> out;
    for (const PlaceItem& it : all) {
        if (!category.empty() && it.category != category) continue;
        if (!containsNoCase(it.label, q) && !containsNoCase(it.category, q)) continue;
        out.push_back(it);
    }
    return out;
}
// Distinct categories in first-seen order.
inline std::vector<std::string> placeCategories(const std::vector<PlaceItem>& all) {
    std::vector<std::string> out;
    for (const PlaceItem& it : all)
        if (std::find(out.begin(), out.end(), it.category) == out.end()) out.push_back(it.category);
    return out;
}

// ---- Message Log -----------------------------------------------------------------------------
// The "[Tag]" prefix every AVER_* line carries, without brackets; empty when there is none.
inline std::string_view logCategoryOf(std::string_view msg) {
    if (msg.empty() || msg.front() != '[') return {};
    const usize close = msg.find(']');
    if (close == std::string_view::npos || close == 1 || close > 40) return {};
    return msg.substr(1, close - 1);
}

struct LogFilter {
    LogLevel    minLevel = LogLevel::Trace;
    std::string category;    // empty = every category
    std::string query;       // case-insensitive substring of the line
};
inline bool logLineMatches(const LogFilter& f, LogLevel level, std::string_view text) {
    if (static_cast<int>(level) < static_cast<int>(f.minLevel)) return false;
    if (!f.category.empty() && logCategoryOf(text) != f.category) return false;
    return containsNoCase(text, lowerAscii(f.query));
}

// ---- Undo History ----------------------------------------------------------------------------
// Rows are the undo stack oldest first (0..undoCount-1), then the redo stack nearest first.
// "State after row i" has i+1 commands applied; row -1 is the state before any. Returns the number
// of steps to take to land there: negative = undo that many, positive = redo that many.
inline int undoHistorySteps(usize undoCount, usize redoCount, int row) {
    const int total = static_cast<int>(undoCount + redoCount);
    row = std::max(-1, std::min(row, total - 1));
    return (row + 1) - static_cast<int>(undoCount);
}

} // namespace aver::editor
