// ComponentBuilder's two methods. Both forward straight to World.
#include "aver/scene/Fields.hpp"

#include "aver/scene/World.hpp"

namespace aver::scene {

// Declares one field of the component. Returns *this for chaining.
ComponentBuilder& ComponentBuilder::field(const char* name, FieldKind kind, u16 offset, u8 arity, bool readOnly) {
    if (world_) world_->addField(typeId_, name, kind, offset, arity, readOnly);
    return *this;
}

// Checks the field table accounts for every byte of the struct.
bool ComponentBuilder::verify(usize structBytes) {
    return world_ && world_->verifyComponent(typeId_, structBytes);
}

} // namespace aver::scene
