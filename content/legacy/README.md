# content/legacy/

**Never populated.** This directory has held nothing but this README since the commit that created
both (`ab2264a`, "Aver Engine foundation" — the day-one commit, before any module existed): no
`demoworld.scene`/`.ocmap`, no `.ocbeam` cage, no `.ocaero` table, no `Ferrari499P.ocbeam` reference —
`content/` has no `.ocbeam` or `.ocaero` file anywhere in the tree today, and no manifest here points
at an external "source project" either.

What actually happened instead: the format loaders' golden fixtures — `demoworld`, `demoworld.ocmap`
included — are inlined as string literals directly inside `tests/formats/src/FormatTest.cpp` (see its
`NAME demoworld` fixtures and the `fnv1a64("demoworld")` checks), not loaded from files here. Sample
content that does exist in the tree lives under `test-content/` (`AN_Playable`, `GraphDemo`,
`MaterialGraph`, `NavRange`, `ParticleDemo`), which is unrelated to the OpenConstructor vehicle formats
this directory was meant to hold.
