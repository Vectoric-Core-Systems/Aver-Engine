#pragma once
// A minimal, strict JSON reader. Written rather than vendored.
//
// WHY THIS EXISTS. glTF is the import format this engine needs -- it is open, and one file carries
// meshes, skeletons and animations, which is exactly the set .ocmesh/.ocskel/.ocanim was built for.
// glTF is JSON, and this tree vendors no JSON library: third_party holds Jolt, imgui, stb and fonts.
// The alternative was vendoring one, which needs a licence review and a file this machine cannot
// download. glTF's JSON is a constrained subset -- no comments, no NaN, no trailing commas, nesting
// a handful deep -- so a correct reader for it is a few hundred lines, and owning it outright is
// worth more here than the generality a library would bring.
//
// STRICT ON PURPOSE. glTF is machine-generated: a trailing comma or an unquoted key means the
// exporter is wrong, and accepting it quietly turns a reportable bug into a mesh that is subtly
// missing data. Everything this rejects, it rejects with a byte offset.
//
// DOM, not a callback parser. An importer needs random access -- accessor 4 refers to bufferView 9
// refers to buffer 0 -- and a streaming reader would mean either two passes or building this anyway.
//
// NOT A GENERAL-PURPOSE JSON LIBRARY, and not trying to be: no writer, no comments, no big integers
// beyond what a double holds exactly, no duplicate-key policy beyond "last wins".
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

class JsonValue;

// Objects keep INSERTION ORDER and are looked up linearly. glTF objects are small -- a primitive has
// five keys, an accessor eight -- so a hash map would cost more in construction than it saves in
// lookup, and order-preserving makes a failure message reproducible.
struct JsonMember;

class JsonValue {
public:
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Accessors with a default. Every one is total: asking a string for its number gives the
    // fallback rather than throwing, because an importer reading optional glTF fields would
    // otherwise need a type check before every read.
    bool        asBool  (bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    f64         asDouble(f64 fallback = 0.0)    const { return type_ == Type::Number ? num_ : fallback; }
    f32         asFloat (f32 fallback = 0.0f)   const { return type_ == Type::Number ? static_cast<f32>(num_) : fallback; }
    i64         asInt   (i64 fallback = 0)      const { return type_ == Type::Number ? static_cast<i64>(num_) : fallback; }
    std::string_view asString(std::string_view fallback = {}) const {
        return type_ == Type::String ? std::string_view(str_) : fallback;
    }

    usize size() const;                       // elements for an array, members for an object, else 0
    const JsonValue& operator[](usize i) const;          // array element; a null value if out of range
    const JsonValue& operator[](std::string_view key) const;  // object member; a null value if absent
    bool has(std::string_view key) const;

    const std::vector<JsonValue>& elements() const { return arr_; }
    const std::vector<JsonMember>& members() const;

private:
    friend class JsonParser;
    friend struct JsonMember;
    Type type_ = Type::Null;
    bool bool_ = false;
    f64  num_  = 0.0;
    std::string str_;
    std::vector<JsonValue>  arr_;
    std::vector<JsonMember> obj_;
};

struct JsonMember {
    std::string key;
    JsonValue   value;
};

// Parse a whole document. Returns false and sets `why` -- with a byte offset -- on anything
// malformed. `out` is left in an unspecified state on failure and must not be read.
bool parseJson(std::string_view text, JsonValue& out, std::string* why = nullptr);

} // namespace aver::fmt
