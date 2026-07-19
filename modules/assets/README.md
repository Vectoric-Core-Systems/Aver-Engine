# Aver.Assets  (`modules/assets`)

- **Language:** C++
- **Depends on:** Core, Platform
- **Status:** implemented (Phase 2 — foundational layer)

Object identity for every asset: `ObjectId` (u64), `makeObjectId(name)` (FNV-1a-64),
and `AssetType` / `assetTypeFromPath()`. This is the "unique id per object" backbone the
`.oc*` formats resolve against. The full asset registry / streaming / AVR1 container
reader lands alongside the renderer in later phases.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) and [docs/formats/DECISIONS.md](../../docs/formats/DECISIONS.md).
