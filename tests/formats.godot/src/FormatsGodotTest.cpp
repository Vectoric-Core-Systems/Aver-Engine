// FormatsGodotTest -- the Godot 4 text-format reader (modules/formats.godot).
//
// Fixtures are small hand-written Godot text files in tests/fabric/data/ (a mini project: project.godot,
// main.tscn instancing props/crate.tscn, a .tres material, an .import, a .gdextension, uid sidecars, a
// stand-in binary .scn, and variants.tres, which spells every Variant literal the parser knows). Every
// Variant type is checked twice: against the expected value, and by writing it back to text with
// godotVariantToText and parsing that again (must be the same tree).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/godot/GodotProjectFs.hpp"

#include <cmath>
#include <initializer_list>
#include <string>

using namespace aver;
using namespace aver::fmt::godot;

static int g_failures = 0;
static const std::string kData = AVER_FABRIC_DATA;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f64 a, f64 b, f64 eps = 1e-9) { return std::fabs(a - b) <= eps; }

static bool numsAre(const GodotVariant* v, std::initializer_list<f64> want) {
    if (!v || v->nums.size() != want.size()) return false;
    usize i = 0;
    for (f64 w : want)
        if (!near(v->nums[i++], w)) return false;
    return true;
}

static void dump(const GodotDiagnostics& d) {
    for (const GodotDiag& x : d.items) AVER_INFO("      diag: {}", x.str());
}

static bool hasDiag(const GodotDiagnostics& d, GodotSeverity sev, const std::string& code, const std::string& textPart = {}) {
    for (const GodotDiag& x : d.items)
        if (x.severity == sev && (code.empty() || x.code == code) &&
            (textPart.empty() || x.message.find(textPart) != std::string::npos))
            return true;
    return false;
}

