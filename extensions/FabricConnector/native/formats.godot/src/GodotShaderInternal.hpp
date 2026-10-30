#pragma once
// Private to the shading-language front-end: the built-in function overload table and the expression typing rules shared by the parser and the emitter.
#include "aver/formats/godot/GodotShader.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot::detail {

struct BuiltinOverload {
    std::string name;
    GdType ret;
    std::vector<GdBase> params;
    std::vector<bool> outParam;
    std::string hlsl;      // HLSL function name; "" = same name, "!" = no HLSL spelling, "@x" = emitter special
};

const std::vector<BuiltinOverload>& builtinOverloads();
// Overload ids with this name (empty when none).
const std::vector<int>& builtinOverloadIds(std::string_view name);

// Result type of a binary operator applied to (a, b); base Error when the operands are invalid. `op` is the source spelling ("+", "<<", "==", ...).
GdType binaryResultType(std::string_view op, const GdType& a, const GdType& b);
// Result of a unary operator, Error when invalid.
GdType unaryResultType(std::string_view op, const GdType& a);
// Swizzle: "xyzw" / "rgba" / "stpq" letters -> normalised xyzw; empty when invalid for a vector of `dim` components. `*sameSet` false = mixed sets.
std::string normaliseSwizzle(std::string_view sw, int dim);

// Folds an expression to a constant; false when it is not one (built-in calls other than min / max / abs ... are not folded).
bool foldConst(const GdExpr& e, const GdShaderModule& m, GdConstValue& out);

}  // namespace aver::fmt::godot::detail
