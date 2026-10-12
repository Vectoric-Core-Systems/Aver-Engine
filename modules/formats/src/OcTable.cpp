// Data-table reader and writer (.csv, table-shaped .json). See OcTable.hpp and docs/DATA_TABLES.md.
#include "aver/formats/OcTable.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/formats/Json.hpp"
#include "aver/formats/detail/Csv.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cstdio>
#include <unordered_map>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

char lowerC(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerStr(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = lowerC(c);
    return o;
}

// Column match key: lowercase with spaces and underscores dropped ("Display Name" == "display_name").
std::string columnKey(std::string_view s) {
    std::string o;
    for (const char c : s) if (c != ' ' && c != '_') o.push_back(lowerC(c));
    return o;
}

bool parseInt64(std::string_view s, i64& out) {
    s = trim(s);
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    i64 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

// A trailing 'f' is accepted (1.5f); nan/inf are not numbers here.
bool parseNumber(std::string_view s, f64& out) {
    s = trim(s);
    if (!s.empty() && (s.back() == 'f' || s.back() == 'F') && s.size() > 1) s.remove_suffix(1);
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    const char c0 = s[0];
    if (!((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '.')) return false;
    f64 v{};
    const auto r = aver::fromChars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

bool parseBoolText(std::string_view s, bool& out) {
    s = trim(s);
    const std::string l = lowerStr(s);
    if (l == "true" || l == "1" || l == "yes") { out = true; return true; }
    if (l == "false" || l == "0" || l == "no") { out = false; return true; }
    return false;
}

bool parseVec3Text(std::string_view s) {
    std::string t(s);
    for (char& c : t) if (c == ',' || c == ';' || c == '|') c = ' ';
    const std::vector<std::string_view> p = splitWhitespace(t);
    if (p.size() != 3) return false;
    f64 v;
    for (const std::string_view x : p) if (!parseNumber(x, v)) return false;
    return true;
}

std::string numText(f64 v) {
    char buf[64];
    if (v > -1e15 && v < 1e15 && v == static_cast<f64>(static_cast<i64>(v))) {
        const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<i64>(v));
        return std::string(buf, r.ptr);
    }
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

std::string jsonQuote(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<u8>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<u8>(c)));
                    o += b;
                } else {
                    o.push_back(c);
                }
        }
    }
    o.push_back('"');
    return o;
}

// Row-name uniqueness is ASCII case-insensitive (UE FName parity).
struct RowIndex {
    std::unordered_map<std::string, std::string> seen;   // lowercase -> first spelling
    // Returns the first spelling when `name` repeats one, else empty.
    std::string add(const std::string& name) {
        const auto [it, fresh] = seen.emplace(lowerStr(name), name);
        return fresh ? std::string() : it->second;
    }
};

} // namespace

// ---- vocabulary ---------------------------------------------------------------------------------

const char* tableTypeWord(OcTableType t) {
    switch (t) {
        case OcTableType::Int: return "int";
        case OcTableType::Float: return "float";
        case OcTableType::Bool: return "bool";
        case OcTableType::Vec3: return "vec3";
        case OcTableType::Text: return "text";
        case OcTableType::Strings: return "strings";
        case OcTableType::String: default: return "string";
    }
}

bool parseTableTypeWord(std::string_view s, OcTableType& out) {
    static const OcTableType kAll[] = {OcTableType::String, OcTableType::Int, OcTableType::Float, OcTableType::Bool,
                                       OcTableType::Vec3, OcTableType::Text, OcTableType::Strings};
    const std::string l = lowerStr(trim(s));
    for (const OcTableType t : kAll) if (l == tableTypeWord(t)) { out = t; return true; }
    return false;
}

i32 OcTableData::findRow(std::string_view name) const {
    const std::string k = lowerStr(name);
    for (usize i = 0; i < rowNames.size(); ++i) if (lowerStr(rowNames[i]) == k) return static_cast<i32>(i);
    return -1;
}

