#pragma once
// std::from_chars for float and double, on every standard library.
//
// Every text format here reads its numbers with std::from_chars: locale-independent, no allocation,
// and exact about where a number ends. The floating-point overloads are missing from libc++ before
// LLVM 20, which is the libc++ in every current Xcode, so on macOS they do not exist at all and a call
// is a hard error. aver::fromChars is a drop-in: the same signature and the same result, std::from_chars
// where the library has it and the fallback below where it does not.
//
// THE FALLBACK ACCEPTS EXACTLY WHAT std::from_chars DOES (chars_format::general): an optional '-', then
// digits with an optional '.', an optional exponent, or inf/infinity/nan. No leading whitespace, no '+',
// no hex. The longest such prefix is handed to strtod/strtof under the "C" locale, so a decimal point is
// always '.', whatever the process locale. tests/platform/src/CharConvTest.cpp holds it to that, and
// compares it with std::from_chars itself wherever both exist.
#include "aver/core/Types.hpp"

#include <charconv>
#include <system_error>
#include <type_traits>

namespace aver {

namespace detail {
std::from_chars_result fromCharsFallback(const char* first, const char* last, f64& value);
std::from_chars_result fromCharsFallback(const char* first, const char* last, f32& value);
}  // namespace detail

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
inline std::from_chars_result fromChars(const char* first, const char* last, f64& value) { return std::from_chars(first, last, value); }
inline std::from_chars_result fromChars(const char* first, const char* last, f32& value) { return std::from_chars(first, last, value); }
#else
inline std::from_chars_result fromChars(const char* first, const char* last, f64& value) { return detail::fromCharsFallback(first, last, value); }
inline std::from_chars_result fromChars(const char* first, const char* last, f32& value) { return detail::fromCharsFallback(first, last, value); }
#endif

// Integers are std::from_chars everywhere -- every standard library has those -- so a template that
// reads both kinds can call aver::fromChars for all of them.
template <class T>
    requires std::is_integral_v<T>
inline std::from_chars_result fromChars(const char* first, const char* last, T& value, int base = 10) {
    return std::from_chars(first, last, value, base);
}

}  // namespace aver
