#pragma once
// .ocparticle — the PARTICLE EFFECT format (particles DECIDED 3). Text, OC dialect, sized and
// shaped like .ocmat: one dedicated record per axis of aver::particles::ParticleEffect
// (modules/particles/include/aver/particles/ParticleTypes.hpp), no generic PARAM name=value bag --
// unlike .ocmat, which has PBR's larger and still-growing parameter set to cover, every field
// ParticleEffect has IS the small orthogonal parameter space that struct's own comment documents,
// so there is nothing left over that would need a generic escape hatch.
//
// Parses straight into particles::ParticleEffect, the SAME struct ParticleSystem/ParticleRenderer
// consume — this module carries no duplicate of it, matching OcMat.hpp parsing straight into
// pbr::MaterialDesc rather than an intermediate struct of its own.
//
// NO COUPLING: modules/particles never includes this header and never opens a file (see
// modules/particles/README.md's own statement of that rule). A composition root
// (sandbox/src/SandboxApp.cpp, modules/runtime.game/src/GameApp.cpp) calls loadOcparticle(), then
// hands the result to particles::particleEffects().set(id, effect); the particle module only ever
// sees the resolved id, never a path. This header is therefore its own target
// (Aver.Formats.Particles, see this directory's CMakeLists.txt) rather than living inside
// Aver.Formats itself, for OcMat.hpp's exact reason: it is the only format here that needs a type
// from outside Aver.Formats (particles::ParticleEffect), so linking it would put a particle-module
// dependency on every consumer of every OTHER format in the tree.
//
// ROUND-TRIP CONTRACT — THE .ocgraph PRECEDENT, NOT THE .ocmat ONE. A load/save cycle must not
// reorder or drop anything the reader did not understand (an unrecognised record, a comment, a
// blank line) — so writeOcparticle takes the file's own previous text as `existing` and merges into
// it the way OcGraph.cpp's writeOcgraph does: each record KIND this format models is replaced in
// place at its own first occurrence, and everything else is copied through untouched, at its
// original position. This is deliberately NOT what OcMat.cpp does — writeOcmat drops a source
// file's GRAPH{} block outright and warns about it, which is a real loss of a well-understood
// record kind and would be the wrong contract for a file authors are expected to hand-edit and
// keep re-saving from a tool. See OcGraph.cpp's own comment on why a whole-block replace (rather
// than a per-kind, in-place one) silently relocates every blank line that used to separate two
// record groups — the merge below follows that same in-place strategy for exactly that reason.
//
// MALFORMED INPUT IS STRICTER THAN THE OTHER TEXT READERS HERE, ON PURPOSE. OcMat/OcGraph's numeric
// helpers (aver::fmt::detail::parseF64 etc.) fall back to a default on a bad token, which is the
// right call for those formats' long history of hand-edited, occasionally-sloppy content. This
// format has no such history and the brief for it is explicit: "a missing, truncated, empty or
// malformed file is an error the loader REPORTS, never a crash and never a silent default effect
// that looks like it worked." So every numeric field here is parsed STRICTLY (the whole token must
// be a valid number, or the record is rejected) — see OcParticle.cpp's strictF32/strictU32/strictU64.
#include "aver/particles/ParticleTypes.hpp"

#include <string>
#include <string_view>

namespace aver::fmt {

// What the file said that particles::ParticleEffect has no field for — currently just the human
// authoring label. Kept for round-tripping, matching OcMatExtras's own reason to exist: nothing
// here reaches simulation or rendering, it is purely for a human or an editor panel.
struct OcParticleExtras {
    std::string name;   // NAME <text to end of line>; optional. Empty = the file carried none.
};

// Parses an effect from memory. Returns false and sets *err on a missing/wrong-version header, a
// record with the wrong field count, an unrecognised enum word (SHAPE kind, BLEND mode, COLOR
// start|end), or a malformed number — never a default effect that looks like it worked. Record
// KINDS this format does not recognise at all are forward-compat: skipped during parse, and
// preserved verbatim by writeOcparticle.
bool parseOcparticle(std::string_view text, particles::ParticleEffect& out,
                      OcParticleExtras* extras = nullptr, std::string* err = nullptr);

// Loads an effect from disk. False with *err set when the file is missing, unreadable, empty,
// truncated or malformed — see parseOcparticle.
bool loadOcparticle(const std::string& path, particles::ParticleEffect& out,
                     OcParticleExtras* extras = nullptr, std::string* err = nullptr);

// Serialises an effect to the text form. Pass the file's own previous text as `existing` to merge
// (preserving comments and anything unrecognised at its original position); pass an empty
// string_view (the default) to produce a fresh file. Round-trips through parseOcparticle.
std::string writeOcparticle(const particles::ParticleEffect& e, const OcParticleExtras* extras = nullptr,
                             std::string_view existing = "");

// Writes an effect to disk, creating parent directories, merging into whatever is already at
// `path` (see writeOcparticle — this reads it first, the way saveOcgraph does). Returns false with
// *err set on a write failure.
bool saveOcparticle(const std::string& path, const particles::ParticleEffect& e,
                     const OcParticleExtras* extras = nullptr, std::string* err = nullptr);

} // namespace aver::fmt