i32 OcTableData::findColumn(std::string_view name) const {
    const std::string k = columnKey(name);
    for (usize i = 0; i < columns.size(); ++i) if (columnKey(columns[i].name) == k) return static_cast<i32>(i);
    return -1;
}

std::string_view OcTableData::cell(usize r, usize c) const {
    if (r >= rowNames.size() || c >= columns.size()) return {};
    const usize i = r * columns.size() + c;
    return i < cells.size() ? std::string_view(cells[i]) : std::string_view();
}

// ---- CSV ------------------------------------------------------------------------------------------

bool parseTableCsv(std::string_view text, OcTableData& out, std::string* err) {
    text = csvStripBom(text);
    const char delim = csvDetectDelimiter(text);
    std::vector<CsvRecord> recs;
    if (!csvParse(text, delim, recs, err)) return false;
    if (recs.empty()) { if (err) *err = "empty table: expected a header row"; return false; }

    OcTableData d;
    d.source = OcTableData::Source::Csv;
    d.delimiter = delim;
    d.crlf = csvUsesCrlf(text);
    const CsvRecord& head = recs[0];
    d.rowNameHeader = std::string(trim(head.cells[0]));
    if (d.rowNameHeader.empty()) d.rowNameHeader = "Name";
    for (usize i = 1; i < head.cells.size(); ++i) {
        OcTableColumn c;
        std::string_view name = trim(head.cells[i]);
        const usize colon = name.rfind(':');
        if (colon != std::string_view::npos) {
            OcTableType t;
            if (parseTableTypeWord(name.substr(colon + 1), t)) {
                c.type = t;
                c.hinted = true;
                name = trim(name.substr(0, colon));
            }
        }
        if (name.empty()) {
            if (err) *err = "line " + std::to_string(head.line) + ": column " + std::to_string(i + 1) + " has no name";
            return false;
        }
        c.name = std::string(name);
        d.columns.push_back(std::move(c));
    }

    RowIndex rows;
    for (usize r = 1; r < recs.size(); ++r) {
        const CsvRecord& rec = recs[r];
        bool allEmpty = true;
        for (const std::string& c : rec.cells) if (!trim(c).empty()) { allEmpty = false; break; }
        if (allEmpty) continue;   // a spreadsheet's trailing ",,," line
        if (rec.cells.size() != head.cells.size()) {
            if (err) *err = "line " + std::to_string(rec.line) + ": expected " + std::to_string(head.cells.size()) +
                            " cells, got " + std::to_string(rec.cells.size());
            return false;
        }
        const std::string name(trim(rec.cells[0]));
        if (name.empty()) { if (err) *err = "line " + std::to_string(rec.line) + ": empty row name"; return false; }
        const std::string dup = rows.add(name);
        if (!dup.empty()) {
            if (err) *err = "line " + std::to_string(rec.line) + ": row name '" + name + "' repeats '" + dup + "' (row names are case-insensitive)";
            return false;
        }
        d.rowNames.push_back(name);
        for (usize c = 1; c < rec.cells.size(); ++c) d.cells.push_back(rec.cells[c]);
    }
    out = std::move(d);
    return true;
}

std::string writeTableCsv(const OcTableData& d) {
    const char delim = d.delimiter == ';' ? ';' : ',';
    const char* eol = d.crlf ? "\r\n" : "\n";
    std::string s;
    csvWriteField(s, d.rowNameHeader.empty() ? std::string_view("Name") : std::string_view(d.rowNameHeader), delim);
    for (const OcTableColumn& c : d.columns) {
        s.push_back(delim);
        std::string h = c.name;
        if (c.hinted) { h.push_back(':'); h += tableTypeWord(c.type); }
        csvWriteField(s, h, delim);
    }
    s += eol;
    const usize nc = d.columns.size();
    for (usize r = 0; r < d.rowNames.size(); ++r) {
        csvWriteField(s, d.rowNames[r], delim);
        for (usize c = 0; c < nc; ++c) {
            s.push_back(delim);
            csvWriteField(s, d.cell(r, c), delim);
        }
        s += eol;
    }
    return s;
}

