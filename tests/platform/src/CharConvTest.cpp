// aver/core/CharConv.hpp -- std::from_chars for float and double on every standard library. Exit code =
// failure count.
//
// WHY THIS EXISTS. Every text format in the engine reads its numbers with std::from_chars, and the
// floating-point overloads are missing from libc++ before LLVM 20 -- which is the libc++ every current
// Xcode ships. aver::fromChars is std::from_chars where the library has it and a fallback where it does
// not, and the fallback must accept EXACTLY what std::from_chars accepts or a file that loads on Windows
// fails to load on a Mac. So it is checked two ways: against fixed cases, and -- wherever the standard
// library does have the overloads, which includes this machine and every Windows build -- against
// std::from_chars itself, input for input.
#include "aver/core/CharConv.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

template <class T>
static void expect(const char* text, T want, usize consumed, const char* what) {
    T v = T(-12345);
    const auto r = detail::fromCharsFallback(text, text + std::strlen(text), v);
    check(r.ec == std::errc() && v == want && static_cast<usize>(r.ptr - text) == consumed,
          std::string(what) + " ('" + text + "')");
}

template <class T>
static void expectInvalid(const char* text, const char* what) {
    T v = T(7);
    const auto r = detail::fromCharsFallback(text, text + std::strlen(text), v);
    check(r.ec == std::errc::invalid_argument && r.ptr == text && v == T(7),
          std::string(what) + " ('" + text + "') is invalid_argument and leaves the value alone");
}

int main() {
    expect<f64>("1.5", 1.5, 3, "a plain decimal");
    expect<f64>("-0.25x", -0.25, 5, "a negative stops at the first non-number");
    expect<f64>("1e3", 1000.0, 3, "an exponent");
    expect<f64>("2.5E-2,", 0.025, 6, "an upper-case exponent with a sign");
    expect<f64>("1e", 1.0, 1, "an exponent with no digits is not part of the number");
    expect<f64>(".5", 0.5, 2, "a leading point");
    expect<f64>("5.", 5.0, 2, "a trailing point");
    expect<f64>("0x10", 0.0, 1, "no hex: 0x10 reads as 0, the way from_chars' general format does");
    expect<f32>("3.25f", 3.25f, 4, "float, stopping at a suffix");
    expectInvalid<f64>(" 1", "leading whitespace");
    expectInvalid<f64>("+1", "a leading plus");
    expectInvalid<f64>("", "an empty string");
    expectInvalid<f64>("-", "a lone minus");
    expectInvalid<f64>(".", "a lone point");
    expectInvalid<f32>("e5", "an exponent with no mantissa");
    {
        f64 v = 7.0;
        const char* t = "1e400";
        const auto r = detail::fromCharsFallback(t, t + 5, v);
        check(r.ec == std::errc::result_out_of_range && r.ptr == t + 5 && v == 7.0,
              "1e400 is out of range for a double, consumes the whole number, and leaves the value alone");
    }
    {
        f64 v = 0.0;
        const char* t = "inf";
        const auto r = detail::fromCharsFallback(t, t + 3, v);
        check(r.ec == std::errc() && std::isinf(v) && r.ptr == t + 3, "inf reads as infinity");
    }
    {
        f64 v = 0.0;
        const char* t = "1.5";
        const auto r = detail::fromCharsFallback(t, t + 2, v);   // the range ends inside the number
        check(r.ec == std::errc() && v == 1.0 && r.ptr == t + 2,
              "the range end is respected: \"1.\" of \"1.5\" reads as 1, consuming the point as from_chars does");
    }

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    // The real thing, input for input. Every case above, plus the shapes the engine's formats write.
    const std::vector<std::string> inputs = {
        "0", "-0", "1", "12", "1.5", "-0.25", "1e3", "1E-3", "2.5e+2", ".5", "5.", "0.000001", "123456789.125",
        "3.4028235e38", "1e39", "1e-46", "1e400", "-1e-400", "0x1p3", "0x10", "inf", "-inf", "infinity", "nan",
        "1e", "1e+", "1.e2", " 1", "+1", "", "-", ".", "e5", "1.5f", "6.022e23 ", "0.1,0.2", "--1", "1..2",
        "100000000000000000000000", "4.9e-324", "2.2250738585072014e-308", "1.7976931348623157e308",
    };
    int compared = 0;
    for (const std::string& s : inputs) {
        const char* b = s.data();
        const char* e = s.data() + s.size();
        f64 a = 7.0, w = 7.0;
        const auto ra = detail::fromCharsFallback(b, e, a);
        const auto rw = std::from_chars(b, e, w);
        const bool same = ra.ec == rw.ec && ra.ptr == rw.ptr && (std::isnan(a) ? std::isnan(w) : a == w);
        check(same, "double fallback agrees with std::from_chars on '" + s + "'");
        f32 af = 7.0f, wf = 7.0f;
        const auto rfa = detail::fromCharsFallback(b, e, af);
        const auto rfw = std::from_chars(b, e, wf);
        const bool samef = rfa.ec == rfw.ec && rfa.ptr == rfw.ptr && (std::isnan(af) ? std::isnan(wf) : af == wf);
        check(samef, "float fallback agrees with std::from_chars on '" + s + "'");
        compared += 2;
    }
    AVER_INFO("   compared the fallback with std::from_chars on {} inputs", compared);
#else
    AVER_INFO("   this standard library has no floating std::from_chars; the fixed cases are the reference");
#endif

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
