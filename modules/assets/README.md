# Aver.Assets  (`modules/assets`)

- **Language:** C++
- **Depends on:** Core, Platform
- **Status:** implemented (Phase 2 — foundational layer)

Object identity for every asset: `ObjectId` (u64), `makeObjectId(name)` (FNV-1a-64),
and `AssetType` / `assetTypeFromPath()`. This is the "unique id per object" backbone the
`.oc*` formats resolve against. The full asset registry and streaming layer still lands
alongside the renderer in later phases. The AVR1 container reader this line used to bundle
with them has already landed, but not here: it is `modules/formats/{include,src}/aver/formats/
Avr1.{hpp,cpp}` in Aver.Formats, and every binary `.oc*` format (`OcMesh`, `OcAnim`, `OcAudio`,
`OcBt`, `OcLand`, `OcNav`, `OcSave`, `OcSound`, plus `GiCache`) is built on it.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) and [docs/formats/DECISIONS.md](../../docs/formats/DECISIONS.md).