// ---- JSON -----------------------------------------------------------------------------------------

namespace {

// One cell's text from a JSON value; false (why set) for a nested object/array.
bool jsonCellText(const JsonValue& v, const std::string& column, std::string& out, std::string& why) {
    switch (v.type()) {
        case JsonValue::Type::Null: out.clear(); return true;
        case JsonValue::Type::Bool: out = v.asBool() ? "true" : "false"; return true;
        case JsonValue::Type::Number: out = numText(v.asDouble()); return true;
        case JsonValue::Type::String: out = std::string(v.asString()); return true;
        case JsonValue::Type::Array: {
            out.clear();
            bool first = true;
            for (const JsonValue& e : v.elements()) {
                std::string t;
                if (e.isArray() || e.isObject()) { why = "column '" + column + "' holds a nested value; flatten it"; return false; }
                if (!jsonCellText(e, column, t, why)) return false;
                if (!first) out.push_back('|');
                first = false;
                out += t;
            }
            return true;
        }
        case JsonValue::Type::Object: why = "column '" + column + "' holds a nested object; flatten it"; return false;
    }
    return false;
}

OcTableType inferType(const JsonValue& v) {
    switch (v.type()) {
        case JsonValue::Type::Bool: return OcTableType::Bool;
        case JsonValue::Type::Number: {
            const f64 x = v.asDouble();
            return (x > -1e15 && x < 1e15 && x == static_cast<f64>(static_cast<i64>(x))) ? OcTableType::Int : OcTableType::Float;
        }
        case JsonValue::Type::Array: return OcTableType::Strings;
        default: return OcTableType::String;
    }
}

} // namespace

bool parseTableJson(std::string_view text, OcTableData& out, std::string* err) {
    JsonValue root;
    std::string why;
    if (!parseJson(text, root, &why)) { if (err) *err = "JSON: " + why; return false; }
    const auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    struct Raw { std::string name; const JsonValue* obj; };
    std::vector<Raw> rows;
    OcTableData d;
    d.source = OcTableData::Source::Json;
    std::string rowKey;   // the member holding the row name (array form)

    if (root.isArray()) {
        static const char* kNameKeys[] = {"Name", "RowName", "name"};
        for (usize i = 0; i < root.size(); ++i) {
            const JsonValue& e = root[i];
            if (!e.isObject()) return fail("not a data table: element " + std::to_string(i) + " is not an object");
            const JsonValue* nv = nullptr;
            std::string key;
            if (!rowKey.empty() && e.has(rowKey)) { nv = &e[rowKey]; key = rowKey; }
            else for (const char* k : kNameKeys) if (e.has(k)) { nv = &e[k]; key = k; break; }
            if (!nv) return fail("element " + std::to_string(i) + " has no Name, RowName or name member");
            if (rowKey.empty()) rowKey = key;
            if (!nv->isString() && !nv->isNumber()) return fail("element " + std::to_string(i) + ": the row name must be a string");
            rows.push_back({nv->isString() ? std::string(nv->asString()) : numText(nv->asDouble()), &e});
        }
        if (!rowKey.empty()) d.rowNameHeader = rowKey;
    } else if (root.isObject()) {
        if (root.size() == 0) return fail("not a data table: the JSON object is empty");
        d.jsonObjectOfObjects = true;
        for (const JsonMember& m : root.members()) {
            if (!m.value.isObject())
                return fail("not a data table: member '" + m.key + "' is not an object (expected an array of objects or an object of objects)");
            rows.push_back({m.key, &m.value});
        }
    } else {
        return fail("not a data table: the JSON root must be an array of objects or an object of objects");
    }

    // Columns: union of member names, first-seen order; the type comes from the first non-null value.
    std::unordered_map<std::string, usize> colIndex;
    std::vector<bool> typed;
    for (const Raw& r : rows) {
        for (const JsonMember& m : r.obj->members()) {
            if (!d.jsonObjectOfObjects && m.key == rowKey) continue;
            auto it = colIndex.find(m.key);
            if (it == colIndex.end()) {
                it = colIndex.emplace(m.key, d.columns.size()).first;
                OcTableColumn c;
                c.name = m.key;
                d.columns.push_back(std::move(c));
                typed.push_back(false);
            }
            if (!typed[it->second] && !m.value.isNull()) {
                d.columns[it->second].type = inferType(m.value);
                d.columns[it->second].hinted = true;
                typed[it->second] = true;
            }
        }
    }

    RowIndex seen;
    const usize nc = d.columns.size();
    d.cells.assign(rows.size() * nc, std::string());
    for (usize ri = 0; ri < rows.size(); ++ri) {
        const Raw& r = rows[ri];
        if (r.name.empty()) return fail("row " + std::to_string(ri) + " has an empty name");
        const std::string dup = seen.add(r.name);
        if (!dup.empty()) return fail("row name '" + r.name + "' repeats '" + dup + "' (row names are case-insensitive)");
        d.rowNames.push_back(r.name);
        for (const JsonMember& m : r.obj->members()) {
            if (!d.jsonObjectOfObjects && m.key == rowKey) continue;
            std::string t, w;
            if (!jsonCellText(m.value, m.key, t, w)) return fail("row '" + r.name + "': " + w);
            d.cells[ri * nc + colIndex[m.key]] = std::move(t);
        }
    }
    out = std::move(d);
    return true;
}

