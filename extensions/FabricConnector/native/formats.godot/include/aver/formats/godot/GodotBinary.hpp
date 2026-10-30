#pragma once
// Godot 4's binary resource files (.res, .scn, and any other "RSRC" file) read into the same document models the text parsers fill, so the
// converter does not care which spelling a file came from. Worked out from how Godot 4 saves and loads the format (read for behaviour; nothing
// copied) and from real files. Contract: docs/plans/fabric/G3_FORMATS.md.
//
// THE CONTAINER. "RSRC", then little-endian (a big-endian file is read too): endian flag, real-is-double flag, engine major, minor, format
// version; the main class name; the import-metadata offset; flags (named scene ids, uids, double reals, script class), the uid, an optional
// script class, 11 reserved words; a string table; the external resources (type, path, uid); the internal resources (path, offset). Each internal
// resource is its class name, a property count and (string-table index, Variant) pairs; the LAST one is the file's main resource and the others
// are its sub-resources, named by the part of their "local://" path after the scheme. A Variant is a 32-bit type tag and a payload; an Object
// payload is an external-resource index or an internal-resource index. The file ends with "RSRC". Compressed files ("RSCC") are that same
// stream, minus its leading "RSRC", cut into blocks; each block is a Zstandard frame (the only compression Godot 4 writes).
//
// WHAT COMES OUT.
//   parseGodotBinaryResource: header.kind "gd_resource", header.type = the main class, props = the main resource's properties, sub-resources as
//     [sub_resource] entries, external resources as [ext_resource] entries (ids "1", "2", ...). Values: ExtResource / SubResource variants
//     like the text parser's, so everything downstream reads them unchanged. A nil "script" property is dropped (the text form omits it).
//   parseGodotBinaryScene: the main resource must be a PackedScene; its "_bundled" table is unpacked into nodes (parents before children, paths
//     resolved as in a .tscn), property values, groups, instances, node-path properties, signal connections, editable instances and the
//     base scene of an inherited scene. A node's sibling-order `index` is not recovered (left at -1; children come in file order).
// Format versions 3 to 6 and engine 4.x are read; a newer format version reads with a warning. Godot 3 binaries (engine major 3) are refused.
// Input is untrusted: every length is checked against the buffer, counts against what could fit, nesting and the number of values are capped, the
// decompressed size is capped (kGodotBinaryMaxBytes), and nothing throws. On a bad file the function returns false with a "godot.binary_resource"
// error naming what is wrong; `out` holds whatever was read before it.
#include "aver/formats/godot/GodotScene.hpp"

#include <string>
#include <string_view>

namespace aver::fmt::godot {

// The most a compressed file may decompress to.
inline constexpr usize kGodotBinaryMaxBytes = usize(512) << 20;

// "uid://..." for a 64-bit resource uid, as Godot spells it (base 34: a-y then 0-8). A negative id (the invalid marker) is empty.
std::string godotUidText(u64 id);

// What the first bytes of a binary resource say, for the project scan (uid map) and the report.
struct GodotBinaryHeader {
    bool compressed = false;
    int majorVersion = 0, minorVersion = 0, formatVersion = 0;
    bool bigEndian = false, realIsDouble = false;
    std::string className;     // the main resource's class ("PackedScene", "ArrayMesh", ...)
    std::string scriptClass;
    std::string uid;           // "uid://..." or empty
};

// Reads the header from the start of a file. `complete` says the buffer is the whole file; with a prefix, the call returns false with *needMore
// set when more of the file is needed (a compressed file's first block). A failure fills *why.
bool readGodotBinaryHeader(const u8* data, usize size, bool complete, GodotBinaryHeader& out, std::string* why, bool* needMore = nullptr);

bool parseGodotBinaryResource(const u8* data, usize size, std::string_view file, GodotResource& out, GodotDiagnostics& diag);
bool parseGodotBinaryScene(const u8* data, usize size, std::string_view file, GodotScene& out, GodotDiagnostics& diag);

}  // namespace aver::fmt::godot