static bool hasErrorAtLine(const GodotDiagnostics& d, int line, const std::string& textPart) {
    for (const GodotDiag& x : d.items)
        if (x.severity == GodotSeverity::Error && x.line == line && x.message.find(textPart) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------

static void testScalars() {
    AVER_INFO("=== Variant: scalars, strings, containers ===");
    GodotVariant v;
    GodotDiagnostics d;
    check(parseGodotVariant("null", v, &d) && v.type == GodotVType::Nil, "null");
    check(parseGodotVariant("nil", v, &d) && v.type == GodotVType::Nil, "nil (Godot 3 spelling)");
    check(parseGodotVariant("true", v, &d) && v.type == GodotVType::Bool && v.b, "true");
    check(parseGodotVariant("false", v, &d) && v.type == GodotVType::Bool && !v.b, "false");
    check(parseGodotVariant("-42", v, &d) && v.type == GodotVType::Int && v.i == -42, "negative int");
    check(parseGodotVariant("0x1F", v, &d) && v.type == GodotVType::Int && v.i == 31, "hex int");
    check(parseGodotVariant("7.0", v, &d) && v.type == GodotVType::Float && v.f == 7.0, "7.0 is a float, not an int");
    check(parseGodotVariant("7", v, &d) && v.type == GodotVType::Int, "7 is an int");
    check(parseGodotVariant("1.5e-3", v, &d) && near(v.f, 0.0015), "exponent float");
    check(parseGodotVariant("-2E+2", v, &d) && near(v.f, -200.0), "signed capital exponent");
    check(parseGodotVariant(".5", v, &d) && near(v.f, 0.5), "leading-dot float");
    check(parseGodotVariant("-.25", v, &d) && near(v.f, -0.25), "negative leading-dot float");
    check(parseGodotVariant("inf", v, &d) && std::isinf(v.f) && v.f > 0, "inf");
    check(parseGodotVariant("inf_neg", v, &d) && std::isinf(v.f) && v.f < 0, "inf_neg");
    check(parseGodotVariant("-inf", v, &d) && std::isinf(v.f) && v.f < 0, "-inf");
    check(parseGodotVariant("nan", v, &d) && std::isnan(v.f), "nan");
    check(parseGodotVariant("99999999999999999999", v, &d) && v.type == GodotVType::Float && v.f > 9.9e19,
          "an int past 64 bits becomes a float");
    check(parseGodotVariant("\"a\\tb\\n\\\"q\\\"\\\\\"", v, &d) && v.s == "a\tb\n\"q\"\\", "string escapes");
    check(parseGodotVariant("\"\\u00e9\"", v, &d) && v.s == "\xC3\xA9", "\\u escape encodes UTF-8");
    check(parseGodotVariant("\"\\U01f600\"", v, &d) && v.s == "\xF0\x9F\x98\x80", "\\U escape (6 digits)");
    check(parseGodotVariant("\"\\ud83d\\ude00\"", v, &d) && v.s == "\xF0\x9F\x98\x80", "a surrogate pair is one code point");
    check(parseGodotVariant("\"a\nb\"", v, &d) && v.s == "a\nb", "a string may span lines");
    check(parseGodotVariant("&\"sn\"", v, &d) && v.type == GodotVType::StringName && v.s == "sn", "StringName &\"x\"");
    check(parseGodotVariant("^\"A/B\"", v, &d) && v.type == GodotVType::NodePath && v.s == "A/B", "NodePath ^\"x\"");
    check(parseGodotVariant("NodePath(\"../C:pos\")", v, &d) && v.type == GodotVType::NodePath && v.s == "../C:pos", "NodePath(\"x\")");
    check(parseGodotVariant("[1, 2.0, \"t\", [4], {\"k\": 5},]", v, &d) && v.type == GodotVType::Array && v.items.size() == 5 &&
              v.items[1].type == GodotVType::Float && v.items[3].type == GodotVType::Array && v.items[4].find("k") &&
              v.items[4].find("k")->i == 5,
          "array of mixed values with a trailing comma");
    check(parseGodotVariant("{\n\"a\": 1,\n2: Vector2(0, 0)\n}", v, &d) && v.type == GodotVType::Dictionary && v.items.size() == 2 &&
              v.find("a") && v.find("a")->i == 1 && v.keys[1].type == GodotVType::Int && v.items[1].type == GodotVType::Vector2,
          "dictionary with string and int keys across lines");
    check(parseGodotVariant("  ; comment\n  5 ; trailing", v, &d) && v.i == 5, "comments and blanks around a value");
    check(d.items.empty(), "none of the above produced a diagnostic");
    dump(d);
}

static void testMathTypes() {
    AVER_INFO("=== Variant: math types, Color, containers ===");
    GodotVariant v;
    GodotDiagnostics d;
    check(parseGodotVariant("Vector2(1, 2.5)", v, &d) && v.type == GodotVType::Vector2 && numsAre(&v, {1, 2.5}), "Vector2");
    check(parseGodotVariant("Vector2i(3, -4)", v, &d) && v.type == GodotVType::Vector2i && numsAre(&v, {3, -4}), "Vector2i");
    check(parseGodotVariant("Vector3(1,2,3)", v, &d) && v.type == GodotVType::Vector3 && numsAre(&v, {1, 2, 3}), "Vector3 (no spaces)");
    check(parseGodotVariant("Vector3i( 1 , -2 , 3 )", v, &d) && v.type == GodotVType::Vector3i && numsAre(&v, {1, -2, 3}), "Vector3i (loose spaces)");
    check(parseGodotVariant("Vector4(1, 2, 3, 4)", v, &d) && v.type == GodotVType::Vector4 && numsAre(&v, {1, 2, 3, 4}), "Vector4");
    check(parseGodotVariant("Vector4i(5, 6, 7, 8)", v, &d) && v.type == GodotVType::Vector4i && numsAre(&v, {5, 6, 7, 8}), "Vector4i");
    check(parseGodotVariant("Rect2(0, 1, 2, 3)", v, &d) && v.type == GodotVType::Rect2 && numsAre(&v, {0, 1, 2, 3}), "Rect2");
    check(parseGodotVariant("Rect2i(-1, -2, 3, 4)", v, &d) && v.type == GodotVType::Rect2i && numsAre(&v, {-1, -2, 3, 4}), "Rect2i");
    check(parseGodotVariant("Quaternion(0, 0, 0.7071, 0.7071)", v, &d) && v.type == GodotVType::Quaternion && numsAre(&v, {0, 0, 0.7071, 0.7071}),
          "Quaternion x,y,z,w");
    check(parseGodotVariant("Basis(1, 2, 3, 4, 5, 6, 7, 8, 9)", v, &d) && v.type == GodotVType::Basis && v.nums.size() == 9 && v.nums[3] == 4,
          "Basis (nine numbers, row by row)");
    check(parseGodotVariant("Transform2D(1, 0, 0, 1, 5, 6)", v, &d) && v.type == GodotVType::Transform2D && numsAre(&v, {1, 0, 0, 1, 5, 6}), "Transform2D");
    check(parseGodotVariant("Transform3D(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)", v, &d) && v.type == GodotVType::Transform3D && v.nums.size() == 12 &&
              v.nums[9] == 10 && v.nums[11] == 12,
          "Transform3D (basis rows then origin)");
    check(parseGodotVariant("Plane(0, 1, 0, 2)", v, &d) && v.type == GodotVType::Plane && numsAre(&v, {0, 1, 0, 2}), "Plane");
    check(parseGodotVariant("AABB(-1, -1, -1, 2, 2, 2)", v, &d) && v.type == GodotVType::AABB && numsAre(&v, {-1, -1, -1, 2, 2, 2}), "AABB");
    check(parseGodotVariant("Projection(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)", v, &d) && v.type == GodotVType::Projection && v.nums.size() == 16, "Projection");
    check(parseGodotVariant("Color(0.1, 0.2, 0.3, 0.4)", v, &d) && v.type == GodotVType::Color && numsAre(&v, {0.1, 0.2, 0.3, 0.4}), "Color");
    check(parseGodotVariant("Vector3(inf, -inf, nan)", v, &d) && std::isinf(v.nums[0]) && std::isinf(v.nums[1]) && std::isnan(v.nums[2]),
          "special floats inside a constructor");

    check(parseGodotVariant("Array[Vector3]([Vector3(1, 2, 3)])", v, &d) && v.type == GodotVType::Array && v.typed && v.s == "Vector3" &&
              v.items.size() == 1,
          "typed array of a builtin");
    check(parseGodotVariant("Array[ExtResource(\"1_s\")]([null])", v, &d) && v.typed && v.s == "ExtResource(\"1_s\")" && v.items.size() == 1,
          "typed array of a script class");
    check(parseGodotVariant("Dictionary[String, int]({\"x\": 1})", v, &d) && v.type == GodotVType::Dictionary && v.typed && v.s == "String" &&
              v.s2 == "int" && v.find("x") && v.find("x")->i == 1,
          "typed dictionary");
    check(parseGodotVariant("Array([1])", v, &d) && v.type == GodotVType::Array && !v.typed && v.items.size() == 1, "Array(...) wrapper");

    check(parseGodotVariant("PackedByteArray(\"AQID\")", v, &d) && v.type == GodotVType::PackedByteArray && v.bytes == std::vector<u8>({1, 2, 3}),
          "PackedByteArray from base64");
    check(parseGodotVariant("PackedByteArray(1, 2, 255)", v, &d) && v.bytes == std::vector<u8>({1, 2, 255}), "PackedByteArray from ints");
    check(parseGodotVariant("PackedInt32Array(1, -2, 3)", v, &d) && v.type == GodotVType::PackedInt32Array && numsAre(&v, {1, -2, 3}), "PackedInt32Array");
    check(parseGodotVariant("PackedInt64Array(4294967296, 2)", v, &d) && numsAre(&v, {4294967296.0, 2}), "PackedInt64Array");
    check(parseGodotVariant("PackedFloat32Array(0.5, 1.5)", v, &d) && v.type == GodotVType::PackedFloat32Array && numsAre(&v, {0.5, 1.5}), "PackedFloat32Array");
    check(parseGodotVariant("PackedFloat64Array(0.25)", v, &d) && v.type == GodotVType::PackedFloat64Array && numsAre(&v, {0.25}), "PackedFloat64Array");
    check(parseGodotVariant("PackedStringArray(\"a\", \"b c\", \"\")", v, &d) && v.type == GodotVType::PackedStringArray &&
              v.strs == std::vector<std::string>({"a", "b c", ""}),
          "PackedStringArray");
    check(parseGodotVariant("PackedVector2Array(0, 1, 2, 3)", v, &d) && v.type == GodotVType::PackedVector2Array && v.count() == 2, "PackedVector2Array");
    check(parseGodotVariant("PackedVector3Array(0, 1, 2, 3, 4, 5)", v, &d) && v.type == GodotVType::PackedVector3Array && v.count() == 2, "PackedVector3Array");
    check(parseGodotVariant("PackedVector4Array(0, 1, 2, 3)", v, &d) && v.type == GodotVType::PackedVector4Array && v.count() == 1, "PackedVector4Array");
    check(parseGodotVariant("PackedColorArray(1, 0, 0, 1, 0, 1, 0, 0.5)", v, &d) && v.type == GodotVType::PackedColorArray && v.count() == 2 &&
              near(v.nums[7], 0.5),
          "PackedColorArray");
    check(parseGodotVariant("PoolIntArray( 1, 2 )", v, &d) && v.type == GodotVType::PackedInt32Array && v.count() == 2, "Godot 3 PoolIntArray");
    check(parseGodotVariant("Quat( 0, 0, 0, 1 )", v, &d) && v.type == GodotVType::Quaternion, "Godot 3 Quat");
    check(parseGodotVariant("Transform( 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 )", v, &d) && v.type == GodotVType::Transform3D, "Godot 3 Transform");

    check(parseGodotVariant("ExtResource(\"1_abc\")", v, &d) && v.type == GodotVType::ExtResource && v.s == "1_abc", "ExtResource(\"id\")");
    check(parseGodotVariant("ExtResource( 3 )", v, &d) && v.type == GodotVType::ExtResource && v.s == "3", "ExtResource( 3 ) (format 2)");
    check(parseGodotVariant("SubResource(\"Box_1\")", v, &d) && v.type == GodotVType::SubResource && v.s == "Box_1", "SubResource");
    check(parseGodotVariant("Resource(\"res://x.tres\")", v, &d) && v.type == GodotVType::Resource && v.s == "res://x.tres", "Resource(\"path\")");
    check(parseGodotVariant("RID(7)", v, &d) && v.type == GodotVType::Rid && v.i == 7, "RID");
    check(parseGodotVariant("Callable()", v, &d) && v.type == GodotVType::Callable && v.items.empty(), "Callable()");
    check(parseGodotVariant("Signal()", v, &d) && v.type == GodotVType::Signal, "Signal()");
    check(parseGodotVariant("Object(InputEventKey,\"keycode\":65,\"echo\":false,\"mods\":[1, 2])", v, &d) && v.type == GodotVType::Object &&
              v.s == "InputEventKey" && v.items.size() == 3 && v.find("keycode") && v.find("keycode")->i == 65 && v.find("mods") &&
              v.find("mods")->items.size() == 2,
          "Object(Class, \"prop\": value, ...)");
    check(parseGodotVariant("Object(Node)", v, &d) && v.s == "Node" && v.items.empty(), "Object with no properties");
    check(d.items.empty(), "none of the above produced a diagnostic");
    dump(d);
}

static void testVariantErrors() {
    AVER_INFO("=== Variant: errors carry file:line ===");
    GodotVariant v;
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("Vector3(1, 2)", v, &d, "a.tres", 10) && hasErrorAtLine(d, 10, "takes 3 numbers"), "Vector3 with two numbers: error at its line");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("[1,\n2,\n\"open", v, &d, "a.tres", 5) && hasErrorAtLine(d, 7, "unterminated string"), "unterminated string reports the line it opened on");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("Wibble(1)", v, &d, "a.tres") && hasDiag(d, GodotSeverity::Error, "godot.parse", "unknown type 'Wibble'"), "unknown constructor");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("PackedInt32Array(1, \"x\")", v, &d), "a string inside a numeric packed array");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("PackedVector3Array(1, 2)", v, &d) && hasDiag(d, GodotSeverity::Error, "", "multiple of 3"), "packed vector with a partial element");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("{ \"a\" 1 }", v, &d), "dictionary without a colon");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("1 2", v, &d) && hasDiag(d, GodotSeverity::Error, "", "after the value"), "trailing text after a value");
    }
    {
        GodotDiagnostics d;
        check(!parseGodotVariant("PackedByteArray(\"!!\")", v, &d), "bad base64");
    }
    {
        GodotDiagnostics d;
        std::string deep(300, '[');
        check(!parseGodotVariant(deep, v, &d) && hasDiag(d, GodotSeverity::Error, "", "nested too deeply"), "absurd nesting is refused, not a stack overflow");
    }
}

