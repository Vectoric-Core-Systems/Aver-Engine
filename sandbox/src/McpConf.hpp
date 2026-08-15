#pragma once
// Reads the optional mcp.conf at the engine root: developer-local port assignments for the editor's
// MCP control channel (Aver.Mcp) and the out-of-process MCP tool server (tools/mcp/aver_mcp.py). See
// mcp.conf.example at the repo root for the full format and every key's built-in default.
//
// COMPOSITION-ROOT ONLY, DELIBERATELY. This lives in sandbox/src, not modules/mcp: a module must not
// learn to find, read or parse a config file, or know a repo layout exists at all -- McpBridge::start()
// still just takes a bare u16 and knows nothing about files or paths. SandboxApp.cpp is what reads
// this and hands the bridge a resolved port, the same shape the rest of the engine already uses
// (EngineScaffold's engineRoot(), EditorPrefs's editor.ini).
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::editor {

// Reads `key` out of `<engineRoot>/mcp.conf` as a TCP port (1-65535).
//
// Returns true and sets *outPort when the file exists, the key is present, and its value parses as
// an in-range port. Returns false and LEAVES *outPort UNCHANGED in every other case: a missing file,
// an empty file, and a missing key are the normal, silent, "the caller supplies its own default"
// case. A key that IS present but does not parse (out of range, not a number, trailing junk) is the
// one case that logs a warning -- naming the file, the key and what it fell back to -- because that
// one is a typo a developer would otherwise never see.
//
// `engineRoot` may be empty (no engine tree found beside the executable, e.g. a shipped build) --
// treated exactly like "no mcp.conf": returns false, no warning.
//
// Grammar: one `key = value` per line, deliberately more forgiving than EditorPrefs.cpp's editor.ini
// (leading/trailing whitespace around the key and the value is trimmed). editor.ini is written and
// read by the same program, so a hand-typed space is not a case that ever arises; mcp.conf is meant
// to be hand-edited from mcp.conf.example, where every line is aligned with spaces around `=`, so
// trimming here is what makes that file actually work rather than silently mis-parsing on first use.
// Otherwise identical: '#' and blank lines ignored, first occurrence of a duplicate key wins.
bool readMcpConfPort(const std::string& engineRoot, std::string_view key, u16* outPort);

} // namespace aver::editor
