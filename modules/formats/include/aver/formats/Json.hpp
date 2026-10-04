#pragma once
// A minimal, strict JSON reader for glTF import. DOM, no writer, no comments, last duplicate key
// wins, and every rejection carries a byte offset.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

class JsonValue;

struct JsonMember;

// One parsed JSON value. Objects keep insertion order and are looked up linearly.
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

    // Total accessors: a type mismatch gives the fallback rather than throwing.
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
    // True when the object has this key.
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

// One key/value pair of an object.
struct JsonMember {
    std::string key;
    JsonValue   value;
};

// Parses a whole document. Returns false with `why` set, and `out` unspecified, on anything malformed.
bool parseJson(std::string_view text, JsonValue& out, std::string* why = nullptr);

} // namespace aver::fmt