// Writes `v` as text, parses that, and requires the same tree.
static bool roundTrips(const GodotVariant& v, std::string* textOut = nullptr) {
    const std::string text = godotVariantToText(v);
    if (textOut) *textOut = text;
    GodotVariant back;
    GodotDiagnostics d;
    if (!parseGodotVariant(text, back, &d)) return false;
    return back == v;
}

static void testVariantsFixture() {
    AVER_INFO("=== variants.tres: every literal, value and round trip ===");
    GodotResource r;
    GodotDiagnostics d;
    const bool ok = loadGodotResource(kData + "/MiniGame/variants.tres", r, d, "res://variants.tres");
    dump(d);
    check(ok && d.errorCount() == 0, "variants.tres parses with no errors");
    check(r.header.kind == "gd_resource" && r.header.type == "Resource" && r.header.format == 3 && r.header.uid == "uid://c5variants00",
          "header: gd_resource, type Resource, format 3, uid");
    check(r.extResources.size() == 1 && r.subResources.size() == 1, "one ext resource and one sub resource");

    auto P = [&](const char* n) { return r.prop(n); };
    check(P("v_null") && P("v_null")->isNil(), "v_null");
    check(P("v_true") && P("v_true")->b && P("v_false") && !P("v_false")->b, "v_true / v_false");
    check(P("v_int") && P("v_int")->i == -42, "v_int");
    check(P("v_hex") && P("v_hex")->i == 255, "v_hex");
    check(P("v_big") && P("v_big")->type == GodotVType::Int && P("v_big")->i == 9007199254740993LL, "v_big keeps all 64 bits");
    check(P("v_float") && P("v_float")->f == 3.5, "v_float");
    check(P("v_exp") && near(P("v_exp")->f, 0.0015) && P("v_neg_exp") && near(P("v_neg_exp")->f, -200.0), "exponent floats");
    check(P("v_whole") && P("v_whole")->type == GodotVType::Float, "7.0 stays a float");
    check(P("v_inf") && std::isinf(P("v_inf")->f) && P("v_ninf") && P("v_ninf")->f < 0 && P("v_nan") && std::isnan(P("v_nan")->f), "inf, inf_neg, nan");
    check(P("v_string") && P("v_string")->s == "line1\nline2\ttab \"q\" \\ \xC3\xA9 \xF0\x9F\x98\x80 \xF0\x9F\x98\x80", "v_string escapes and code points");
    check(P("v_multiline") && P("v_multiline")->s == "a\nb", "v_multiline");
    check(P("v_sname") && P("v_sname")->type == GodotVType::StringName && P("v_sname")->s == "stringname", "v_sname");
    check(P("v_npath") && P("v_npath")->type == GodotVType::NodePath && P("v_npath")->s == "../Crate:position", "v_npath");
    check(P("v_npath2") && P("v_npath2")->type == GodotVType::NodePath && P("v_npath2")->s == "A/B", "v_npath2");
    check(numsAre(P("v_v2"), {1, 2.5}) && P("v_v2")->type == GodotVType::Vector2, "v_v2");
    check(numsAre(P("v_v2i"), {3, -4}) && P("v_v2i")->type == GodotVType::Vector2i, "v_v2i");
    check(numsAre(P("v_v3"), {1, 2, 3}) && P("v_v3")->type == GodotVType::Vector3, "v_v3");
    check(numsAre(P("v_v3i"), {1, -2, 3}) && P("v_v3i")->type == GodotVType::Vector3i, "v_v3i");
    check(numsAre(P("v_v4"), {1, 2, 3, 4}) && P("v_v4")->type == GodotVType::Vector4, "v_v4");
    check(numsAre(P("v_v4i"), {5, 6, 7, 8}) && P("v_v4i")->type == GodotVType::Vector4i, "v_v4i");
    check(numsAre(P("v_rect"), {0, 1, 2, 3}) && P("v_rect")->type == GodotVType::Rect2, "v_rect");
    check(numsAre(P("v_recti"), {-1, -2, 3, 4}) && P("v_recti")->type == GodotVType::Rect2i, "v_recti");
    check(numsAre(P("v_quat"), {0, 0, 0.7071, 0.7071}) && P("v_quat")->type == GodotVType::Quaternion, "v_quat");
    check(numsAre(P("v_basis"), {1, 2, 3, 4, 5, 6, 7, 8, 9}) && P("v_basis")->type == GodotVType::Basis, "v_basis");
    check(numsAre(P("v_t2d"), {1, 0, 0, 1, 5, 6}) && P("v_t2d")->type == GodotVType::Transform2D, "v_t2d");
    check(numsAre(P("v_t3d"), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) && P("v_t3d")->type == GodotVType::Transform3D, "v_t3d");
    check(numsAre(P("v_plane"), {0, 1, 0, 2}) && P("v_plane")->type == GodotVType::Plane, "v_plane");
    check(numsAre(P("v_aabb"), {-1, -1, -1, 2, 2, 2}) && P("v_aabb")->type == GodotVType::AABB, "v_aabb");
    check(P("v_proj") && P("v_proj")->type == GodotVType::Projection && P("v_proj")->nums.size() == 16 && P("v_proj")->nums[5] == 1, "v_proj");
    check(numsAre(P("v_color"), {0.1, 0.2, 0.3, 0.4}) && P("v_color")->type == GodotVType::Color, "v_color");
    check(P("v_array") && P("v_array")->items.size() == 5 && P("v_array")->items[4].find("five") && P("v_array")->items[4].find("five")->i == 5, "v_array (nested, multi-line)");
    check(P("v_empty_array") && P("v_empty_array")->type == GodotVType::Array && P("v_empty_array")->items.empty(), "v_empty_array");
    check(P("v_typed_array") && P("v_typed_array")->typed && P("v_typed_array")->s == "Vector3" && P("v_typed_array")->items.size() == 2, "v_typed_array");
    check(P("v_typed_ext") && P("v_typed_ext")->typed && P("v_typed_ext")->s == "ExtResource(\"1_script\")", "v_typed_ext");
    check(P("v_dict") && P("v_dict")->items.size() == 3 && P("v_dict")->find("a")->i == 1 && P("v_dict")->keys[2].i == 2, "v_dict");
    check(P("v_empty_dict") && P("v_empty_dict")->type == GodotVType::Dictionary && P("v_empty_dict")->items.empty(), "v_empty_dict");
    check(P("v_typed_dict") && P("v_typed_dict")->typed && P("v_typed_dict")->s == "String" && P("v_typed_dict")->s2 == "int", "v_typed_dict");
    check(P("v_bytes") && P("v_bytes")->bytes == std::vector<u8>({1, 2, 3}), "v_bytes");
    check(P("v_bytes_list") && P("v_bytes_list")->bytes == std::vector<u8>({1, 2, 255}), "v_bytes_list");
    check(numsAre(P("v_i32"), {1, -2, 3}) && numsAre(P("v_i64"), {4294967296.0, 2}), "v_i32, v_i64");
    check(numsAre(P("v_f32"), {0.5, 1.5}) && numsAre(P("v_f64"), {0.25}), "v_f32, v_f64");
    check(P("v_strs") && P("v_strs")->strs == std::vector<std::string>({"a", "b c", ""}), "v_strs");
    check(P("v_v2s") && P("v_v2s")->count() == 2 && P("v_v3s") && P("v_v3s")->count() == 2 && P("v_v4s") && P("v_v4s")->count() == 1, "v_v2s, v_v3s, v_v4s");
    check(P("v_cols") && P("v_cols")->count() == 2 && near(P("v_cols")->nums[7], 0.5), "v_cols");
    check(P("v_ext") && P("v_ext")->type == GodotVType::ExtResource && P("v_ext")->s == "1_script", "v_ext");
    check(P("v_sub") && P("v_sub")->type == GodotVType::SubResource && P("v_sub")->s == "BoxMesh_v", "v_sub");
    check(P("v_res") && P("v_res")->type == GodotVType::Resource && P("v_res")->s == "res://other.tres", "v_res");
    check(P("v_rid") && P("v_rid")->type == GodotVType::Rid, "v_rid");
    check(P("v_obj") && P("v_obj")->type == GodotVType::Object && P("v_obj")->s == "InputEventKey" && P("v_obj")->find("keycode")->i == 65, "v_obj");
    check(P("v_callable") && P("v_callable")->type == GodotVType::Callable && P("v_signal") && P("v_signal")->type == GodotVType::Signal, "v_callable, v_signal");
    check(P("v_old_ints") && P("v_old_ints")->type == GodotVType::PackedInt32Array && P("v_old_xform") && P("v_old_xform")->type == GodotVType::Transform3D &&
              P("v_old_quat") && P("v_old_quat")->type == GodotVType::Quaternion,
          "Godot 3 spellings");

    // Round trip: every property written as text and read back must be the same tree.
    int bad = 0;
    int total = 0;
    for (const GodotProperty& p : r.props) {
        ++total;
        std::string text;
        if (!roundTrips(p.value, &text)) {
            ++bad;
            AVER_ERROR("   round trip failed for {}: {}", p.name, text);
        }
    }
    check(total >= 60 && bad == 0, "all " + std::to_string(total) + " properties round trip through godotVariantToText");

    // A whole document: re-serialise the [resource] properties, parse them as a resource, same values.
    std::string doc = "[gd_resource type=\"Resource\" format=3]\n\n[ext_resource type=\"Script\" path=\"res://scripts/player.gd\" id=\"1_script\"]\n\n"
                      "[sub_resource type=\"BoxMesh\" id=\"BoxMesh_v\"]\n\n[resource]\n";
    for (const GodotProperty& p : r.props) doc += p.name + " = " + godotVariantToText(p.value) + "\n";
    GodotResource r2;
    GodotDiagnostics d2;
    const bool ok2 = parseGodotResource(doc, "regen.tres", r2, d2);
    dump(d2);
    bool same = ok2 && r2.props.size() == r.props.size();
    for (usize i = 0; same && i < r.props.size(); ++i) same = r.props[i].name == r2.props[i].name && r.props[i].value == r2.props[i].value;
    check(same, "a regenerated variants.tres reads back to the same properties");
}

