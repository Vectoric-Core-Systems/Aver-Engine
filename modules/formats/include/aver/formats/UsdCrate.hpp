#pragma once
// USDC -- Pixar's binary "crate" layer encoding -- read natively, the way UsdImport.hpp reads USDA.
//
// ONE LAYER, AS DATA. Its specs (prims, attributes, relationships) keyed by path, each with its
// fields, and each field's value decoded ON DEMAND from the file bytes held in memory: a 500 MB
// terrain layer is mostly vertex data nobody asks for until the mesh that owns it is built. This does
// no composition -- sublayers, references, inherits and PointInstancers are UsdStageImport's -- and
// interprets no schema: "points" is only a field name here.
//
// WHY NATIVE, NOT OpenUSD: UsdImport.hpp's argument, unchanged. What the engine needs of the encoding
// is small: a table of contents, six structural sections (tokens, strings, fields, field sets, paths,
// specs), LZ4 blocks, a delta-plus-width-code integer packing, and one 64-bit ValueRep per value.
//
// VERSION 0.4.0 AND LATER, which is every crate written since 2017 -- Blender, Houdini and usdcat
// write 0.8-0.10. Earlier files store their structural sections uncompressed and are refused by name.
//
// NOT DECODED, and reported as such rather than guessed: dictionaries (customData, assetInfo),
// time-sample maps (field present, value refused -- a caller falls back to `default`), splines,
// variant selections and unregistered values. None of them carry geometry.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::fmt {

// A reference or payload arc as the crate stores it.
struct UsdCrateRef {
    std::string assetPath;   // as authored, e.g. "anthurium_classes.usda"; empty = this layer
    std::string primPath;    // empty = the target layer's defaultPrim
};

// Crate value type ids (crateDataTypes.h). The numbering is the file format, not an engine choice.
namespace UsdCrateType {
enum : u8 {
    Bool = 1, UChar, Int, UInt, Int64, UInt64, Half, Float, Double, String, Token, AssetPath,
    Matrix2d, Matrix3d, Matrix4d, Quatd, Quatf, Quath,
    Vec2d, Vec2f, Vec2h, Vec2i, Vec3d, Vec3f, Vec3h, Vec3i, Vec4d, Vec4f, Vec4h, Vec4i,
    Dictionary, TokenListOp, StringListOp, PathListOp, ReferenceListOp, IntListOp, Int64ListOp,
    UIntListOp, UInt64ListOp, PathVector, TokenVector, Specifier, Permission, Variability,
    VariantSelectionMap, TimeSamples, Payload, DoubleVector, LayerOffsetVector, StringVector,
    ValueBlock, Value, UnregisteredValue, UnregisteredValueListOp, PayloadListOp, TimeCode,
    PathExpression, Relocates, Spline,
};
}

// SdfSpecType, as the crate stores it.
enum class UsdSpecType : u8 {
    Unknown = 0, Attribute, Connection, Expression, Mapper, MapperArg, Prim, PseudoRoot,
    Relationship, RelationshipTarget, Variant, VariantSet,
};

// One decoded value. Only the vector the type uses is filled; `components` says how many numbers
// make one element (3 for a Vec3f, 4 for a quaternion stored imaginary-first, 16 for a Matrix4d).
struct UsdCrateValue {
    u8   type = 0;
    bool isArray = false;
    u32  components = 1;
    std::vector<f32>         f;      // Half/Float and every f/h vector and quaternion
    std::vector<f64>         d;      // Double, TimeCode, every d vector, matrix and quaternion, DoubleVector
    std::vector<i64>         i;      // Bool, UChar, Int(64), UInt(64), i vectors, Specifier, Variability, Permission
    std::vector<std::string> s;      // Token, String, AssetPath (and arrays), paths, list-op items
    std::vector<UsdCrateRef> refs;   // references and payloads

    // The numbers, whichever of f/d/i holds them.
    std::vector<f32> asFloats() const;
    std::vector<i32> asInts() const;
    usize numberCount() const { return f.size() + d.size() + i.size(); }
    f64 number(usize k = 0, f64 fallback = 0.0) const;
    const std::string* str(usize k = 0) const { return k < s.size() ? &s[k] : nullptr; }
};

class UsdCrate {
public:
    bool loadFile(const std::string& path, std::string* why = nullptr);
    bool loadMemory(std::vector<u8> bytes, std::string* why = nullptr);

    // -1 when this layer has no spec at `path` ("/", "/World/Tree", "/World/Tree.points").
    i32 specIndex(std::string_view path) const;
    usize specCount() const { return specs_.size(); }
    UsdSpecType specType(i32 spec) const;
    const std::string& specPath(i32 spec) const;

    bool hasField(i32 spec, std::string_view name) const;
    // Decodes one field of one spec. False when the spec has no such field, or when its value is one
    // of the kinds this reader does not decode (see the header comment); `why` says which.
    bool field(i32 spec, std::string_view name, UsdCrateValue& out, std::string* why = nullptr) const;
    // `field` looked up by path.
    bool field(std::string_view path, std::string_view name, UsdCrateValue& out,
               std::string* why = nullptr) const {
        return field(specIndex(path), name, out, why);
    }

    // "0.9.0".
    std::string version() const;

private:
    struct Spec { u32 path; u32 fieldSet; UsdSpecType type; };

    bool parse(std::string* why);
    bool decode(u64 rep, UsdCrateValue& out, std::string* why) const;
    i32 fieldRep(i32 spec, std::string_view name, u64& rep) const;

    std::vector<u8> bytes_;
    u8 ver_[3] = {0, 0, 0};
    std::vector<std::string> tokens_;
    std::vector<u32> strings_;                  // string index -> token index
    std::vector<std::pair<u32, u64>> fields_;   // (name token, value rep)
    std::vector<u32> fieldSets_;                // field indices, each set ended by 0xFFFFFFFF
    std::vector<std::string> paths_;
    std::vector<Spec> specs_;
    std::unordered_map<std::string, i32> specByPath_;
    std::unordered_map<std::string, u32> tokenIndex_;
};

} // namespace aver::fmt