namespace {

// The JSON value for one cell, honouring the column's (hinted) type.
std::string jsonValue(const OcTableColumn& col, std::string_view cell) {
    if (col.hinted) {
        switch (col.type) {
            case OcTableType::Int: {
                i64 iv;
                if (cell.empty()) return "null";
                if (parseInt64(cell, iv)) return std::to_string(iv);
                f64 fv;
                if (parseNumber(cell, fv) && cell.back() != 'f' && cell.back() != 'F') return numText(fv);
                break;
            }
            case OcTableType::Float: {
                f64 fv;
                if (cell.empty()) return "null";
                if (parseNumber(cell, fv) && cell.back() != 'f' && cell.back() != 'F') return numText(fv);
                break;
            }
            case OcTableType::Bool: {
                bool bv;
                if (cell.empty()) return "null";
                if (parseBoolText(cell, bv)) return bv ? "true" : "false";
                break;
            }
            case OcTableType::Strings: {
                std::string o = "[";
                if (!cell.empty()) {
                    bool first = true;
                    for (const std::string_view p : splitChar(cell, '|')) {
                        if (!first) o += ", ";
                        first = false;
                        o += jsonQuote(p);
                    }
                }
                return o + "]";
            }
            default: break;
        }
    }
    return jsonQuote(cell);
}

} // namespace

std::string writeTableJson(const OcTableData& d) {
    const usize nc = d.columns.size();
    std::string s;
    const auto members = [&](usize r, const char* indent, bool withName) {
        std::string m;
        bool first = true;
        const auto sep = [&] { if (!first) m += ",\n"; first = false; };
        if (withName) {
            sep();
            m += indent + jsonQuote(d.rowNameHeader.empty() ? std::string_view("Name") : std::string_view(d.rowNameHeader)) + ": " + jsonQuote(d.rowNames[r]);
        }
        for (usize c = 0; c < nc; ++c) {
            sep();
            m += indent + jsonQuote(d.columns[c].name) + ": " + jsonValue(d.columns[c], d.cell(r, c));
        }
        return m;
    };
    if (d.jsonObjectOfObjects) {
        s = "{\n";
        for (usize r = 0; r < d.rowNames.size(); ++r) {
            s += "  " + jsonQuote(d.rowNames[r]) + ": {";
            if (nc) s += "\n" + members(r, "    ", false) + "\n  ";
            s += r + 1 < d.rowNames.size() ? "},\n" : "}\n";
        }
        s += "}\n";
    } else {
        s = "[\n";
        for (usize r = 0; r < d.rowNames.size(); ++r) {
            s += "  {\n" + members(r, "    ", true) + "\n  ";
            s += r + 1 < d.rowNames.size() ? "},\n" : "}\n";
        }
        s += "]\n";
    }
    return s;
}