static void testMainScene() {
    AVER_INFO("=== main.tscn ===");
    GodotScene s;
    GodotDiagnostics d;
    const bool ok = loadGodotScene(kData + "/MiniGame/main.tscn", s, d, "res://main.tscn");
    dump(d);
    check(ok && d.errorCount() == 0 && d.warningCount() == 0, "parses with no errors or warnings");
    check(s.header.kind == "gd_scene" && s.header.format == 3 && s.header.loadSteps == 8 && s.header.uid == "uid://b6x2mainscene", "header");
    check(s.extResources.size() == 4, "four ext resources");
    const GodotExtResource* crate = s.findExt("1_crate");
    check(crate && crate->type == "PackedScene" && crate->path == "res://props/crate.tscn" && crate->uid == "uid://c1crate00000", "ext resource fields");
    check(s.subResources.size() == 3 && s.findSub("BoxMesh_a1") && s.findSub("BoxMesh_a1")->type == "BoxMesh" &&
              numsAre(s.findSub("BoxMesh_a1")->prop("size"), {2, 0.5, 2}),
          "sub resources with properties");
    const GodotSubResource* mat = s.findSub("StandardMaterial3D_c3");
    check(mat && mat->prop("albedo_texture") && mat->prop("albedo_texture")->type == GodotVType::ExtResource && mat->prop("albedo_texture")->s == "4_tex" &&
              near(mat->prop("metallic")->f, 0.25),
          "a sub resource referencing an ext resource");

    check(s.nodes.size() == 10, "ten nodes");
    const GodotNode& root = s.nodes[0];
    check(root.isRoot && root.name == "Main" && root.type == "Node3D" && root.path == "." && root.parent == -1, "root node");
    const int floor = s.findNode("Floor");
    const int mesh = s.findNode("Floor/Mesh");
    check(floor == 1 && mesh == 2 && s.nodes[mesh].parent == floor && s.nodes[floor].parent == 0, "nested node paths resolve to parent indices");
    check(s.nodes[floor].groups == std::vector<std::string>({"ground", "solid"}), "groups=[...]");
    check(numsAre(s.nodes[floor].prop("transform"), {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.25, 0}), "Floor transform");
    check(s.nodes[floor].prop("collision_layer") && s.nodes[floor].prop("collision_layer")->i == 2, "Floor collision_layer");
    check(s.nodes[mesh].prop("surface_material_override/0") && s.nodes[mesh].prop("surface_material_override/0")->s == "StandardMaterial3D_c3", "slash-keyed property");
    const int player = s.findNode("Player");
    check(player >= 0 && s.nodes[player].nodePaths == std::vector<std::string>({"target"}) && s.nodes[player].groups == std::vector<std::string>({"player"}) &&
              s.nodes[player].prop("script") && s.nodes[player].prop("script")->s == "3_player" && s.nodes[player].prop("target")->type == GodotVType::NodePath &&
              s.nodes[player].prop("target")->s == "../Crate",
          "Player: node_paths, script ext resource, NodePath property");
    const int crateNode = s.findNode("Crate");
    check(crateNode >= 0 && s.nodes[crateNode].type.empty() && s.nodes[crateNode].instance == "1_crate" && s.nodes[crateNode].isInstance() && !s.nodes[crateNode].isOverride,
          "Crate is an instance=ExtResource node");
    const int lid = s.findNode("Crate/Lid");
    check(lid >= 0 && s.nodes[lid].isOverride && s.nodes[lid].parent == crateNode && s.nodes[lid].index == 1 && !s.nodes[lid].unresolvedParent &&
              s.nodes[lid].prop("visible") && !s.nodes[lid].prop("visible")->b,
          "Crate/Lid overrides a node of the instance (no type, no instance, index=1)");
    const int extra = s.findNode("Crate/Body/Extra");
    check(extra >= 0 && s.nodes[extra].type == "Marker3D" && s.nodes[extra].unresolvedParent && s.nodes[extra].parent == -1 && s.nodes[extra].anchor == crateNode &&
              s.nodes[extra].prop("unique_name_in_owner") && s.nodes[extra].prop("unique_name_in_owner")->b,
          "Crate/Body/Extra hangs under a node inside the instance: unresolved parent, anchored at Crate");
    const int trig = s.findNode("Trigger");
    check(trig >= 0 && s.nodes[trig].uniqueId == "123456" && s.nodes[trig].prop("metadata/tag") && s.nodes[trig].prop("metadata/tag")->s == "trigger", "unique_id and metadata/ keys");
    const int sun = s.findNode("Sun");
    check(sun >= 0 && s.nodes[sun].prop("light_energy") && s.nodes[sun].prop("light_energy")->f == 2.0 && s.nodes[sun].prop("shadow_enabled")->b, "an inline comment after a value");
    check(s.childrenOf(0).size() == 5, "the root has five children in this file (Lid and Extra hang under the instance)");

    check(s.connections.size() == 2, "two connections");
    const GodotConnection& c = s.connections[0];
    check(c.signal == "body_entered" && c.from == "Trigger" && c.to == "Player" && c.method == "_on_trigger_body_entered" && c.flags == 3 && c.binds.size() == 2 &&
              c.binds[0].i == 1 && c.binds[1].s == "x",
          "connection with flags and binds");
    check(s.connections[1].to == "." && s.connections[1].flags == 0 && s.connections[1].binds.empty(), "connection to the root");
    check(s.editablePaths == std::vector<std::string>({"Crate"}), "[editable path=\"Crate\"]");
    check(s.inheritsFrom().empty(), "not an inherited scene");
}

