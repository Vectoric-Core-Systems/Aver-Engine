#pragma once
// Godot Variant text literals (the right-hand side of every `key = value` in .tscn/.tres/project.godot,
// and every attribute in a [tag ...] header): a small value tree, a parser and a canonical writer.
// Contract: docs/plans/fabric/G3_FORMATS.md.
//
// PAYLOAD BY TYPE (everything else is empty):
//   Bool b | Int i | Float f | Rid i
//   String, StringName, NodePath: s
//   Vector2/2i/3/3i/4/4i, Rect2/2i, Quaternion, Plane, AABB, Color, Basis, Transform2D, Transform3D,
//   Projection: nums, in the order the literal spells them (counts in godotVTypeNums). Basis is nine
//   numbers ROW by row as written (nums[r*3+c]); Transform3D is those nine then the origin; Transform2D
//   is xx,xy,yx,yy,ox,oy (the file's own order, i.e. columns x, y, origin); Projection is 16 numbers
//   as written (column by column). Raw numbers: no unit or axis conversion happens in this layer.
//   Array: items; typed Array[T]: typed = true, s = T ("Vector3", or ExtResource("id") for a script class)
//   Dictionary: keys + items (parallel); typed Dictionary[K, V]: typed = true, s = K, s2 = V
//   Object(Class, "prop": v, ...): s = Class, keys (String variants) + items
//   PackedByteArray: bytes | Packed{Int32,Int64,Float32,Float64}Array and Packed{Vector2,Vector3,Vector4,
//   Color}Array: nums (flat, stride 1/2/3/4/4) | PackedStringArray: strs
//   ExtResource, SubResource: s = id text ("1_abc", or "3" in format 2) | Resource("path"): s = path
//   Callable, Signal: items = constructor arguments (they have no text form Godot itself writes)
// Integers inside nums (Vector2i, Rect2i, PackedInt64Array) are doubles: exact to 2^53.
#include "aver/core/Types.hpp"
#include "aver/formats/godot/GodotDiagnostics.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt::godot {

enum class GodotVType : u8 {
    Nil, Bool, Int, Float, String, StringName, NodePath,
    Vector2, Vector2i, Vector3, Vector3i, Vector4, Vector4i,
    Rect2, Rect2i, Quaternion, Basis, Transform2D, Transform3D, Plane, AABB, Projection, Color,
    Array, Dictionary, Object,
    PackedByteArray, PackedInt32Array, PackedInt64Array, PackedFloat32Array, PackedFloat64Array,
    PackedStringArray, PackedVector2Array, PackedVector3Array, PackedVector4Array, PackedColorArray,
    ExtResource, SubResource, Resource, Rid, Callable, Signal,
};

// The Godot spelling ("Vector3", "PackedStringArray", ...).
const char* godotVTypeName(GodotVType t);
// How many numbers a fixed-size math type carries in nums (0 for every other type).
int godotVTypeNums(GodotVType t);

struct GodotVariant {
    GodotVType type = GodotVType::Nil;
    int line = 0;  // 1-based source line of the literal's first character (0 when synthesised)

    bool b = false;
    i64 i = 0;
    f64 f = 0;
    std::string s, s2;
    std::vector<f64> nums;
    std::vector<std::string> strs;
    std::vector<u8> bytes;
    std::vector<GodotVariant> items;
    std::vector<GodotVariant> keys;
    bool typed = false;

    bool isNil() const { return type == GodotVType::Nil; }
    bool isNumber() const { return type == GodotVType::Int || type == GodotVType::Float; }
    bool isStringLike() const {
        return type == GodotVType::String || type == GodotVType::StringName || type == GodotVType::NodePath;
    }
    // Int or Float as a double; `def` for anything else.
    f64 asNumber(f64 def = 0) const;
    i64 asInt(i64 def = 0) const;
    bool asBool(bool def = false) const;
    // The text of a String/StringName/NodePath (also an ExtResource/SubResource id); `def` otherwise.
    const std::string& asString(const std::string& def = emptyString()) const;
    // nums[idx], 0 when out of range.
    f64 num(usize idx) const { return idx < nums.size() ? nums[idx] : 0.0; }
    // Number of elements of a packed array / array / dictionary.
    usize count() const;
    // A Dictionary's or Object's value for a String key; nullptr if absent.
    const GodotVariant* find(std::string_view key) const;

    // Structural equality; line is ignored and NaN equals NaN.
    bool operator==(const GodotVariant& o) const;
    bool operator!=(const GodotVariant& o) const { return !(*this == o); }

    static const std::string& emptyString();
};

// Parses one complete literal. True when it parsed and nothing but whitespace/comments follows;
// errors go to `diag` (when non-null) as file:line, `firstLine` is the line `text` starts on.
bool parseGodotVariant(std::string_view text, GodotVariant& out, GodotDiagnostics* diag = nullptr,
                       std::string_view file = {}, int firstLine = 1);

// Canonical Godot text that parseGodotVariant reads back to an equal tree (shortest round-trip floats,
// strings escaped, PackedByteArray as base64). Locale: assumes the "C" locale for the decimal point.
std::string godotVariantToText(const GodotVariant& v);

// A cursor over text that parses literals in place, tracking the line number. The section/tag parser
// and the config parsers drive it; `parseGodotVariant` is the one-shot wrapper.
class GodotVariantReader {
public:
    GodotVariantReader(std::string_view text, std::string_view file, GodotDiagnostics* diag, int firstLine = 1);

    bool atEnd() const { return pos_ >= text_.size(); }
    // The character `ahead` positions on (0 = next), or '\0' past the end.
    char peek(usize ahead = 0) const { return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0'; }
    char advance();  // consumes one character, counting lines
    int line() const { return line_; }
    usize pos() const { return pos_; }
    std::string_view slice(usize from, usize to) const { return text_.substr(from, to - from); }
    std::string_view file() const { return file_; }

    // Spaces and tabs always; newlines when `newlines`; a ';' comment runs to the end of the line.
    void skipSpace(bool newlines = true);
    // Skips to the start of the next line.
    void skipLine();
    // Error recovery: advances to the start of the next line whose first non-blank character is '['.
    void skipToNextTag();

    bool consume(char c);  // consumes `c` if it is next (after no skipping)
    bool expect(char c, const char* what);

    // [A-Za-z0-9_]+ ; empty on failure.
    std::string readIdentifier();
    // A "..." string literal with Godot's escapes; the cursor must be on the opening quote.
    bool parseString(std::string& out);
    // One value of any type.
    bool parseValue(GodotVariant& out);

    void error(const std::string& message, std::string_view code = "godot.parse");
    void errorAt(int line, const std::string& message, std::string_view code = "godot.parse");
    void warn(const std::string& message);
    bool failed() const { return failed_; }
    void clearFailed() { failed_ = false; }

private:
    bool parseNumber(GodotVariant& out);
    bool parseIdentifierValue(GodotVariant& out);
    bool parseConstructor(const std::string& name, GodotVariant& out, int line);
    bool parseArray(GodotVariant& out);
    bool parseDictionary(GodotVariant& out);
    bool parseTypeArg(std::string& out);
    bool parseArgs(std::vector<GodotVariant>& args);

    std::string_view text_;
    std::string_view file_;
    GodotDiagnostics* diag_;
    usize pos_ = 0;
    int line_ = 1;
    int depth_ = 0;
    bool failed_ = false;
};

}  // namespace aver::fmt::godot
