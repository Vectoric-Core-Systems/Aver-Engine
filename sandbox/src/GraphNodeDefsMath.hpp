#pragma once
// Palette entries for the math, integer, random, vector and rotator nodes. NOT standalone: it is included by the generated GraphNodeFamilies.gen.inc inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphMathNodeTable.cs pin for pin (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2).
// Pure rows work under either compiler; the random rows take part in the PUSH compiler only.

inline void appendMathNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_Tan", "Tan", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Asin", "Asin", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Acos", "Acos", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Atan", "Atan", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Atan2", "Atan2", "Math", {
        pin("y", "float", false), pin("x", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Sign", "Sign", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Frac", "Frac", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Truncate", "Truncate", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_DegToRad", "Deg To Rad", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_RadToDeg", "Rad To Deg", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Exp", "Exp", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_Log", "Log", "Math", {
        pin("a", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_SmoothStep", "Smooth Step", "Math", {
        pin("edge0", "float", false), pin("edge1", "float", false), pin("x", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_InverseLerp", "Inverse Lerp", "Math", {
        pin("a", "float", false), pin("b", "float", false), pin("value", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_MapRange", "Map Range", "Math", {
        pin("value", "float", false), pin("inMin", "float", false, "0"), pin("inMax", "float", false, "1"), pin("outMin", "float", false, "0"), pin("outMax", "float", false, "1"), pin("clamp", "bool", false, "true"),
        pin("result", "float", true)}});
    t.push_back({"AN_Wrap", "Wrap", "Math", {
        pin("value", "float", false), pin("min", "float", false), pin("max", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_InterpTo", "Interp To", "Math", {
        pin("current", "float", false), pin("target", "float", false), pin("deltaTime", "float", false), pin("speed", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_DeltaAngle", "Delta Angle", "Math", {
        pin("a", "float", false), pin("b", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_LerpAngle", "Lerp Angle", "Math", {
        pin("a", "float", false), pin("b", "float", false), pin("t", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_NearlyEqual", "Nearly Equal", "Math", {
        pin("a", "float", false), pin("b", "float", false), pin("tolerance", "float", false, "0.0001"), pin("result", "bool", true)}});
    t.push_back({"AN_IsFinite", "Is Finite", "Math", {
        pin("a", "float", false), pin("result", "bool", true)}});
    t.push_back({"AN_IntAdd", "Int Add", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntSubtract", "Int Subtract", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntMultiply", "Int Multiply", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntMin", "Int Min", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntMax", "Int Max", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntDivide", "Int Divide", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntModulo", "Int Modulo", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntClamp", "Int Clamp", "Integer", {
        pin("a", "int", false), pin("min", "int", false), pin("max", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntAbs", "Int Abs", "Integer", {
        pin("a", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntSign", "Int Sign", "Integer", {
        pin("a", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_IntCompare", "Int Compare", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("less", "bool", true), pin("equal", "bool", true), pin("greater", "bool", true)}});
    t.push_back({"AN_BitAnd", "Bit And", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_BitOr", "Bit Or", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_BitXor", "Bit Xor", "Integer", {
        pin("a", "int", false), pin("b", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_BitShiftLeft", "Bit Shift Left", "Integer", {
        pin("a", "int", false), pin("count", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_BitShiftRight", "Bit Shift Right", "Integer", {
        pin("a", "int", false), pin("count", "int", false), pin("result", "int", true)}});
    t.push_back({"AN_BitTest", "Bit Test", "Integer", {
        pin("value", "int", false), pin("bit", "int", false), pin("result", "bool", true)}});
    t.push_back({"AN_RandomFloat", "Random Float", "Random", {
        pin("exec", "exec", false), pin("min", "float", false, "0"), pin("max", "float", false, "1"), pin("then", "exec", true), pin("value", "float", true)}});
    t.push_back({"AN_RandomInt", "Random Int", "Random", {
        pin("exec", "exec", false), pin("min", "int", false, "0"), pin("max", "int", false, "1"), pin("then", "exec", true), pin("value", "int", true)}});
    t.push_back({"AN_RandomBool", "Random Bool", "Random", {
        pin("exec", "exec", false), pin("chance", "float", false, "0.5"), pin("then", "exec", true), pin("value", "bool", true)}});
    t.push_back({"AN_RandomUnitVector", "Random Unit Vector", "Random", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_SetRandomSeed", "Set Random Seed", "Random", {
        pin("exec", "exec", false), pin("seed", "int", false), pin("then", "exec", true)}});
    t.push_back({"AN_VecNegate", "Vec Negate", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_VecMultiply", "Vec Multiply", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_VecLength2D", "Vec Length 2D", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_VecDistance2D", "Vec Distance 2D", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("result", "float", true)}});
    t.push_back({"AN_VecReflect", "Vec Reflect", "Vector", {
        pin("vx", "float", false), pin("vy", "float", false), pin("vz", "float", false), pin("nx", "float", false), pin("ny", "float", false), pin("nz", "float", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_VecProject", "Vec Project", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_VecClampLength", "Vec Clamp Length", "Vector", {
        pin("vx", "float", false), pin("vy", "float", false), pin("vz", "float", false), pin("maxLength", "float", false), pin("x", "float", true), pin("y", "float", true),
        pin("z", "float", true)}});
    t.push_back({"AN_VecAngle", "Vec Angle", "Vector", {
        pin("ax", "float", false), pin("ay", "float", false), pin("az", "float", false), pin("bx", "float", false), pin("by", "float", false), pin("bz", "float", false),
        pin("result", "float", true)}});
    t.push_back({"AN_RotatorToForward", "Rotator To Forward", "Rotator", {
        pin("yaw", "float", false), pin("pitch", "float", false), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_ForwardToRotator", "Forward To Rotator", "Rotator", {
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("yaw", "float", true), pin("pitch", "float", true)}});
    t.push_back({"AN_RotateVector", "Rotate Vector", "Rotator", {
        pin("vx", "float", false), pin("vy", "float", false), pin("vz", "float", false), pin("yaw", "float", false), pin("pitch", "float", false), pin("roll", "float", false),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true)}});
    t.push_back({"AN_LookAtRotation", "Look At Rotation", "Rotator", {
        pin("fromX", "float", false), pin("fromY", "float", false), pin("fromZ", "float", false), pin("toX", "float", false), pin("toY", "float", false), pin("toZ", "float", false),
        pin("yaw", "float", true), pin("pitch", "float", true)}});
}