static void testInstancedScene() {
    AVER_INFO("=== crate.tscn and red.tres ===");
    GodotScene s;
    GodotDiagnostics d;
    check(loadGodotScene(kData + "/MiniGame/props/crate.tscn", s, d, "res://props/crate.tscn") && d.items.empty(), "crate.tscn parses cleanly");
    check(s.nodes.size() == 3 && s.nodes[0].type == "RigidBody3D" && s.nodes[0].prop("mass")->f == 5.0 && s.nodes[1].path == "Body" && s.nodes[2].path == "Lid", "its nodes");
    check(s.findSub("BoxMesh_crate") && s.findSub("BoxMesh_crate")->prop("material")->s == "1_red", "its sub resource points at the material ext resource");

    GodotResource r;
    GodotDiagnostics d2;
    check(loadGodotResource(kData + "/MiniGame/materials/red.tres", r, d2, "res://materials/red.tres") && d2.items.empty(), "red.tres parses cleanly");
    check(r.header.type == "StandardMaterial3D" && r.header.uid == "uid://c2redmat0000" && numsAre(r.prop("albedo_color"), {1, 0, 0, 1}) && near(r.prop("roughness")->f, 0.4) &&
              r.prop("shading_mode")->i == 0,
          "material resource: header and [resource] properties");

    GodotDiagnostics d3;
    GodotScene wrong;
    check(!loadGodotScene(kData + "/MiniGame/materials/red.tres", wrong, d3) && hasDiag(d3, GodotSeverity::Error, "godot.kind"), "a .tres read as a scene is refused");
    check(detectGodotTextKind("\n; c\n[gd_scene format=3]") == "gd_scene" && detectGodotTextKind("[gd_resource type=\"X\" format=3]") == "gd_resource" &&
              detectGodotTextKind("[remap]").empty(),
          "detectGodotTextKind");
}

