// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The #define groups of physics_shape_builder_abi.h and physics_surface_abi.h as enums. Values are copied from the header and pinned by the
// ABI parity test (tests/abi/src/AbiEnumTest.cpp): the index blocks name the slots of the float and int arrays the
// shape calls take, each with a Count member for its _COUNT.

namespace Aver.Physics;

/// <summary>What a shape is, as <see cref="ShapeInfo.Type"/> reports it (AVER_PHYS_SHAPE_TYPE_*).</summary>
public enum ShapeType
{
    Unknown = 0,
    Box = 1,
    Sphere = 2,
    Capsule = 3,
    Cylinder = 4,
    TaperedCapsule = 5,
    TaperedCylinder = 6,
    ConvexHull = 7,
    Triangle = 8,
    Plane = 9,
    Empty = 10,
    Mesh = 11,
    Heightfield = 12,
    StaticCompound = 13,
    MutableCompound = 14,
    RotatedTranslated = 15,
    Scaled = 16,
    OffsetCom = 17,
    SoftBody = 18,
}

/// <summary>Whether a compound's children can change after it is built (AVER_PHYS_COMPOUND_*).</summary>
public enum CompoundKind
{
    Static = 0,
    Mutable = 1,
}

// The parameter and info blocks, as enums, so the parity test pins every number and the code below names them.
internal enum SpIndex { ConvexRadius = 0, Density = 1, HullTolerance = 2, EdgeAngleDeg = 3, BuildQuality = 4, MaxTrisPerLeaf = 5, Count = 6 }
internal enum SiIndex
{
    VolumeCm3 = 0, MassKg = 1, ComX = 2, ComY = 3, ComZ = 4, BoundsMinX = 5, BoundsMinY = 6, BoundsMinZ = 7,
    BoundsMaxX = 8, BoundsMaxY = 9, BoundsMaxZ = 10, InnerRadius = 11, InertiaPrincipalX = 12, InertiaPrincipalY = 13,
    InertiaPrincipalZ = 14, InertiaFrameX = 15, InertiaFrameY = 16, InertiaFrameZ = 17, InertiaFrameW = 18, Count = 19,
}
internal enum SiiIndex { Type = 0, Children = 1, Leaves = 2, Triangles = 3, Bytes = 4, MustBeStatic = 5, HasMass = 6, Count = 7 }
internal enum BpIndex { Layer = 0, Sensor = 1, InertiaFromBounds = 2, EnhancedEdges = 3, Activate = 4, Count = 5 }

// The detail blocks of physics_surface_abi.h (raycast_detail, subshape_info, character ground, wheel surface, contact_detail).
internal enum HitFloat
{
    PointX = 0, PointY = 1, PointZ = 2, NormalX = 3, NormalY = 4, NormalZ = 5, DistanceCm = 6, Friction = 7, Restitution = 8, Count = 9,
}
internal enum HitInt
{
    Body = 0, Entity = 1, SubShape = 2, Surface = 3, Tag = 4, Child = 5, ChildUserData = 6, Triangle = 7, CellX = 8, CellY = 9, Count = 10,
}
internal enum ContactFloat { ClosingSpeedCms = 0, Friction = 1, Restitution = 2, Count = 3 }
internal enum ContactInt { SubShapeA = 0, SubShapeB = 1, SurfaceA = 2, SurfaceB = 3, Count = 4 }

// The heightfield blocks of physics_terrain_abi.h (heightfield_shape's parameter block, heightfield_info, the edit flags).
internal enum HpIndex { BlockSize = 0, BitsPerSample = 1, EdgeAngleDeg = 2, EditHeadroomCm = 3, Count = 4 }
internal enum TerrainFloat { SpacingCm = 0, MinHeightCm = 1, MaxHeightCm = 2, ClampedTotal = 3, Count = 4 }
internal enum TerrainInt { Cols = 0, Rows = 1, Padded = 2, BlockSize = 3, Bits = 4, Materials = 5, Count = 6 }
internal enum HeightfieldFlags { Wake = 1 }
