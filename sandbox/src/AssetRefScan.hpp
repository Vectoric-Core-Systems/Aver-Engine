// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Finding one asset path inside another file's text, at a PATH-SEGMENT BOUNDARY.
//
// WHY THIS EXISTS. The Content Browser's "who references this asset" scan matched with a bare
// `hay.find(want)` and no boundary test at all. Every reference here is a content-relative path, so
// the needle for `Meshes/Cube.ocmesh` is that whole string -- and it is a SUBSTRING of
// `PropMeshes/Cube.ocmesh`, of `Sub/Meshes/Cube.ocmesh`, and of anything else ending the same way.
// The scan therefore reported files that reference a DIFFERENT, unrelated asset.
//
// As a warning that was merely noisy. It stops being merely noisy the moment anything REWRITES what
// the scan finds: a rename that string-replaced every hit would silently repoint
// `PropMeshes/Cube.ocmesh` at the new name and break an asset the author never touched. Anchoring is
// the precondition for any rewrite, which is why it is extracted, header-only and tested on its own
// rather than left inline in a 24,000-line file.
//
// THE RULE IS THE ONE THE FILE ALREADY USES ELSEWHERE. SandboxApp's cbRewriteHistory decides whether
// a browser-history entry sits under a renamed folder with "equal, or followed by a separator". This
// is the same idea generalised to both ends: a match counts only when the characters on either side
// of it cannot be part of a path.
//
// HEADER-ONLY AND std-ONLY, so tests/editor can drive it with no device, no ImGui and no sandbox
// translation unit -- the same trade EditorEuler.hpp and DropPlacement.hpp already make.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

// Characters that can be part of a path, in the NORMALISED form the scan compares in: lower-cased
// with every backslash already turned into a forward slash.
//
// '/' IS INCLUDED, and that is the important one. If the character before a match is a separator the
// needle is a SUFFIX of a longer path -- `Meshes/Cube.ocmesh` inside `Props/Meshes/Cube.ocmesh` --
// which names a different file in a different folder, not this one.
//
// A DELIMITER is anything else: a quote, whitespace, '=', '{', a line break, the ends of the buffer.
// Those are what actually surround a reference in every format the scan reads.
inline bool isAssetPathChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == '/';
}

// Is the match of `len` bytes at `at` a whole path, rather than the tail or head of a longer one?
inline bool isAnchoredAssetRef(std::string_view hay, std::size_t at, std::size_t len) {
    if (at > hay.size() || len == 0 || at + len > hay.size()) return false;
    if (at > 0 && isAssetPathChar(hay[at - 1])) return false;
    const std::size_t end = at + len;
    if (end < hay.size() && isAssetPathChar(hay[end])) return false;
    return true;
}

// Every anchored occurrence of `needle` in `hay`, as byte offsets. BOTH ARE EXPECTED PRE-NORMALISED
// (lower-cased, forward slashes) -- normalising here would mean doing it per call on the whole file.
//
// OFFSETS, NOT A BOOL, because a rewriter needs to splice at these positions in the ORIGINAL text.
// The scan normalises a COPY to compare against, so the offsets have to index a buffer whose bytes
// line up with the original one-for-one; lower-casing and swapping '\\' for '/' both preserve length,
// which is what makes that safe and is the reason neither step is allowed to become anything cleverer.
inline std::vector<std::size_t> findAnchoredAssetRefs(std::string_view hay, std::string_view needle) {
    std::vector<std::size_t> out;
    if (needle.empty() || hay.size() < needle.size()) return out;
    for (std::size_t i = hay.find(needle); i != std::string_view::npos;
         i = hay.find(needle, i + 1)) {
        if (isAnchoredAssetRef(hay, i, needle.size())) out.push_back(i);
    }
    return out;
}

// True when `hay` references `needle` at least once. What the Content Browser's scan actually asks.
inline bool referencesAsset(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (std::size_t i = hay.find(needle); i != std::string_view::npos;
         i = hay.find(needle, i + 1)) {
        if (isAnchoredAssetRef(hay, i, needle.size())) return true;
    }
    return false;
}

// Lower-cases and forward-slashes in place, LENGTH-PRESERVINGLY. Both callers need exactly this and
// need it to keep offsets valid; see findAnchoredAssetRefs for why that property is load-bearing.
inline std::string normaliseForRefScan(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

} // namespace aver::editor