// ---- files ----------------------------------------------------------------------------------------

namespace {
std::string extOf(const std::string& path) {
    const usize dot = path.rfind('.');
    const usize slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lowerStr(std::string_view(path).substr(dot));
}
} // namespace

bool loadTable(const std::string& path, OcTableData& out, std::string* err) {
    const std::string ext = extOf(path);
    if (ext != ".csv" && ext != ".json") { if (err) *err = "unsupported data table extension '" + ext + "' (use .csv or .json)"; return false; }
    std::string text;
    if (!readFileText(path, text)) { if (err) *err = "cannot read " + path; return false; }
    return ext == ".json" ? parseTableJson(text, out, err) : parseTableCsv(text, out, err);
}

bool saveTable(const std::string& path, const OcTableData& d, std::string* err) {
    const std::string text = extOf(path) == ".json" ? writeTableJson(d) : writeTableCsv(d);
    if (!writeFileTextAtomic(path, text)) { if (err) *err = "cannot write " + path; return false; }
    return true;
}

// ---- validation -----------------------------------------------------------------------------------

std::string cellProblem(const OcTableData& d, usize r, usize c) {
    if (c >= d.columns.size() || r >= d.rowNames.size()) return {};
    const OcTableColumn& col = d.columns[c];
    if (!col.hinted) return {};
    const std::string_view cell = d.cell(r, c);
    if (trim(cell).empty()) return {};
    i64 iv;
    f64 fv;
    bool bv;
    switch (col.type) {
        case OcTableType::Int:   if (!parseInt64(cell, iv)) return "an int"; break;
        case OcTableType::Float: if (!parseNumber(cell, fv)) return "a float"; break;
        case OcTableType::Bool:  if (!parseBoolText(cell, bv)) return "a bool (true/false/1/0/yes/no)"; break;
        case OcTableType::Vec3:  if (!parseVec3Text(cell)) return "a vec3 (three numbers)"; break;
        default: break;
    }
    return {};
}

std::vector<std::string> validateTable(const OcTableData& d) {
    std::vector<std::string> out;
    std::unordered_map<std::string, std::string> cols;
    for (usize c = 0; c < d.columns.size(); ++c) {
        const std::string& n = d.columns[c].name;
        if (trim(n).empty()) { out.push_back("column " + std::to_string(c + 1) + " has no name"); continue; }
        const auto [it, fresh] = cols.emplace(columnKey(n), n);
        if (!fresh) out.push_back("column '" + n + "' duplicates column '" + it->second + "'");
    }
    RowIndex rows;
    const usize nc = d.columns.size();
    for (usize r = 0; r < d.rowNames.size(); ++r) {
        const std::string& rn = d.rowNames[r];
        if (trim(rn).empty()) out.push_back("row " + std::to_string(r + 1) + ": empty name");
        else {
            const std::string dup = rows.add(rn);
            if (!dup.empty()) out.push_back("row '" + rn + "' duplicates row '" + dup + "'");
        }
        for (usize c = 0; c < nc; ++c) {
            const std::string bad = cellProblem(d, r, c);
            if (!bad.empty())
                out.push_back("row '" + rn + "' column '" + d.columns[c].name + "': '" + std::string(d.cell(r, c)) + "' is not " + bad);
        }
    }
    return out;
}

} // namespace aver::fmt