static void testSceneErrors() {
    AVER_INFO("=== scene diagnostics ===");
    {
        GodotScene s;
        GodotDiagnostics d;
        const std::string text =
            "[gd_scene format=3]\n"
            "\n"
            "[node name=\"R\" type=\"Node3D\"]\n"
            "a = Vector3(1, 2)\n"
            "[node name=\"C\" type=\"Node3D\" parent=\"Nope/Deeper\"]\n"
            "script = ExtResource(\"missing\")\n"
            "[node name=\"C2\" type=\"Node3D\" parent=\".\"]\n"
            "[node name=\"C2\" type=\"Node3D\" parent=\".\"]\n";
        check(!parseGodotScene(text, "bad.tscn", s, d), "a broken scene reports failure");
        check(hasErrorAtLine(d, 4, "takes 3 numbers"), "bad literal: bad.tscn line 4");
        check(hasErrorAtLine(d, 5, "not a node of this scene"), "missing parent: line 5");
        check(hasErrorAtLine(d, 6, "ExtResource(\"missing\")"), "undeclared ExtResource: line 6");
        check(hasDiag(d, GodotSeverity::Error, "godot.node_path", "two nodes have the path"), "duplicate node path");
        check(d.items[0].file == "bad.tscn" && d.items[0].str().rfind("bad.tscn:4:", 0) == 0, "diagnostics print as file:line");
        check(s.nodes.size() == 4, "parsing recovered: all four nodes are still listed");
    }
    {
        GodotScene s;
        GodotDiagnostics d;
        check(!parseGodotScene("[node name=\"X\"]\n", "nohead.tscn", s, d) && hasDiag(d, GodotSeverity::Error, "", "before the [gd_scene]"), "a node before the header");
    }
    {
        GodotScene s;
        GodotDiagnostics d;
        check(!parseGodotScene("", "empty.tscn", s, d), "an empty file is an error");
    }
    {
        GodotScene s;
        GodotDiagnostics d;
        check(!parseGodotScene("[gd_scene]\n[node name=\"X\" type=\"Node\"]\n[node name=\"Y\" type=\"Node\"]\n", "two.tscn", s, d) &&
                  hasDiag(d, GodotSeverity::Error, "godot.format", "no format") && hasDiag(d, GodotSeverity::Error, "", "no parent"),
              "no format=, and a second node with no parent");
    }
    {
        GodotScene s;
        GodotDiagnostics d;
        check(parseGodotScene("[gd_scene format=4]\n[node name=\"X\" type=\"Node\"]\n[wibble foo=1]\nx = 1\n", "new.tscn", s, d) && d.errorCount() == 0 &&
                  hasDiag(d, GodotSeverity::Warning, "godot.format") && hasDiag(d, GodotSeverity::Warning, "godot.unknown_section"),
              "a newer format and an unknown section are warnings, not errors");
    }
    {
        GodotScene s;
        GodotDiagnostics d;
        check(loadGodotScene(kData + "/Godot3Scene/old.tscn", s, d, "res://old.tscn") && d.errorCount() == 0 && hasDiag(d, GodotSeverity::Warning, "godot.format", "Godot 3"),
              "format=2 (Godot 3) reads, with a warning");
        check(s.header.format == 2 && s.extResources.size() == 1 && s.extResources[0].id == "1" && s.nodes.size() == 2 && s.nodes[1].instance == "1" &&
                  s.nodes[1].prop("pool") && s.nodes[1].prop("pool")->count() == 2 && s.nodes[1].prop("transform")->type == GodotVType::Transform3D,
              "format 2: integer ids, ExtResource( 1 ), PoolIntArray, Transform( ... )");
    }
}

static void testBinary() {
    AVER_INFO("=== binary resources ===");
    GodotDiagnostics d;
    std::string text;
    check(!readGodotTextFile(kData + "/MiniGame/binary/level.scn", text, d, "res://binary/level.scn"), "a binary .scn is refused");
    check(hasDiag(d, GodotSeverity::Error, "godot.binary_resource", "resave as text"), "with the 'resave as text' diagnostic");
    check(d.items.size() == 1 && d.items[0].file == "res://binary/level.scn", "naming the res:// file");
    GodotScene s;
    GodotDiagnostics d2;
    check(!loadGodotScene(kData + "/MiniGame/binary/level.scn", s, d2) && hasDiag(d2, GodotSeverity::Error, "godot.binary_resource"), "loadGodotScene reports it too");
    const u8 rsrc[] = {'R', 'S', 'R', 'C', 0, 1, 2};
    const u8 rscc[] = {'R', 'S', 'C', 'C', 0, 1, 2};
    const u8 gdsc[] = {'G', 'D', 'S', 'C', 0, 1, 2};
    const u8 txt[] = {'[', 'g', 'd', '_', 's', 'c', 'e', 'n', 'e', ']'};
    const u8 nul[] = {1, 0, 2, 0, 3};
    check(detectGodotFileKind(rsrc, sizeof rsrc) == GodotFileKind::BinaryResource && detectGodotFileKind(rscc, sizeof rscc) == GodotFileKind::BinaryResource &&
              detectGodotFileKind(gdsc, sizeof gdsc) == GodotFileKind::BinaryScript && detectGodotFileKind(txt, sizeof txt) == GodotFileKind::Text &&
              detectGodotFileKind(nul, sizeof nul) == GodotFileKind::Unknown,
          "detectGodotFileKind: RSRC, RSCC, GDSC, text, unknown");
    GodotDiagnostics d3;
    GodotScene missing;
    check(!loadGodotScene(kData + "/MiniGame/nope.tscn", missing, d3) && hasDiag(d3, GodotSeverity::Error, "godot.unreadable"), "a missing file is reported");
}

