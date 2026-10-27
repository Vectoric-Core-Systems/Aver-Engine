# Code notes: Aver.Framework

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## scripting/csharp/Aver.Framework/Hair.cs

- file header: CHair is registered at RUNTIME (like CCloth), so adding and configuring it is a field write over the generic scene ABI; a host that never registered it gets false, not a throw. The component goes on the entity that owns the skeleton the scalp is skinned to (the character root).
- file header: a groom is built by the hair system at its next tick. What HAPPENS (a teleport, a collider) goes through the framework relay (framework_hair_abi.h) and applies at once, in arrival order. Scales read 0 as 1 on the native side, so a zero-filled component is the default.
- HairHas: False, not a throw, in a process with no native scene (the bare-process graph tests, a tool).
- HairSetScale: a scale field reads 0 as "the default 1" on the native side; a negative or non-finite value is refused here.
- AddHair: the hair system makes a child entity "Hair" that carries the draw, and shares the build with every other entity using the same groom.
- SetHairLook: an empty string goes back to the groom's look.
- SetHairWidthScale: 1 is the groom's own width.
- SetHairStiffness: shared by every instance of the groom; the last entity to ask wins.
- SetHairWindScale: use SetHairWindEnabled for no wind at all.
- SetHairCollides: the layer is chosen when the hair is built, so this takes effect for a rebuilt groom.
- SetHairEnabled: hidden hair is neither drawn nor expanded; the simulation still follows the distance rules.
- TeleportHair: returns false while the groom is not built.
- AddHairCollider: size is in centimetres (see HairColliderShape), offset is in the bone's frame. At most 16 per character. The hull is made once the groom is built; a bone the skeleton does not have is logged and skipped then.
- Hair.SetWind: the same wind the soft bodies use (SoftBodies.SetWind), so cloth, ropes and hair move together. Centimetres per second; the gust is an amplitude in cm/s.
- HairGraphInterop: a hair crosses the graph as its entity id.
- SetHairParamsForGraph: a negative value leaves that scale alone, so a node can set just one.


## scripting/csharp/Aver.Framework/SoftShapes.cs

- file header: design docs are docs/plans/wave11/SOFTBODY_HAIR.md 2.4 and 2.6 (and docs/SOFTBODY.md, kept in the header).
- file header: what HAPPENS (a grab, a cut, buoyancy, pressure) goes through the framework relay (framework_softshape_abi.h) and applies at once, in arrival order.
- AddJelly: needs a mesh renderer; refused on an entity that is already a rope. The asset is an .ocsoft path of kind "jelly" (empty = the jelly preset). The mesh must be closed, or the jelly reports failed and the entity keeps its mesh.
- AddRope: lengthCm 0 = the distance between the points times the asset's slack; radiusCm 0 = the asset's. It draws through a generated tube (the entity is given a renderer if it has none).
- SetRopeEnd: clears the end's free flag.
- CutRope: this entity keeps the start; `other` is a NEW entity holding the rest; both cut ends are free.
- GrabSoftVertex: the grab id (vertex index + 1) is what MoveSoftGrab and ReleaseSoftGrab take; grabbing two vertices is two calls.
- SetSoftContact: a soft scale of 0 pins the body against anything it touches; an other scale of 0 lets a body land on it without being slowed.
- Ropes.Create: lengthCm 0 = the distance times the asset's slack.
- SoftShapes.DrainSensorEvents: pairs set to SoftContactMode.Sensor and Jolt sensor bodies raise these events; they also reach graphs as the named events SoftBodySensorEntered and SoftBodySensorExited at the soft body's entity.
- SoftBodyGraphInterop: forwarded from the node table (GraphSoftBodyNodeTable.cs).


## scripting/csharp/Aver.Framework/WorldPos.cs

- file header: the arithmetic is aver/world/WorldPos.hpp restated; a test pins the two against each other.
- Double3 summary: exact to 2^53 cm.
- WorldPos summary: 1.2 micrometres at any distance, where a bare float is 1 cm at 100 km.
- WorldPos remarks (dropped): the scene, physics and every other API speak origin-relative floats (Entity.WorldPosition); this is what a script stores when a position must outlive a floating-origin shift. Region 0 is centred on the world's zero; a region is 1024 chunks (16.384 km) on a side. Convert with WorldOrigin or the members here.
- WorldPos constructor: the offset is not normalised; use FromAbsolute for that.
- SplitAxis: the f64 division can land a value one chunk off, and narrowing a remainder just under the chunk size can round up to it. Both are repaired, exactly like world::detail::splitAxis.
- ToAbsolute: exact to 2^53 cm.
- ToLocal: one rounding.
- Double3.ToVec3: one rounding to single precision (kept in the summary).
- Inheritdoc lines on Equals, GetHashCode and ToString were removed (no content).
