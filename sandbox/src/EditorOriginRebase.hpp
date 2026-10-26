#pragma once
// What a floating-origin shift does to the positions the editor remembers between frames (LARGE_WORLD 2.7): the undo and
// redo stacks, the clipboard, the Play-start snapshot. Every one of them was recorded in the old scene frame, so every
// origin-relative position in them changes by -delta, with the same f32 subtraction World::shiftRoots applies to the scene.
//
// Header-only and Core-only: the functions are templates over the editor's own types (SandboxApp::EditCmd and friends are
// nested in SandboxApp, which no test can include), so EditorOriginTest drives them with stand-ins that carry the same
// member names. A rename in the editor breaks the instantiation in SandboxEditorOrigin.cpp, not silently.
//
// The frame of each stored position decides whether it moves:
//   - WORLD positions (a Transform command's before and after: selectedXform composes up the parent chain) always move: a
//     child follows its root, so its world position changes by the same delta.
//   - LOCAL positions move only when the entity is a root (a child's local is parent-relative and never changes).
//   - Grid indices (landscape and foliage strokes), snapshots without a transform (EntitySnapshot omits CLocal) and absolute
//     level records (water bodies, LEVELINST) hold no origin-relative position and are left alone.
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE

namespace aver::editor::originRebase {

inline void shiftPos(Vec3& p, const Vec3& deltaF) {
    p.x -= deltaF.x;
    p.y -= deltaF.y;
    p.z -= deltaF.z;
}

// prefab::PrefabState: an instance or a plain subtree remembers the local transform it is recreated with, and the entity it
// hangs from. A root (no parent) moves.
template <class State, class Entity>
void rebasePrefabState(State& s, const Vec3& deltaF, Entity invalid) {
    for (auto& i : s.instances) if (i.parent == invalid) shiftPos(i.local.position, deltaF);
    for (auto& p : s.plains)    if (p.parent == invalid) shiftPos(p.local.position, deltaF);
}

// One EditCmd (any kind). Kind names are the editor's: Transform, Create, Destroy, Reparent, CreateObj, DestroyObj, Prefab.
template <class Cmd, class Entity>
void rebaseCmd(Cmd& c, const Vec3& deltaF, Entity invalid) {
    using K = typename Cmd::Kind;
    switch (c.kind) {
        case K::Transform:
            // before and after are world positions; the rest of a multi-selection drag is stored local, and moves only when it is a root.
            shiftPos(c.before.pos, deltaF);
            shiftPos(c.after.pos, deltaF);
            for (auto& m : c.alsoMoved)
                if (m.root) { shiftPos(m.beforeLocal.position, deltaF); shiftPos(m.afterLocal.position, deltaF); }
            break;
        case K::Create:
        case K::Destroy:
            // `after` is the entity's local position when it was captured (describeEntity); a subtree is parent-relative.
            if (c.parentId == 0) shiftPos(c.after.pos, deltaF);
            break;
        case K::Reparent:
            // Both ends are local transforms: a root on that end is the frame the scene shifts.
            if (c.reparentOldParentId == 0) shiftPos(c.before.pos, deltaF);
            if (c.reparentNewParentId == 0) shiftPos(c.after.pos, deltaF);
            break;
        case K::CreateObj:
        case K::DestroyObj:
            shiftPos(c.objSnapshot.pos, deltaF);
            break;
        case K::Prefab:
            if (c.prefab) {
                rebasePrefabState(c.prefab->before, deltaF, invalid);
                rebasePrefabState(c.prefab->after, deltaF, invalid);
            }
            break;
        default:
            break;   // no origin-relative position: visibility, material, rename, strokes, water records, component edits, level instances
    }
}

// Both stacks. A shared payload (the prefab edit is a shared_ptr) is rebased once per command that holds it; two commands never
// share one (pushEdit stores a fresh make_shared each time).
template <class Stack, class Entity>
void rebaseStacks(Stack& undoStack, Stack& redoStack, const Vec3& deltaF, Entity invalid) {
    for (auto& c : undoStack) rebaseCmd(c, deltaF, invalid);
    for (auto& c : redoStack) rebaseCmd(c, deltaF, invalid);
}

// The clipboard: a copy remembers where the entity was. Paste keeps only the layout relative to the first entry, so a uniform
// shift changes nothing it does; shifting all of them keeps the stored values honest all the same.
template <class Clipboard>
void rebaseClipboard(Clipboard& cb, const Vec3& deltaF) {
    for (auto& e : cb.entities) shiftPos(e.xform.pos, deltaF);
    if (cb.hasObject) shiftPos(cb.object.pos, deltaF);
}

// The Play-start snapshot (SandboxApp::PlaySavedTransform { e, xf, visible }): `xf` is the entity's LOCAL transform, so only a
// root moves. `isRoot(e)` answers for the live entity; an entity that is gone is not restored on Stop and is left as it is.
template <class Snapshot, class IsRoot>
void rebasePlaySnapshot(Snapshot& snap, const Vec3& deltaF, IsRoot isRoot) {
    for (auto& s : snap) if (isRoot(s.e)) shiftPos(s.xf.position, deltaF);
}

} // namespace aver::editor::originRebase

#endif // AVER_MODULE_SCENE
