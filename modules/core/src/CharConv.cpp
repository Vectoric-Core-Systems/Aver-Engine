#include "aver/core/CharConv.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <locale.h>
#elif defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif

namespace aver::detail {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Case-insensitive: does [p, last) start with `lit`?
bool startsWithNoCase(const char* p, const char* last, const char* lit) {
    for (; *lit; ++lit, ++p) {
        if (p == last) return false;
        const char a = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p + 32) : *p;
        if (a != *lit) return false;
    }
    return true;
}

// The end of the longest prefix of [first, last) that std::from_chars (chars_format::general) would read
// as a number, or `first` when there is none.
const char* scanNumber(const char* first, const char* last) {
    const char* p = first;
    if (p != last && *p == '-') ++p;
    if (startsWithNoCase(p, last, "infinity")) return p + 8;
    if (startsWithNoCase(p, last, "inf")) return p + 3;
    if (startsWithNoCase(p, last, "nan")) {
        const char* q = p + 3;
        if (q != last && *q == '(') {   // nan(n-char-sequence), taken only when it closes
            const char* r = q + 1;
            while (r != last && (isDigit(*r) || (*r >= 'a' && *r <= 'z') || (*r >= 'A' && *r <= 'Z') || *r == '_')) ++r;
            if (r != last && *r == ')') return r + 1;
        }
        return q;
    }
    const char* digits = p;
    while (p != last && isDigit(*p)) ++p;
    usize mantissa = static_cast<usize>(p - digits);
    if (p != last && *p == '.') {
        const char* frac = ++p;
        while (p != last && isDigit(*p)) ++p;
        mantissa += static_cast<usize>(p - frac);
    }
    if (mantissa == 0) return first;
    if (p != last && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q != last && (*q == '+' || *q == '-')) ++q;
        if (q != last && isDigit(*q)) {
            while (q != last && isDigit(*q)) ++q;
            p = q;
        }
    }
    return p;
}

// The "C" numeric locale, once, so a '.' is the decimal point whatever the process locale says.
#if defined(_WIN32)
using CLocale = _locale_t;
CLocale cLocale() { static const CLocale l = _create_locale(LC_NUMERIC, "C"); return l; }
f64 toDouble(const char* s, char** end) { return _strtod_l(s, end, cLocale()); }
f32 toFloat(const char* s, char** end) { return _strtof_l(s, end, cLocale()); }
#else
using CLocale = locale_t;
CLocale cLocale() { static const CLocale l = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(nullptr)); return l; }
f64 toDouble(const char* s, char** end) { return strtod_l(s, end, cLocale()); }
f32 toFloat(const char* s, char** end) { return strtof_l(s, end, cLocale()); }
#endif

template <class T, class Convert>
std::from_chars_result parse(const char* first, const char* last, T& value, Convert convert) {
    const char* end = scanNumber(first, last);
    if (end == first) return {first, std::errc::invalid_argument};
    const std::string text(first, end);   // strtod needs a terminator; the range may not have one
    char* stop = nullptr;
    const T v = convert(text.c_str(), &stop);
    // OUT OF RANGE IS DECIDED FROM THE RESULT, std::from_chars' own rule, not from errno: MSVC's
    // _strtod_l/_strtof_l do not set ERANGE for these inputs. An infinity the text did not spell, or a zero
    // from a mantissa with a non-zero digit, is out of range; a subnormal is a value. As with
    // std::from_chars, `value` is then left as it was.
    const char* p = text.c_str();
    if (*p == '-') ++p;
    const bool spelledInf = *p == 'i' || *p == 'I';
    bool nonZeroDigit = false;
    for (const char* q = p; *q && *q != 'e' && *q != 'E'; ++q) nonZeroDigit |= (*q >= '1' && *q <= '9');
    if ((std::isinf(v) && !spelledInf) || (v == T(0) && nonZeroDigit)) return {end, std::errc::result_out_of_range};
    value = v;
    return {end, std::errc()};
}

}  // namespace

std::from_chars_result fromCharsFallback(const char* first, const char* last, f64& value) {
    return parse(first, last, value, toDouble);
}

std::from_chars_result fromCharsFallback(const char* first, const char* last, f32& value) {
    return parse(first, last, value, toFloat);
}

}  // namespace aver::detail
