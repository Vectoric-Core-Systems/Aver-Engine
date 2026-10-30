// Godot shading language: the preprocessor. #define (object- and function-like, with ## pasting), #undef, #if / #ifdef / #ifndef / #elif / #else /
// #endif with defined(), #include through a resolver callback, #pragma, #error. Comments are removed first (string literals in hint arguments are
// respected). The output keeps one line per origin line and a line map back to (file, line), so later diagnostics name the real place.
#include "aver/formats/godot/GodotShader.hpp"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>

namespace aver::fmt::godot {
namespace {

bool isIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replaces // and /* */ comments; a block comment becomes one space and keeps its newlines. CR is dropped (CRLF checkouts read as LF).
std::string stripComments(std::string_view t) {
    std::string out;
    out.reserve(t.size());
    usize start = 0;
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB && static_cast<unsigned char>(t[2]) == 0xBF) start = 3;   // UTF-8 BOM
    for (usize i = start; i < t.size();) {
        const char c = t[i];
        if (c == '\r') { ++i; continue; }
        if (c == '"') {
            out += c;
            ++i;
            while (i < t.size() && t[i] != '"' && t[i] != '\n') out += t[i++];
            if (i < t.size() && t[i] == '"') out += t[i++];
            continue;
        }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            out += ' ';
            while (i < t.size() && !(t[i] == '*' && i + 1 < t.size() && t[i + 1] == '/')) {
                if (t[i] == '\n') out += '\n';
                ++i;
            }
            i = i + 2 <= t.size() ? i + 2 : t.size();
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string trim(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

struct Macro {
    bool funcLike = false;
    std::vector<std::string> params;
    std::string body;
};

class Pre {
public:
    Pre(const GdPreprocessOptions& opt, GdPreprocessed& out, std::vector<GdShaderDiag>& diags) : opt_(opt), out_(out), diags_(diags) {
        for (const auto& [n, v] : opt.defines) {
            Macro m;
            m.body = v;
            macros_[n] = m;
        }
    }

    bool run(std::string_view source, std::string_view fileName) {
        out_.files.push_back(std::string(fileName));
        file(source, 0, 0);
        if (!out_.text.empty() && out_.text.back() == '\n') out_.text.pop_back();
        return !failed_;
    }

private:
    const GdPreprocessOptions& opt_;
    GdPreprocessed& out_;
    std::vector<GdShaderDiag>& diags_;
    std::map<std::string, Macro> macros_;
    bool failed_ = false;
    bool aborted_ = false;     // a hostile-input budget ran out: every loop unwinds
    static constexpr usize kMaxOutput = usize{1} << 25;     // output bytes (macro bombs)
    static constexpr usize kMaxFiles = 4096;                // files pulled in by #include (also keeps GdLineOrigin::file in a u16)
    static constexpr usize kMaxDiags = 1000;

    void abort(int fileIdx, int line, const std::string& msg) {
        if (aborted_) return;
        aborted_ = true;
        failed_ = true;
        GdShaderDiag d;
        d.severity = GodotSeverity::Error;
        d.file = out_.files[static_cast<usize>(fileIdx)];
        d.line = line;
        d.column = 1;
        d.code = "gdshader.preprocess";
        d.message = msg;
        diags_.push_back(std::move(d));
    }

    void diag(GodotSeverity sev, int fileIdx, int line, int col, const std::string& msg) {
        if (aborted_) return;
        GdShaderDiag d;
        d.severity = sev;
        d.file = out_.files[static_cast<usize>(fileIdx)];
        d.line = line;
        d.column = col;
        d.code = "gdshader.preprocess";
        d.message = msg;
        diags_.push_back(std::move(d));
        if (sev == GodotSeverity::Error) failed_ = true;
        if (diags_.size() >= kMaxDiags) abort(fileIdx, line, "too many diagnostics");
    }

    void emit(const std::string& text, int fileIdx, int line) {
        if (aborted_) return;
        out_.text += text;
        out_.text += '\n';
        out_.lines.push_back({static_cast<u16>(fileIdx), line});
        if (out_.text.size() > kMaxOutput) abort(fileIdx, line, "preprocessed output is too large");
    }

    // True when a function-like macro name in `line` is followed by an argument list that does not close on this line.
    bool openCall(const std::string& s) const {
        for (usize i = 0; i < s.size();) {
            if (isIdStart(s[i])) {
                usize j = i;
                while (j < s.size() && isIdChar(s[j])) ++j;
                auto it = macros_.find(s.substr(i, j - i));
                i = j;
                if (it == macros_.end() || !it->second.funcLike) continue;
                usize k = j;
                while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k >= s.size() || s[k] != '(') continue;
                int depth = 0;
                for (; k < s.size(); ++k) {
                    if (s[k] == '(') ++depth;
                    else if (s[k] == ')' && --depth == 0) break;
                }
                if (depth > 0) return true;
                i = k;
            } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
                while (i < s.size() && (isIdChar(s[i]) || s[i] == '.')) ++i;
            } else ++i;
        }
        return false;
    }

    std::string expand(const std::string& s, std::set<std::string>& hide, int depth, int fileIdx, int line) {
        if (depth > 64) { diag(GodotSeverity::Error, fileIdx, line, 1, "macro expansion is nested too deeply"); return s; }
        std::string out;
        for (usize i = 0; i < s.size();) {
            if (out.size() > (1u << 20)) { diag(GodotSeverity::Error, fileIdx, line, 1, "macro expansion is too large"); return out; }
            if (std::isdigit(static_cast<unsigned char>(s[i]))) {
                while (i < s.size() && (isIdChar(s[i]) || s[i] == '.')) out += s[i++];
                continue;
            }
            if (!isIdStart(s[i])) { out += s[i++]; continue; }
            usize j = i;
            while (j < s.size() && isIdChar(s[j])) ++j;
            const std::string name = s.substr(i, j - i);
            auto it = macros_.find(name);
            if (it == macros_.end() || hide.count(name)) { out += name; i = j; continue; }
            const Macro& m = it->second;
            if (!m.funcLike) {
                hide.insert(name);
                out += expand(m.body, hide, depth + 1, fileIdx, line);
                hide.erase(name);
                i = j;
                continue;
            }
            usize k = j;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k >= s.size() || s[k] != '(') { out += name; i = j; continue; }
            // arguments
            std::vector<std::string> args;
            std::string cur;
            int pd = 0;
            usize e = k + 1;
            bool closed = false;
            for (; e < s.size(); ++e) {
                const char c = s[e];
                if (c == '(') { ++pd; cur += c; }
                else if (c == ')') {
                    if (pd == 0) { closed = true; break; }
                    --pd;
                    cur += c;
                } else if (c == ',' && pd == 0) { args.push_back(trim(cur)); cur.clear(); }
                else cur += c;
            }
            if (!closed) { diag(GodotSeverity::Error, fileIdx, line, static_cast<int>(i) + 1, "unterminated argument list for macro '" + name + "'"); return out + s.substr(i); }
            if (!trim(cur).empty() || !args.empty()) args.push_back(trim(cur));
            if (args.size() != m.params.size()) {
                diag(GodotSeverity::Error, fileIdx, line, static_cast<int>(i) + 1,
                     "macro '" + name + "' takes " + std::to_string(m.params.size()) + " argument(s), " + std::to_string(args.size()) + " given");
                return out + s.substr(i);
            }
            for (std::string& a : args) a = expand(a, hide, depth + 1, fileIdx, line);
            // substitute parameters in the body
            std::string sub;
            for (usize b = 0; b < m.body.size();) {
                if (std::isdigit(static_cast<unsigned char>(m.body[b]))) {   // a number keeps its suffix letters (255u, 2.0f)
                    while (b < m.body.size() && (isIdChar(m.body[b]) || m.body[b] == '.')) sub += m.body[b++];
                } else if (isIdStart(m.body[b])) {
                    usize c = b;
                    while (c < m.body.size() && isIdChar(m.body[c])) ++c;
                    const std::string id = m.body.substr(b, c - b);
                    usize p = 0;
                    for (; p < m.params.size(); ++p) if (m.params[p] == id) break;
                    sub += p < m.params.size() ? args[p] : id;
                    b = c;
                } else sub += m.body[b++];
            }
            // token pasting
            for (usize p = sub.find("##"); p != std::string::npos; p = sub.find("##")) {
                usize l = p, r = p + 2;
                while (l > 0 && (sub[l - 1] == ' ' || sub[l - 1] == '\t')) --l;
                while (r < sub.size() && (sub[r] == ' ' || sub[r] == '\t')) ++r;
                sub.erase(l, r - l);
            }
            hide.insert(name);
            out += expand(sub, hide, depth + 1, fileIdx, line);
            hide.erase(name);
            i = e + 1;
        }
        return out;
    }