static void testProject() {
    AVER_INFO("=== project.godot ===");
    GodotProject p;
    GodotDiagnostics d;
    const bool ok = loadGodotProject(kData + "/MiniGame", p, d);
    dump(d);
    check(ok && d.errorCount() == 0 && d.warningCount() == 0, "loads from the folder, no errors or warnings");
    check(p.configVersion == 5 && p.name == "Mini Game" && p.mainScene == "res://main.tscn" && p.icon == "res://icon.svg", "config_version, name, main scene, icon");
    check(p.features == std::vector<std::string>({"4.3", "C#", "Forward Plus"}) && p.engineVersion == "4.3" && p.usesCSharp && p.renderingMethod == "forward_plus", "features: version, C#, renderer");
    check(p.dir.size() > 0 && p.dir.find("MiniGame") != std::string::npos, "dir is set by loadGodotProject");
    check(p.setting("display/window/size/viewport_width") && p.setting("display/window/size/viewport_width")->i == 1280, "setting(\"display/window/size/viewport_width\")");
    check(p.setting("config_version") && p.setting("config_version")->i == 5, "a top-level key: setting(\"config_version\")");
    check(p.setting("physics/3d/default_gravity") && near(p.setting("physics/3d/default_gravity")->f, 9.8), "physics/3d/default_gravity");
    check(p.autoloads.size() == 3, "three autoloads");
    check(p.autoloads[0].name == "Global" && p.autoloads[0].singleton && p.autoloads[0].path == "res://scripts/global.gd" && p.autoloads[0].order == 0, "Global=\"*res://...\" is a singleton");
    check(p.autoloads[1].name == "Helper" && !p.autoloads[1].singleton && p.autoloads[1].order == 1, "no '*': not a singleton");
    check(p.autoloads[2].name == "Bus" && p.autoloads[2].singleton && p.autoloads[2].path == "uid://c6busscript0", "an autoload by uid://");
    check(p.inputActions.size() == 2 && p.inputActions[0].name == "ui_accept" && p.inputActions[0].deadzone == 0.5 && p.inputActions[0].events.size() == 2, "input map: ui_accept has two events");
    check(p.inputActions[0].events[0].s == "InputEventKey" && p.inputActions[0].events[0].find("physical_keycode")->i == 32 && p.inputActions[0].events[1].s == "InputEventJoypadButton",
          "events are Object variants keeping their properties");
    check(p.inputActions[1].name == "move_left" && near(p.inputActions[1].deadzone, 0.2) && p.inputActions[1].events[0].find("physical_keycode")->i == 65, "move_left");
    check(p.enabledPlugins == std::vector<std::string>({"res://addons/mything/plugin.cfg"}), "enabled editor plugins");
    check(p.layerNames.size() == 2 && p.layerNames[0].first == "3d_physics/layer_1" && p.layerNames[0].second == "player", "layer names");

    GodotProject old;
    GodotDiagnostics d2;
    check(!parseGodotProject("config_version=4\n[application]\nconfig/name=\"Old\"\n", "project.godot", old, d2) && hasDiag(d2, GodotSeverity::Error, "godot.unsupported_version", "Godot 3"),
          "config_version 4 is refused as a Godot 3 project");
    GodotProject early;
    GodotDiagnostics d3;
    check(parseGodotProject("config_version=5\n[application]\nconfig/features=PackedStringArray(\"4.0\")\n", "project.godot", early, d3) &&
              hasDiag(d3, GodotSeverity::Warning, "godot.unsupported_version"),
          "features saying 4.0 warn (4.1+ is the target)");
    GodotProject none;
    GodotDiagnostics d4;
    check(!parseGodotProject("[application]\nconfig/name=\"X\"\n", "project.godot", none, d4), "no config_version is an error");
    GodotProject broken;
    GodotDiagnostics d5;
    check(!parseGodotProject("config_version=5\n[autoload]\nA=\"*res://a.gd\"\nB=Vector3(1,2)\nC=\"res://c.gd\"\n", "project.godot", broken, d5) && hasErrorAtLine(d5, 4, "3 numbers") &&
              broken.autoloads.size() == 2,
          "a bad value is reported by line and the rest of the file still reads");
}

static void testImportAndExtension() {
    AVER_INFO("=== .import and .gdextension ===");
    GodotImport imp;
    GodotDiagnostics d;
    const bool ok = loadGodotImport(kData + "/MiniGame/textures/albedo.png.import", imp, d, "res://textures/albedo.png.import");
    dump(d);
    check(ok && d.items.empty(), "albedo.png.import parses cleanly");
    check(imp.importer == "texture" && imp.type == "CompressedTexture2D" && imp.uid == "uid://c4albedo0000" && imp.valid, "[remap] importer, type, uid");
    check(imp.pathsByTag.size() == 2 && imp.pathsByTag[0].first == "s3tc" && imp.pathsByTag[1].first == "etc2" && imp.path.empty(), "path.<tag> outputs");
    check(imp.sourceFile == "res://textures/albedo.png" && imp.destFiles.size() == 2, "[deps] source_file and dest_files");
    check(imp.param("compress/mode") && imp.param("compress/mode")->i == 2 && imp.param("mipmaps/generate")->b && !imp.param("compress/high_quality")->b, "[params]");
    check(imp.metadata.type == GodotVType::Dictionary && imp.metadata.find("vram_texture") && imp.metadata.find("vram_texture")->b, "[remap] metadata dictionary");
    GodotImport bad;
    GodotDiagnostics d2;
    check(!parseGodotImport("[params]\na=1\n", "x.import", bad, d2), "an .import without [remap] is refused");

    GodotGdExtension ext;
    GodotDiagnostics d3;
    const bool ok2 = loadGodotGdExtension(kData + "/MiniGame/addons/mything/mything.gdextension", ext, d3, "res://addons/mything/mything.gdextension");
    dump(d3);
    check(ok2 && d3.items.empty(), "mything.gdextension parses cleanly");
    check(ext.entrySymbol == "mything_library_init" && ext.compatibilityMinimum == "4.1" && ext.compatibilityMaximum == "4.3" && ext.reloadable, "[configuration]");
    check(ext.libraries.size() == 4 && ext.libraries[1].key == "windows.debug.x86_64" &&
              ext.libraries[1].tags == std::vector<std::string>({"windows", "debug", "x86_64"}) && ext.libraries[1].path.find("template_debug.x86_64.dll") != std::string::npos,
          "[libraries] with feature tags");
    const GodotGdLibrary* sel = ext.selectLibrary({"windows", "release", "x86_64", "editor"});
    check(sel && sel->key == "windows.release.x86_64", "selectLibrary: windows + release + x86_64");
    sel = ext.selectLibrary({"windows", "debug", "x86_64"});
    check(sel && sel->key == "windows.debug.x86_64", "selectLibrary: windows + debug");
    sel = ext.selectLibrary({"macos", "debug", "arm64"});
    check(sel && sel->key == "macos.debug", "selectLibrary: a one-tag-pair key matches first in file order");
    check(ext.selectLibrary({"linux", "release", "x86_64"}) == nullptr, "selectLibrary: nothing for linux release");
    check(ext.icons.size() == 1 && ext.icons[0].first == "MyNode", "[icons]");
    check(ext.dependencies.size() == 1 && ext.dependencies[0].key == "windows.x86_64" && ext.dependencies[0].files.size() == 1 &&
              ext.dependencies[0].files[0].first == "res://addons/mything/bin/helper.dll",
          "[dependencies] dictionary");
    GodotGdExtension badExt;
    GodotDiagnostics d4;
    check(!parseGodotGdExtension("[configuration]\ncompatibility_minimum=\"4.1\"\n", "x.gdextension", badExt, d4) && hasDiag(d4, GodotSeverity::Error, "godot.gdextension", "entry_symbol") &&
              hasDiag(d4, GodotSeverity::Error, "godot.gdextension", "no library"),
          "a .gdextension needs entry_symbol and libraries");
}

