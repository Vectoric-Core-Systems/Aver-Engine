#include "aver/scene/Fields.hpp"

#include "aver/scene/World.hpp"

namespace aver::scene {

// ComponentBuilder is sugar over two World entry points and holds no state of its own beyond which
// component it is describing. Keeping the work in World means the framework's script-declared
// components — which arrive one field at a time across a C ABI and never see this class — go through
// exactly the same code as a hand-written table.

ComponentBuilder& ComponentBuilder::field(const char* name, FieldKind kind, u16 offset, u8 arity, bool readOnly) {
    if (world_) world_->addField(typeId_, name, kind, offset, arity, readOnly);
    return *this;
}

bool ComponentBuilder::verify(usize structBytes) {
    return world_ && world_->verifyComponent(typeId_, structBytes);
}

} // namespace aver::scene