    // ---- #if expressions
    struct Tok { enum K { Num, Op, End } k = End; long long v = 0; std::string op; };
    std::vector<Tok> toks_;
    usize tp_ = 0;
    bool exprBad_ = false;
    int exprDepth_ = 0;

    void lex(const std::string& s) {
        toks_.clear();
        tp_ = 0;
        for (usize i = 0; i < s.size();) {
            const char c = s[i];
            if (c == ' ' || c == '\t') { ++i; continue; }
            Tok t;
            if (std::isdigit(static_cast<unsigned char>(c))) {
                usize j = i;
                while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])))) ++j;
                t.k = Tok::Num;
                t.v = std::strtoll(s.substr(i, j - i).c_str(), nullptr, 0);
                i = j;
            } else if (isIdStart(c)) {
                usize j = i;
                while (j < s.size() && isIdChar(s[j])) ++j;
                t.k = Tok::Num;     // an identifier that survived expansion is an undefined macro: 0
                t.v = 0;
                i = j;
            } else {
                static const char* ops[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+", "-", "*", "/", "%", "<", ">", "&", "^", "|", "!", "~", "(", ")", "?", ":"};
                bool found = false;
                for (const char* o : ops) {
                    const usize n = std::char_traits<char>::length(o);
                    if (s.compare(i, n, o) == 0) { t.k = Tok::Op; t.op = o; i += n; found = true; break; }
                }
                if (!found) { exprBad_ = true; ++i; continue; }
            }
            toks_.push_back(t);
        }
    }
    bool isOp(const char* o) const { return tp_ < toks_.size() && toks_[tp_].k == Tok::Op && toks_[tp_].op == o; }

    long long parseTernary() {
        if (++exprDepth_ > 200) { exprBad_ = true; --exprDepth_; return 0; }
        struct Dec { int& d; ~Dec() { --d; } } dec{exprDepth_};
        const long long c = parseBin(1);
        if (isOp("?")) {
            ++tp_;
            const long long a = parseTernary();
            if (!isOp(":")) { exprBad_ = true; return 0; }
            ++tp_;
            const long long b = parseTernary();
            return c ? a : b;
        }
        return c;
    }
    // Precedence of the operator at tp_ (higher binds tighter); 0 = none.
    int binPrec() const {
        if (tp_ >= toks_.size() || toks_[tp_].k != Tok::Op) return 0;
        const std::string& o = toks_[tp_].op;
        if (o == "||") return 1;
        if (o == "&&") return 2;
        if (o == "|") return 3;
        if (o == "^") return 4;
        if (o == "&") return 5;
        if (o == "==" || o == "!=") return 6;
        if (o == "<" || o == ">" || o == "<=" || o == ">=") return 7;
        if (o == "<<" || o == ">>") return 8;
        if (o == "+" || o == "-") return 9;
        if (o == "*" || o == "/" || o == "%") return 10;
        return 0;
    }

    // Precedence climbing, wrapping arithmetic (no signed-overflow UB on hostile input).
    long long parseBin(int minPrec) {
        long long l = parseUnary();
        for (;;) {
            const int prec = binPrec();
            if (prec == 0 || prec < minPrec) return l;
            const std::string o = toks_[tp_++].op;
            const long long r = parseBin(prec + 1);
            const unsigned long long ul = static_cast<unsigned long long>(l), ur = static_cast<unsigned long long>(r);
            if (o == "||") l = (l || r) ? 1 : 0;
            else if (o == "&&") l = (l && r) ? 1 : 0;
            else if (o == "|") l = l | r;
            else if (o == "^") l = l ^ r;
            else if (o == "&") l = l & r;
            else if (o == "==") l = l == r;
            else if (o == "!=") l = l != r;
            else if (o == "<") l = l < r;
            else if (o == ">") l = l > r;
            else if (o == "<=") l = l <= r;
            else if (o == ">=") l = l >= r;
            else if (o == "<<") l = static_cast<long long>(ul << (ur & 63));
            else if (o == ">>") l = l >> (r & 63);
            else if (o == "+") l = static_cast<long long>(ul + ur);
            else if (o == "-") l = static_cast<long long>(ul - ur);
            else if (o == "*") l = static_cast<long long>(ul * ur);
            else if (r == 0 || (l == std::numeric_limits<long long>::min() && r == -1)) { exprBad_ = true; return 0; }
            else if (o == "/") l = l / r;
            else l = l % r;
        }
    }
    long long parseUnary() {
        if (++exprDepth_ > 200) { exprBad_ = true; --exprDepth_; return 0; }
        struct Dec { int& d; ~Dec() { --d; } } dec{exprDepth_};
        if (isOp("!")) { ++tp_; return !parseUnary(); }
        if (isOp("-")) { ++tp_; return static_cast<long long>(0ull - static_cast<unsigned long long>(parseUnary())); }
        if (isOp("+")) { ++tp_; return parseUnary(); }
        if (isOp("~")) { ++tp_; return ~parseUnary(); }
        if (isOp("(")) {
            ++tp_;
            const long long v = parseTernary();
            if (!isOp(")")) { exprBad_ = true; return 0; }
            ++tp_;
            return v;
        }
        if (tp_ < toks_.size() && toks_[tp_].k == Tok::Num) return toks_[tp_++].v;
        exprBad_ = true;
        return 0;
    }

    bool evalIf(std::string expr, int fileIdx, int line, int col) {
        // defined(X) / defined X first, so X is not expanded
        std::string pre;
        for (usize i = 0; i < expr.size();) {
            if (isIdStart(expr[i])) {
                usize j = i;
                while (j < expr.size() && isIdChar(expr[j])) ++j;
                const std::string id = expr.substr(i, j - i);
                if (id == "defined") {
                    usize k = j;
                    while (k < expr.size() && expr[k] == ' ') ++k;
                    const bool paren = k < expr.size() && expr[k] == '(';
                    if (paren) ++k;
                    while (k < expr.size() && expr[k] == ' ') ++k;
                    usize m = k;
                    while (m < expr.size() && isIdChar(expr[m])) ++m;
                    const std::string name = expr.substr(k, m - k);
                    while (m < expr.size() && expr[m] == ' ') ++m;
                    if (paren && m < expr.size() && expr[m] == ')') ++m;
                    pre += macros_.count(name) ? " 1 " : " 0 ";
                    i = m;
                    continue;
                }
                pre += id;
                i = j;
            } else pre += expr[i++];
        }
        std::set<std::string> hide;
        const std::string ex = expand(pre, hide, 0, fileIdx, line);
        exprBad_ = false;
        lex(ex);
        const long long v = parseTernary();
        if (exprBad_ || tp_ != toks_.size()) {
            diag(GodotSeverity::Error, fileIdx, line, col, "invalid expression in #if / #elif: " + trim(expr));
            return false;
        }
        return v != 0;
    }

    struct Cond { bool parentActive = true, taken = false, active = false, sawElse = false; };

    void file(std::string_view source, int fileIdx, int depth) {
        const std::string text = stripComments(source);
        std::vector<std::string> raw;
        for (usize b = 0; b <= text.size();) {
            usize e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            raw.push_back(text.substr(b, e - b));
            b = e + 1;
        }
        std::vector<Cond> conds;
        bool active = true;
        bool noPre = false;
        for (usize li = 0; li < raw.size() && !aborted_; ++li) {
            const int lineNo = static_cast<int>(li) + 1;
            std::string line = raw[li];
            usize extra = 0;
            // backslash continuation
            while (!line.empty() && line.back() == '\\' && li + extra + 1 < raw.size()) {
                line.pop_back();
                ++extra;
                line += raw[li + extra];
            }
            const std::string t = trim(line);
            if (noPre) { emit(line, fileIdx, lineNo); for (usize x = 0; x < extra; ++x) emit("", fileIdx, lineNo + static_cast<int>(x) + 1); li += extra; continue; }
            if (!t.empty() && t[0] == '#') {
                const int col = static_cast<int>(line.find('#')) + 1;
                std::string rest = trim(t.substr(1));
                usize dl = 0;
                while (dl < rest.size() && isIdChar(rest[dl])) ++dl;
                const std::string dir = rest.substr(0, dl);
                const std::string arg = trim(rest.substr(dl));
                emit("", fileIdx, lineNo);
                for (usize x = 0; x < extra; ++x) emit("", fileIdx, lineNo + static_cast<int>(x) + 1);
                li += extra;

                if (dir == "ifdef" || dir == "ifndef" || dir == "if") {
                    Cond c;
                    c.parentActive = active;
                    if (active) {
                        bool v;
                        if (dir == "if") v = evalIf(arg, fileIdx, lineNo, col);
                        else {
                            const bool def = macros_.count(arg) != 0;
                            v = dir == "ifdef" ? def : !def;
                        }
                        c.active = v;
                        c.taken = v;
                    }
                    conds.push_back(c);
                    active = c.active;
                } else if (dir == "elif" || dir == "else") {
                    if (conds.empty()) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "#" + dir + " without #if"); continue; }
                    Cond& c = conds.back();
                    if (c.sawElse) diag(GodotSeverity::Error, fileIdx, lineNo, col, "#" + dir + " after #else");
                    if (dir == "else") {
                        c.sawElse = true;
                        c.active = c.parentActive && !c.taken;
                        c.taken = true;
                    } else if (c.parentActive && !c.taken) {
                        c.active = evalIf(arg, fileIdx, lineNo, col);
                        c.taken = c.active;
                    } else c.active = false;
                    active = c.active;
                } else if (dir == "endif") {
                    if (conds.empty()) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "#endif without #if"); continue; }
                    conds.pop_back();
                    active = conds.empty() ? true : conds.back().active;
                } else if (!active) {
                    // other directives in a skipped region are ignored
                } else if (dir == "define") {
                    usize n = 0;
                    while (n < arg.size() && isIdChar(arg[n])) ++n;
                    const std::string name = arg.substr(0, n);
                    if (name.empty() || !isIdStart(name[0])) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "#define needs a macro name"); continue; }
                    Macro m;
                    usize p = n;
                    if (p < arg.size() && arg[p] == '(') {
                        m.funcLike = true;
                        const usize close = arg.find(')', p);
                        if (close == std::string::npos) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "unterminated parameter list in #define"); continue; }
                        std::string cur;
                        for (usize q = p + 1; q < close; ++q) {
                            if (arg[q] == ',') { m.params.push_back(trim(cur)); cur.clear(); }
                            else cur += arg[q];
                        }
                        if (!trim(cur).empty()) m.params.push_back(trim(cur));
                        p = close + 1;
                    }
                    m.body = trim(arg.substr(p));
                    macros_[name] = m;
                } else if (dir == "undef") {
                    macros_.erase(arg);
                } else if (dir == "include") {
                    std::string path;
                    if (arg.size() >= 2 && (arg[0] == '"' || arg[0] == '<')) {
                        const char close = arg[0] == '"' ? '"' : '>';
                        const usize e = arg.find(close, 1);
                        if (e != std::string::npos) path = arg.substr(1, e - 1);
                    }
                    if (path.empty()) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "#include needs a quoted path"); continue; }
                    if (depth + 1 > opt_.maxIncludeDepth) { diag(GodotSeverity::Error, fileIdx, lineNo, col, "#include nested too deeply (cycle?): " + path); continue; }
                    std::string inc, resolved;
                    if (!opt_.resolver || !opt_.resolver(path, out_.files[static_cast<usize>(fileIdx)], inc, resolved)) {
                        diag(GodotSeverity::Error, fileIdx, lineNo, col, "cannot open include file '" + path + "'");
                        continue;
                    }
                    if (out_.files.size() >= kMaxFiles) { abort(fileIdx, lineNo, "too many #include files (limit " + std::to_string(kMaxFiles) + ")"); continue; }
                    out_.files.push_back(resolved.empty() ? path : resolved);
                    const int idx = static_cast<int>(out_.files.size()) - 1;
                    file(inc, idx, depth + 1);
                } else if (dir == "pragma") {
                    out_.pragmas.push_back(arg);
                    if (arg == "disable_preprocessor") noPre = true;
                } else if (dir == "error") {
                    diag(GodotSeverity::Error, fileIdx, lineNo, col, "#error " + arg);
                } else if (dir == "warning") {
                    diag(GodotSeverity::Warning, fileIdx, lineNo, col, "#warning " + arg);
                } else {
                    diag(GodotSeverity::Error, fileIdx, lineNo, col, "unknown preprocessor directive '#" + dir + "'");
                }
                continue;
            }
            if (!active) { emit("", fileIdx, lineNo); for (usize x = 0; x < extra; ++x) emit("", fileIdx, lineNo + static_cast<int>(x) + 1); li += extra; continue; }
            // function-like macro calls that span lines
            while (openCall(line) && li + extra + 1 < raw.size()) {
                ++extra;
                line += " " + raw[li + extra];
            }
            std::set<std::string> hide;
            emit(macros_.empty() ? line : expand(line, hide, 0, fileIdx, lineNo), fileIdx, lineNo);
            for (usize x = 0; x < extra; ++x) emit("", fileIdx, lineNo + static_cast<int>(x) + 1);
            li += extra;
        }
        if (!conds.empty() && !aborted_) diag(GodotSeverity::Error, fileIdx, static_cast<int>(raw.size()), 1, "unterminated #if / #ifdef (missing #endif)");
    }
};

}  // namespace

bool gdShaderPreprocess(std::string_view source, std::string_view fileName, const GdPreprocessOptions& opt, GdPreprocessed& out,
                        std::vector<GdShaderDiag>& diags) {
    out = GdPreprocessed{};
    Pre p(opt, out, diags);
    return p.run(source, fileName);
}

}  // namespace aver::fmt::godot
