#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

// Shader source, loaded from files rather than carried in C++ raw string literals.
//
// WHY THIS EXISTS. Every line of this engine's HLSL used to live inside R"( )" in a .cpp, which
// costs three things that only look small until you are in the middle of them:
//   - No hot reload. A one-character shader edit is a C++ recompile plus a relink of the render
//     stack plus a relaunch, because the text is a string literal in a translation unit.
//   - A real, recorded bug class. VoxiShaders.hpp's own comment describes a `)"` inside a PROSE
//     COMMENT terminating the raw string mid-file and producing "forty lines of C++ syntax errors
//     about HLSL identifiers". Nothing about the arrangement prevents that; you just have to
//     remember not to put a bracket next to a quote for two and a half thousand lines.
//   - No tooling. No syntax highlighting, no shader linter, no editor that knows what it is
//     looking at.
// docs/ARCHITECTURE.md records that an offline shader tool was planned and never built and that
// HLSL-in-C++ is the fallback -- so this is not undoing a design, it is finishing an unfinished one.
//
// WHERE THE FILES COME FROM, and why the default is the boring one. shaderFile() reads from
// `<executableDir>/shaders/` and nowhere else unless a human says otherwise. That default is not
// laziness: aver::platform's file trace exists so a packaged game can PROVE it never read the tree
// that built it (see FileSystem.hpp's own note, and scripts/verify-game.ps1, which asserts every
// traced path is under the package root). A loader that quietly preferred a source directory would
// run perfectly on the machine that built it and fail everywhere else -- the exact failure that
// trace was added to catch. So the source directory is opt-in, by flag, per run.
//
// LINE ENDINGS ARE NORMALISED TO \n ON LOAD. This repository checks out CRLF on Windows (git's
// autocrlf), and the literals these files replace were LF. Without normalising, "did the extracted
// file reproduce the literal byte for byte" -- the check that made this refactor safe to do at all
// -- would fail for every file on a fresh clone, for a reason that has nothing to do with the
// shader. DXC does not care either way.
namespace aver::rhi {

// Returns the text of `name` (e.g. "shared_prelude.hlsl"), cached after the first read.
//
// Returns an EMPTY string if the file cannot be read, having logged an error naming the path it
// tried. Empty is deliberately not fatal here: the caller concatenates preludes and hands the
// result to DXC, which will fail with its own diagnostic pointing at the missing declarations, and
// two errors describing one cause is better than an abort that describes none.
const std::string& shaderFile(std::string_view name);

// The same lookup, silent when the file is absent. For a caller that EXPECTS misses: the DXC include
// handler resolves an include by trying path suffixes longest-first, so all but the last attempt are
// expected to fail and shaderFile()'s (deliberately loud) error would be noise on every include.
// Returns nullptr rather than an empty string so "absent" and "empty file" stay distinguishable.
const std::string* shaderFileIfPresent(std::string_view name);

// A hash of EVERY shader file that could take part in a compile, for a cache key to fold in.
//
// WHY A BLOB CACHE NEEDS THIS. The DXIL cache keys on the source text handed to DXC plus the defines
// -- which was the whole input back when shader composition was C++ string concatenation and that
// text really did contain every prelude. It is not any more: a shader pulls in what it needs, and an
// edit to an included or concatenated PRELUDE leaves the top-level text byte-identical. The key does
// not move, the cache hits, and DXC is never asked -- so the edit is silently ignored, for every
// shader, until something else happens to change a key.
//
// That is not a stale-cache annoyance, it is a correctness hole: shared_prelude.hlsl holds the sky,
// the fog, the tonemap and the frame constant buffer, so editing any of them appeared to do nothing.
// It cost a real debugging session before it was found.
//
// COARSE ON PURPOSE. Any shader edit invalidates every entry rather than just the dependents, which
// costs one full recompile after a change to any shader. Tracking real dependencies would mean
// knowing a file's includes BEFORE compiling it to look it up -- the chicken-and-egg the include
// handler cannot answer -- and a shader edit is a developer action measured in seconds either way.
// Computed once and memoised; reloadShaderFiles() drops it along with the texts.
u64 shaderCorpusHash();

// Points shaderFile() at a directory to read from BEFORE `<executableDir>/shaders/`.
//
// For iterating on shaders without rebuilding: aim it at the source tree and the next reload picks
// up an edit. Off unless set, so a shipped build reads only what shipped with it. Call before the
// first shaderFile() for a given name, or follow it with reloadShaderFiles().
void setShaderSourceDir(std::string_view dir);

// The directory set above, or empty. Used to decide whether to watch anything.
const std::string& shaderSourceDir();

// Drops the cache so the next shaderFile() re-reads from disk. Returns the number of entries
// dropped. Does not itself recompile anything -- the caller owns the pipelines and decides when
// rebuilding them is safe (never mid-frame; see D3D12Device::setRenderScale's own comment for what
// happens when a resource is rebuilt while a command list is recording).
usize reloadShaderFiles();

// Bumps every time reloadShaderFiles() actually drops something, so a renderer can poll one integer
// to decide whether its pipelines are stale -- the same shape VoxiRenderer already uses to notice a
// material-graph change (pbr::materialGraphs().revision()).
u64 shaderFileRevision();

}   // namespace aver::rhi
