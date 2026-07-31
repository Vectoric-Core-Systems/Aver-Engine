// Field kinds, the field descriptor, and the builder that registers a component's field table.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {

class World;

// The type of one component field. Values are pinned to the AVER_SCENE_KIND_* defines in scene_abi.h.
enum class FieldKind : u32 {
    F32 = 0, Vec3 = 1, Quat = 2, I32 = 3, Bool = 4, I64 = 5, Entity = 6, String = 7, Mat4 = 8
};

// Floats per value for the float kinds, 0 for the rest.
inline constexpr u8 canonicalArity(FieldKind k) {
    switch (k) {
        case FieldKind::F32:  return 1;
        case FieldKind::Vec3: return 3;
        case FieldKind::Quat: return 4;
        case FieldKind::Mat4: return 16;
        default:              return 0;
    }
}

// How many bytes of the component struct a field of this kind occupies.
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

// One registered field: its names, the component it belongs to, its kind, and where it sits.
struct FieldDesc {
    const char* name      = "";   // unqualified, as declared
    const char* qualified = "";   // "CLocal.position"
    u32         component = 0;
    FieldKind   kind      = FieldKind::F32;
    u16         offset    = 0;    // byte offset into the component struct
    u8          arity     = 0;
    bool        readOnly  = false;   // readable over the generic ABI, but a generic set is rejected
};

// Registers one component's field table. Sugar over World::addField and World::verifyComponent.
class AVER_SCENE_API ComponentBuilder {
public:
    ComponentBuilder() = default;
    // Builds the table for component `typeId` in `world`.
    ComponentBuilder(World* world, u32 typeId) : world_(world), typeId_(typeId) {}

    // Declares one field. `arity` is ignored for the non-float kinds. Returns *this for chaining.
    ComponentBuilder& field(const char* name, FieldKind kind, u16 offset, u8 arity = 0, bool readOnly = false);

    // Checks the table covers the struct byte for byte. False on drift, having logged which bytes.
    bool verify(usize structBytes);

    u32 typeId() const { return typeId_; }

private:
    World* world_  = nullptr;
    u32    typeId_ = 0;
};

} // namespace aver::scene
