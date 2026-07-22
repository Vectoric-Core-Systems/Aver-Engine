#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {

class World;

// Kinds are wider than any one consumer strictly needs, because growing them later is a v2 ABI
// decision rather than a patch, and a Details panel that cannot show a toggle or a colour is not a
// Details panel. The numeric values are pinned to the AVER_SCENE_KIND_* defines in scene_abi.h.
enum class FieldKind : u32 {
    F32 = 0, Vec3 = 1, Quat = 2, I32 = 3, Bool = 4, I64 = 5, Entity = 6, String = 7, Mat4 = 8
};

// Arity is floats-per-value for the float kinds and 0 for the rest, which is what the ABI's
// aver_scene_field_arity reports and what a generic vector accessor sizes its buffer from.
inline constexpr u8 canonicalArity(FieldKind k) {
    switch (k) {
        case FieldKind::F32:  return 1;
        case FieldKind::Vec3: return 3;
        case FieldKind::Quat: return 4;
        case FieldKind::Mat4: return 16;
        default:              return 0;
    }
}

// How many bytes of the component struct a field of this kind occupies. String is a slice into the
// world's name blob — the same {offset, len} pair CName carries — because a std::string inside a
// component would defeat the memcpy that snapshot/restore is.
inline constexpr usize fieldByteSize(FieldKind k) {
    switch (k) {
        case FieldKind::F32:
        case FieldKind::Vec3:
        case FieldKind::Quat:
        case FieldKind::Mat4:   return 4u * canonicalArity(k);
        case FieldKind::I32:
        case FieldKind::Bool:
        case FieldKind::Entity: return 4;
        case FieldKind::I64:    return 8;
        case FieldKind::String: return 8;
    }
    return 0;
}

struct FieldDesc {
    const char* name      = "";   // unqualified, as declared
    const char* qualified = "";   // "CLocal.position"
    u32         component = 0;
    FieldKind   kind      = FieldKind::F32;
    u16         offset    = 0;    // byte offset into the component struct
    u8          arity     = 0;
};

// Registration is hand-written next to the struct with offsetof — no codegen, so there is nothing to
// keep in sync at build time. ONE table then serves the generic get/set ABI, scene save/load and the
// editor's Details panel, so there is no second place to update and therefore no second place to
// forget. The .verify(sizeof(T)) terminator catches drift where it happens rather than three layers
// away.
class AVER_SCENE_API ComponentBuilder {
public:
    ComponentBuilder() = default;
    ComponentBuilder(World* world, u32 typeId) : world_(world), typeId_(typeId) {}

    // `arity` is ignored for the non-float kinds; passing it is allowed so a registration reads the
    // same whatever the kind is.
    ComponentBuilder& field(const char* name, FieldKind kind, u16 offset, u8 arity = 0);

    // False when the table does not account for the struct, having already logged which component
    // and which byte range is unexplained. It returns rather than asserting because the failure has
    // to be observable from a test in the same process — an abort proves nothing to a caller.
    //
    // WHAT IT CANNOT CATCH, by construction: this is a byte-COVERAGE check — offsets sorted, no gap,
    // no overlap, sizes summing to the struct — and the offsets ARE the registration, so it has no
    // independent ground truth for which name belongs at which offset. Two same-type members listed
    // in the wrong order, or a field given a wrong kind of the same width (I32/F32/Bool/Entity all
    // 4 bytes; I64/String both 8), leave coverage intact and pass. The eight built-ins are immune
    // because they pass offsetof(T, member) directly, so an offset cannot drift from its name; the
    // exposure is a SCRIPT-declared component whose fields arrive over the ABI, where a swapped or
    // mis-kinded same-width field is published silently. Closing that needs a name↔offset oracle a
    // coverage check does not have, so it is a documented limit rather than a bug to fix here.
    bool verify(usize structBytes);

    u32 typeId() const { return typeId_; }

private:
    World* world_  = nullptr;
    u32    typeId_ = 0;
};

} // namespace aver::scene
