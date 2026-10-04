// The JSON reader the glTF importer stands on: scalars, escapes, containers, nesting limits and
// the malformed input it must refuse.
#include "aver/formats/Json.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Parses text that must be accepted, and returns the value for the caller to assert on.
static fmt::JsonValue mustParse(const std::string& text, const std::string& what) {
    fmt::JsonValue v;
    std::string why;
    const bool ok = fmt::parseJson(text, v, &why);
    check(ok, what + (ok ? "" : "  -> " + why));
    return v;
}

// Asserts that text is rejected.
static void mustReject(const std::string& text, const std::string& what) {
    fmt::JsonValue v;
    std::string why;
    check(!fmt::parseJson(text, v, &why), "rejects " + what);
}

// Runs every JSON test. Returns 0 when they all pass.
int main() {
    AVER_INFO("=== scalars ===");
    {
        check(mustParse("true",  "true").asBool() == true,   "true");
        check(mustParse("false", "false").asBool(true) == false, "false");
        check(mustParse("null",  "null").isNull(),           "null");
        check(mustParse("0", "zero").asInt() == 0,           "0");
        check(mustParse("-17", "negative").asInt() == -17,   "-17");
        check(std::fabs(mustParse("3.5", "fraction").asDouble() - 3.5) < 1e-12, "3.5");
        check(std::fabs(mustParse("1e3", "exponent").asDouble() - 1000.0) < 1e-9, "1e3");
        check(std::fabs(mustParse("-2.5e-3", "signed exponent").asDouble() + 0.0025) < 1e-12, "-2.5e-3");
        check(mustParse("16777217", "large int").asInt() == 16777217, "16777217 survives (a float would not)");
    }

    AVER_INFO("=== strings and escapes ===");
    {
        check(mustParse("\"hello\"", "plain").asString() == "hello", "plain string");
        check(mustParse("\"\"", "empty").asString().empty(), "empty string");
        check(mustParse("\"a\\\"b\"", "escaped quote").asString() == "a\"b", "escaped quote");
        check(mustParse("\"a\\\\b\"", "escaped backslash").asString() == "a\\b", "escaped backslash");
        check(mustParse("\"a\\nb\"", "newline").asString() == "a\nb", "\\n");
        check(mustParse("\"a\\/b\"", "solidus").asString() == "a/b", "escaped solidus");
        check(mustParse("\"\\u0041\"", "ascii escape").asString() == "A", "\\u0041 is A");
        check(mustParse("\"\\u00e9\"", "latin1 escape").asString() == "\xc3\xa9", "\\u00e9 encodes as two UTF-8 bytes");
        check(mustParse("\"\\ud83d\\ude00\"", "surrogate pair").asString() == "\xf0\x9f\x98\x80",
              "a surrogate pair decodes to one 4-byte code point");
    }

    AVER_INFO("=== arrays and objects ===");
    {
        const fmt::JsonValue a = mustParse("[1,2,3]", "array");
        check(a.isArray() && a.size() == 3, "array of three");
        check(a[1].asInt() == 2, "indexing");
        check(a[99].isNull(), "out-of-range index gives null rather than crashing");

        const fmt::JsonValue o = mustParse("{\"a\":1,\"b\":\"two\"}", "object");
        check(o.isObject() && o.size() == 2, "object with two members");
        check(o["a"].asInt() == 1, "member by name");
        check(o["b"].asString() == "two", "string member");
        check(o["missing"].isNull(), "absent member gives null");
        check(!o.has("missing") && o.has("a"), "has() agrees");

        check(mustParse("[]", "empty array").size() == 0, "empty array");
        check(mustParse("{}", "empty object").size() == 0, "empty object");
        check(mustParse("  { \"a\" : [ 1 , 2 ] } ", "whitespace").isObject(), "whitespace everywhere legal");

        const fmt::JsonValue doc = mustParse("{\"meshes\":[{\"name\":\"Cube\"}]}", "gltf-shaped");
        check(doc["meshes"][0]["name"].asString() == "Cube", "chained access reads");
        check(doc["nope"][3]["deep"].asInt(-1) == -1, "chained access on absent keys yields the fallback");

        check(mustParse("{\"k\":1,\"k\":2}", "duplicate key")["k"].asInt() == 2, "duplicate key: last wins");
    }

    AVER_INFO("=== nesting ===");
    {
        std::string deep;
        for (int i = 0; i < 40; ++i) deep += "[";
        deep += "1";
        for (int i = 0; i < 40; ++i) deep += "]";
        check(mustParse(deep, "40 deep").isArray(), "40 levels is fine");

        std::string tooDeep;
        for (int i = 0; i < 200; ++i) tooDeep += "[";
        for (int i = 0; i < 200; ++i) tooDeep += "]";
        mustReject(tooDeep, "nesting past the depth limit (would otherwise be a stack overflow)");
    }

    AVER_INFO("=== malformed input is refused ===");
    {
        mustReject("",            "an empty document");
        mustReject("{",           "an unterminated object");
        mustReject("[1,2",        "an unterminated array");
        mustReject("[1,2,]",      "a trailing comma in an array");
        mustReject("{\"a\":1,}",  "a trailing comma in an object");
        mustReject("{a:1}",       "an unquoted member name");
        mustReject("{\"a\" 1}",   "a missing colon");
        mustReject("'single'",    "single quotes");
        mustReject("01",          "a leading zero");
        mustReject("+1",          "a leading plus");
        mustReject(".5",          "a bare leading decimal point");
        mustReject("1.",          "a trailing decimal point");
        mustReject("1e",          "an empty exponent");
        mustReject("tru",         "a truncated literal");
        mustReject("\"unterminated", "an unterminated string");
        mustReject("\"a\\qb\"",   "an unknown escape");
        mustReject("\"\\ud83d\"", "an unpaired high surrogate");
        mustReject("\"\\udc00\"", "an unpaired low surrogate");
        mustReject("{} {}",       "trailing content after the top-level value");
        mustReject("[1] junk",    "trailing junk");
        mustReject(std::string("\"a\x01" "b\""), "a raw control character in a string");
    }

    AVER_INFO("=== a UTF-8 BOM is tolerated ===");
    {
        const std::string bom = "\xEF\xBB\xBF{\"a\":1}";
        check(mustParse(bom, "BOM-prefixed document")["a"].asInt() == 1, "a leading BOM is skipped");
    }

    if (g_failures == 0) AVER_INFO("=== all JSON tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