static void testProjectFs() {
    AVER_INFO("=== res:// and uid:// resolution ===");
    check(GodotProjectFs::normalizeRes("res://a/./b/../c.png") == "res://a/c.png", "normalizeRes collapses . and ..");
    check(GodotProjectFs::normalizeRes("res://a\\b.png") == "res://a/b.png" && GodotProjectFs::normalizeRes("res://") == "res://", "backslashes, and the root itself");
    check(GodotProjectFs::normalizeRes("res://../x").empty() && GodotProjectFs::normalizeRes("user://x").empty() && GodotProjectFs::normalizeRes("a/b").empty(),
          "normalizeRes rejects escaping the root, user://, and non-res paths");

    GodotProjectFs fs;
    GodotDiagnostics d;
    check(fs.open(kData + "/MiniGame", d) && d.items.empty(), "open the project folder");
    check(fs.scan(d), "scan");
    dump(d);
    check(fs.files().size() >= 10, "files() lists the project (sorted res:// paths)");
    bool sorted = true;
    for (usize i = 1; i < fs.files().size(); ++i) sorted = sorted && fs.files()[i - 1] <= fs.files()[i];
    check(sorted, "files() is sorted");
    check(fs.binaryResources() == std::vector<std::string>({"res://binary/level.scn"}), "the binary .scn is listed for a text resave");
    check(hasDiag(d, GodotSeverity::Warning, "godot.binary_resource", "resave as text"), "and warned about");

    const GodotUidMap& u = fs.uids();
    auto uidIs = [&](const char* uid, const char* res) { const std::string* p = u.find(uid); return p && *p == res; };
    check(uidIs("uid://b6x2mainscene", "res://main.tscn"), "uid from a .tscn header");
    check(uidIs("uid://c1crate00000", "res://props/crate.tscn"), "uid from another .tscn header");
    check(uidIs("uid://c2redmat0000", "res://materials/red.tres"), "uid from a .tres header");
    check(uidIs("uid://c4albedo0000", "res://textures/albedo.png"), "uid from a .import maps to the source file");
    check(uidIs("uid://c3playerscr0", "res://scripts/player.gd"), "uid from a .gd.uid sidecar");
    check(uidIs("uid://c5variants00", "res://variants.tres"), "uid from variants.tres");
    check(u.size() >= 7 && u.find("uid://nothing") == nullptr, "an unknown uid is absent");

    check(fs.resolve("uid://c1crate00000", "res://props/crate.tscn") == "res://props/crate.tscn", "resolve: uid and path agree");
    GodotDiagnostics dm;
    check(fs.resolve("uid://c1crate00000", "res://old/crate_moved.tscn", &dm, "main.tscn", 3) == "res://props/crate.tscn" && hasDiag(dm, GodotSeverity::Info, "godot.uid_moved"),
          "resolve: the uid wins over a stale path (and says so)");
    check(fs.resolve("uid://unknown", "res://props/./crate.tscn") == "res://props/crate.tscn", "resolve: an unknown uid falls back to the path");
    check(fs.resolve("", "uid://b6x2mainscene") == "res://main.tscn", "resolve: a uid:// in the path slot");
    check(fs.resolve("", "").empty(), "resolve: nothing in, nothing out");
    GodotScene mainScene;
    GodotDiagnostics ds;
    check(fs.loadScene("res://main.tscn", mainScene, ds) && ds.errorCount() == 0, "loadScene by res:// path");
    check(mainScene.file == "res://main.tscn" && mainScene.nodes.size() == 10, "the document is named by its res:// path");
    bool allResolve = true;
    for (const GodotExtResource& e : mainScene.extResources) allResolve = allResolve && !fs.resolve(e).empty() && fs.exists(fs.resolve(e));
    check(allResolve, "every ext resource of main.tscn resolves to a file that exists");
    GodotScene byUid;
    GodotDiagnostics du;
    check(fs.loadScene("uid://c1crate00000", byUid, du) && byUid.file == "res://props/crate.tscn" && byUid.nodes.size() == 3, "loadScene by uid://");
    GodotResource red;
    GodotDiagnostics dr;
    check(fs.loadResource("res://materials/red.tres", red, dr) && red.header.type == "StandardMaterial3D", "loadResource");
    GodotImport imp;
    GodotDiagnostics di;
    check(fs.loadImport("res://textures/albedo.png", imp, di) && imp.importer == "texture", "loadImport reads <file>.import");
    GodotScene lost;
    GodotDiagnostics dl;
    check(!fs.loadScene("uid://not-a-uid", lost, dl) && hasDiag(dl, GodotSeverity::Error, "godot.unresolved"), "an unresolvable reference is an error, not a crash");
    GodotScene bin;
    GodotDiagnostics db;
    check(!fs.loadScene("res://binary/level.scn", bin, db) && hasDiag(db, GodotSeverity::Error, "godot.binary_resource"), "loadScene on the binary scene reports 'resave as text'");

    const std::string disk = fs.toDiskPath("res://props/crate.tscn");
    check(!disk.empty() && disk.find("props/crate.tscn") != std::string::npos, "toDiskPath");
    check(fs.toResPath(disk) == "res://props/crate.tscn", "toResPath is the inverse");
    check(fs.toResPath("C:/definitely/elsewhere/x.png").empty() && fs.toDiskPath("user://save.dat").empty(), "outside the project, and user://, do not resolve");
    check(fs.toDiskPath("uid://c4albedo0000").find("textures/albedo.png") != std::string::npos, "toDiskPath through a uid");

    GodotProjectFs nope;
    GodotDiagnostics dn;
    check(!nope.open(kData + "/DoesNotExist", dn) && dn.errorCount() == 1, "opening a missing folder is an error");
}

int main() {
    testScalars();
    testMathTypes();
    testVariantErrors();
    testVariantsFixture();
    testMainScene();
    testInstancedScene();
    testSceneErrors();
    testBinary();
    testProject();
    testImportAndExtension();
    testProjectFs();

    AVER_INFO("==================================================");
    AVER_INFO("FormatsGodotTest done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
