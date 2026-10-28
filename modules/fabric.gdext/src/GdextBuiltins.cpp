// Builtin methods (String.length(), Array.append() ...) and the utility functions (print, sin, randf ...) the host implements.
// Anything not here is answered with a null function pointer by the interface and listed once in the log (G3_GDEXT.md).
#include "GdextHost.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

static void ret(Var* r, V&& v) { varDestroy(r); *r = v.v; varNil(&v.v); }       // r is an initialised Variant
static double num(const Var* v) { return v->type == T_INT ? double(ld<int64_t>(v->d)) : v->type == T_FLOAT ? ld<double>(v->d) : v->type == T_BOOL ? v->d[0] : 0.0; }
static int64_t inum(const Var* v) { return v->type == T_INT ? ld<int64_t>(v->d) : int64_t(num(v)); }
static std::u32string S(const Var* v) {
    if (v->type == T_STRING_NAME) return utf8ToU32(nameText(ld<NameData*>(v->d)));
    auto* s = ld<StrData*>(v->d);
    return s ? s->s : std::u32string();
}
static V str(const std::u32string& s) { return V::ofString(u32ToUtf8(s)); }

// ---------------------------------------------------------------------------------------------------
// output

static void (*g_printHook)(int, const std::string&) = nullptr;
void setPrintHook(void (*fn)(int, const std::string&)) { g_printHook = fn; }

void hostLog(int level, const std::string& text) {
    if (g_printHook) { g_printHook(level, text); return; }
    if (level == 0) AVER_INFO("[GDExtension] {}", text);
    else if (level == 1) AVER_WARN("[GDExtension] {}", text);
    else AVER_ERROR("[GDExtension] {}", text);
}

// ---------------------------------------------------------------------------------------------------
// String

static size_t findSub(const std::u32string& s, const std::u32string& what, size_t from) { return s.find(what, from); }

static std::u32string lowerU(std::u32string s) { for (auto& c : s) if (c < 128) c = char32_t(std::tolower(int(c))); return s; }
static std::u32string upperU(std::u32string s) { for (auto& c : s) if (c < 128) c = char32_t(std::toupper(int(c))); return s; }

static void strLength(Var* self, const Var* const*, int, Var* r) { ret(r, V::ofInt(int64_t(S(self).size()))); }
static void strIsEmpty(Var* self, const Var* const*, int, Var* r) { ret(r, V::ofBool(S(self).empty())); }
static void strUpper(Var* self, const Var* const*, int, Var* r) { ret(r, str(upperU(S(self)))); }
static void strLower(Var* self, const Var* const*, int, Var* r) { ret(r, str(lowerU(S(self)))); }
static void strSubstr(Var* self, const Var* const* a, int, Var* r) {
    std::u32string s = S(self);
    int64_t from = std::max<int64_t>(0, inum(a[0])), len = inum(a[1]);
    if (from >= int64_t(s.size())) { ret(r, str({})); return; }
    ret(r, str(len < 0 ? s.substr(size_t(from)) : s.substr(size_t(from), size_t(len))));
}
static void strBegins(Var* self, const Var* const* a, int, Var* r) { auto s = S(self), p = S(a[0]); ret(r, V::ofBool(s.compare(0, p.size(), p) == 0 && s.size() >= p.size())); }
static void strEnds(Var* self, const Var* const* a, int, Var* r) { auto s = S(self), p = S(a[0]); ret(r, V::ofBool(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0)); }
static void strFind(Var* self, const Var* const* a, int, Var* r) {
    size_t i = findSub(S(self), S(a[0]), size_t(std::max<int64_t>(0, inum(a[1]))));
    ret(r, V::ofInt(i == std::u32string::npos ? -1 : int64_t(i)));
}
static void strContains(Var* self, const Var* const* a, int, Var* r) { ret(r, V::ofBool(S(self).find(S(a[0])) != std::u32string::npos)); }
static void strReplace(Var* self, const Var* const* a, int, Var* r) {
    std::u32string s = S(self);
    const std::u32string what = S(a[0]), with = S(a[1]);
    if (!what.empty())
        for (size_t i = 0; (i = s.find(what, i)) != std::u32string::npos; i += with.size()) s.replace(i, what.size(), with);
    ret(r, str(s));
}
static void strToInt(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofInt(std::strtoll(u32ToUtf8(S(self)).c_str(), nullptr, 10))); }
static void strToFloat(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(std::strtod(u32ToUtf8(S(self)).c_str(), nullptr))); }
static void strStrip(Var* self, const Var* const* a, int, Var* r) {
    std::u32string s = S(self);
    auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    if (a[1]->d[0]) while (!s.empty() && ws(s.back())) s.pop_back();
    if (a[0]->d[0]) { size_t i = 0; while (i < s.size() && ws(s[i])) ++i; s.erase(0, i); }
    ret(r, str(s));
}
static void strSplit(Var* self, const Var* const* a, int, Var* r) {
    std::u32string s = S(self);
    const std::u32string d = S(a[0]);
    const bool allowEmpty = a[1]->d[0];
    const int64_t maxSplit = inum(a[2]);
    V out = makePacked(T_PACKED_STRING);
    std::vector<std::u32string> parts;
    if (d.empty()) { for (char32_t c : s) parts.push_back(std::u32string(1, c)); }
    else {
        size_t i = 0;
        while (true) {
            size_t j = s.find(d, i);
            if (j == std::u32string::npos || (maxSplit > 0 && int64_t(parts.size()) >= maxSplit)) { parts.push_back(s.substr(i)); break; }
            parts.push_back(s.substr(i, j - i));
            i = j + d.size();
        }
    }
    for (auto& p : parts) {
        if (p.empty() && !allowEmpty) continue;
        V e = str(p);
        bool oob;
        PackedData* pd = ld<PackedData*>(out.v.d);
        pd->bytes.resize(pd->bytes.size() + 8, 0);
        varSetIndexed(&out.v, int64_t(pd->count()) - 1, &e.v, &oob);
    }
    ret(r, std::move(out));
}
static void strHash(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofInt(varHash(self))); }
static void strRepeat(Var* self, const Var* const* a, int, Var* r) {
    std::u32string s = S(self), o;
    for (int64_t i = 0; i < inum(a[0]); ++i) o += s;
    ret(r, str(o));
}
static void strLeft(Var* self, const Var* const* a, int, Var* r) { auto s = S(self); int64_t n = inum(a[0]); ret(r, str(n >= 0 ? s.substr(0, size_t(n)) : s.substr(0, size_t(std::max<int64_t>(0, int64_t(s.size()) + n))))); }
static void strRight(Var* self, const Var* const* a, int, Var* r) { auto s = S(self); int64_t n = inum(a[0]); size_t k = n >= 0 ? size_t(std::min<int64_t>(n, int64_t(s.size()))) : size_t(std::max<int64_t>(0, int64_t(s.size()) + n)); ret(r, str(s.substr(s.size() - k))); }
static void strTrimPrefix(Var* self, const Var* const* a, int, Var* r) { auto s = S(self), p = S(a[0]); if (!p.empty() && s.compare(0, p.size(), p) == 0) s.erase(0, p.size()); ret(r, str(s)); }
static void strTrimSuffix(Var* self, const Var* const* a, int, Var* r) { auto s = S(self), p = S(a[0]); if (!p.empty() && s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0) s.erase(s.size() - p.size()); ret(r, str(s)); }
static void strIsValidInt(Var* self, const Var*const*, int, Var* r) {
    std::string s = u32ToUtf8(S(self));
    char* end = nullptr;
    if (!s.empty()) std::strtoll(s.c_str(), &end, 10);
    ret(r, V::ofBool(!s.empty() && end && *end == 0));
}
static void strIsValidFloat(Var* self, const Var*const*, int, Var* r) {
    std::string s = u32ToUtf8(S(self));
    char* end = nullptr;
    if (!s.empty()) std::strtod(s.c_str(), &end);
    ret(r, V::ofBool(!s.empty() && end && *end == 0));
}
static void strToUtf8(Var* self, const Var*const*, int, Var* r) {
    std::string u = u32ToUtf8(S(self));
    V out = makePacked(T_PACKED_BYTE);
    ld<PackedData*>(out.v.d)->bytes.assign(u.begin(), u.end());
    ret(r, std::move(out));
}
static void strJoin(Var* self, const Var* const* a, int, Var* r) {
    const PackedData* p = packedOf(a[0]);
    std::u32string d = S(self), o;
    for (size_t i = 0; i < p->count(); ++i) {
        if (i) o += d;
        if (auto* s = ld<StrData*>(p->bytes.data() + i * 8)) o += s->s;
    }
    ret(r, str(o));
}

// ---------------------------------------------------------------------------------------------------
// vectors

static const float* F(const Var* v) { return static_cast<const float*>(varPtr(v)); }
static double len2(const Var* v, int n) { double s = 0; for (int i = 0; i < n; ++i) s += double(F(v)[i]) * F(v)[i]; return s; }
static V vec(int type, const double* c, int n) { float f[4] = {}; for (int i = 0; i < n; ++i) f[i] = float(c[i]); return V::ofNative(type, f); }

static void v2Length(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(std::sqrt(len2(self, 2)))); }
static void v3Length(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(std::sqrt(len2(self, 3)))); }
static void v4Length(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(std::sqrt(len2(self, 4)))); }
static void v2LengthSq(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(len2(self, 2))); }
static void v3LengthSq(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(len2(self, 3))); }
static void v4LengthSq(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(len2(self, 4))); }

template <int N> static void vNormalized(Var* self, const Var*const*, int, Var* r) {
    double l = std::sqrt(len2(self, N)), c[4];
    for (int i = 0; i < N; ++i) c[i] = l > 0 ? F(self)[i] / l : 0;
    ret(r, vec(self->type, c, N));
}
template <int N> static void vDot(Var* self, const Var* const* a, int, Var* r) {
    double s = 0;
    for (int i = 0; i < N; ++i) s += double(F(self)[i]) * F(a[0])[i];
    ret(r, V::ofFloat(s));
}
template <int N> static void vDistance(Var* self, const Var* const* a, int, Var* r) {
    double s = 0;
    for (int i = 0; i < N; ++i) { double d = double(F(self)[i]) - F(a[0])[i]; s += d * d; }
    ret(r, V::ofFloat(std::sqrt(s)));
}
template <int N> static void vDistanceSq(Var* self, const Var* const* a, int, Var* r) {
    double s = 0;
    for (int i = 0; i < N; ++i) { double d = double(F(self)[i]) - F(a[0])[i]; s += d * d; }
    ret(r, V::ofFloat(s));
}
template <int N> static void vLerp(Var* self, const Var* const* a, int, Var* r) {
    double c[4], t = num(a[1]);
    for (int i = 0; i < N; ++i) c[i] = F(self)[i] + (F(a[0])[i] - F(self)[i]) * t;
    ret(r, vec(self->type, c, N));
}
template <int N> static void vAbs(Var* self, const Var*const*, int, Var* r) { double c[4]; for (int i = 0; i < N; ++i) c[i] = std::fabs(F(self)[i]); ret(r, vec(self->type, c, N)); }
template <int N> static void vFloor(Var* self, const Var*const*, int, Var* r) { double c[4]; for (int i = 0; i < N; ++i) c[i] = std::floor(F(self)[i]); ret(r, vec(self->type, c, N)); }
template <int N> static void vIsZeroApprox(Var* self, const Var*const*, int, Var* r) { bool z = true; for (int i = 0; i < N; ++i) z = z && std::fabs(F(self)[i]) < 1e-5f; ret(r, V::ofBool(z)); }
template <int N> static void vIsEqualApprox(Var* self, const Var* const* a, int, Var* r) { bool z = true; for (int i = 0; i < N; ++i) z = z && std::fabs(F(self)[i] - F(a[0])[i]) < 1e-5f * std::max(1.0f, std::fabs(F(self)[i])); ret(r, V::ofBool(z)); }
static void v3Cross(Var* self, const Var* const* a, int, Var* r) {
    const float* x = F(self); const float* y = F(a[0]);
    double c[3] = {double(x[1]) * y[2] - double(x[2]) * y[1], double(x[2]) * y[0] - double(x[0]) * y[2], double(x[0]) * y[1] - double(x[1]) * y[0]};
    ret(r, vec(T_VECTOR3, c, 3));
}
static void v3DirectionTo(Var* self, const Var* const* a, int, Var* r) {
    double c[3], l = 0;
    for (int i = 0; i < 3; ++i) { c[i] = double(F(a[0])[i]) - F(self)[i]; l += c[i] * c[i]; }
    l = std::sqrt(l);
    for (double& d : c) d = l > 0 ? d / l : 0;
    ret(r, vec(T_VECTOR3, c, 3));
}
static void v2DirectionTo(Var* self, const Var* const* a, int, Var* r) {
    double c[2], l = 0;
    for (int i = 0; i < 2; ++i) { c[i] = double(F(a[0])[i]) - F(self)[i]; l += c[i] * c[i]; }
    l = std::sqrt(l);
    for (double& d : c) d = l > 0 ? d / l : 0;
    ret(r, vec(T_VECTOR2, c, 2));
}
static void v2Angle(Var* self, const Var*const*, int, Var* r) { ret(r, V::ofFloat(std::atan2(double(F(self)[1]), double(F(self)[0])))); }
static void qInverse(Var* self, const Var*const*, int, Var* r) { double c[4] = {-F(self)[0], -F(self)[1], -F(self)[2], F(self)[3]}; ret(r, vec(T_QUATERNION, c, 4)); }

// ---------------------------------------------------------------------------------------------------
// Array / Dictionary / Packed

static ArrayData* A(Var* v) { return ld<ArrayData*>(v->d); }
static DictData* D(Var* v) { return ld<DictData*>(v->d); }

static void arrSize(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofInt(int64_t(A(s)->items.size()))); }
static void arrIsEmpty(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofBool(A(s)->items.empty())); }
static void arrClear(Var* s, const Var*const*, int, Var*) { for (Var& v : A(s)->items) varDestroy(&v); A(s)->items.clear(); }
static void arrPushBack(Var* s, const Var* const* a, int, Var*) { A(s)->items.emplace_back(); varCopy(&A(s)->items.back(), a[0]); }
static void arrPushFront(Var* s, const Var* const* a, int, Var*) { Var v; varCopy(&v, a[0]); A(s)->items.insert(A(s)->items.begin(), v); }
static void arrPopBack(Var* s, const Var*const*, int, Var* r) {
    ArrayData* d = A(s);
    if (d->items.empty()) return;
    Var v = d->items.back();
    d->items.pop_back();
    varDestroy(r); *r = v;
}
static void arrPopFront(Var* s, const Var*const*, int, Var* r) {
    ArrayData* d = A(s);
    if (d->items.empty()) return;
    Var v = d->items.front();
    d->items.erase(d->items.begin());
    varDestroy(r); *r = v;
}
static void arrFront(Var* s, const Var*const*, int, Var* r) { if (!A(s)->items.empty()) varAssign(r, &A(s)->items.front()); }
static void arrBack(Var* s, const Var*const*, int, Var* r) { if (!A(s)->items.empty()) varAssign(r, &A(s)->items.back()); }
static int arrFindIdx(Var* s, const Var* x, int64_t from) {
    ArrayData* d = A(s);
    for (size_t i = size_t(std::max<int64_t>(0, from)); i < d->items.size(); ++i) if (varEqual(&d->items[i], x)) return int(i);
    return -1;
}
static void arrHas(Var* s, const Var* const* a, int, Var* r) { ret(r, V::ofBool(arrFindIdx(s, a[0], 0) >= 0)); }
static void arrFind(Var* s, const Var* const* a, int, Var* r) { ret(r, V::ofInt(arrFindIdx(s, a[0], inum(a[1])))); }
static void arrCount(Var* s, const Var* const* a, int, Var* r) { int64_t n = 0; for (Var& v : A(s)->items) n += varEqual(&v, a[0]); ret(r, V::ofInt(n)); }
static void arrErase(Var* s, const Var* const* a, int, Var*) {
    int i = arrFindIdx(s, a[0], 0);
    if (i < 0) return;
    varDestroy(&A(s)->items[size_t(i)]);
    A(s)->items.erase(A(s)->items.begin() + i);
}
static void arrRemoveAt(Var* s, const Var* const* a, int, Var*) {
    int64_t i = inum(a[0]);
    ArrayData* d = A(s);
    if (i < 0) i += int64_t(d->items.size());
    if (i < 0 || i >= int64_t(d->items.size())) return;
    varDestroy(&d->items[size_t(i)]);
    d->items.erase(d->items.begin() + i);
}
static void arrInsert(Var* s, const Var* const* a, int, Var* r) {
    ArrayData* d = A(s);
    int64_t i = inum(a[0]);
    if (i < 0) i += int64_t(d->items.size());
    if (i < 0 || i > int64_t(d->items.size())) { ret(r, V::ofInt(31)); return; }
    Var v; varCopy(&v, a[1]);
    d->items.insert(d->items.begin() + i, v);
    ret(r, V::ofInt(0));
}
static void arrResize(Var* s, const Var* const* a, int, Var* r) {
    ArrayData* d = A(s);
    int64_t n = inum(a[0]);
    if (n < 0) { ret(r, V::ofInt(31)); return; }
    while (int64_t(d->items.size()) > n) { varDestroy(&d->items.back()); d->items.pop_back(); }
    while (int64_t(d->items.size()) < n) { d->items.emplace_back(); varNil(&d->items.back()); }
    ret(r, V::ofInt(0));
}
static void arrAppendArray(Var* s, const Var* const* a, int, Var*) {
    ArrayData* src = A(const_cast<Var*>(a[0]));
    std::vector<Var> copy(src->items.size());
    for (size_t i = 0; i < copy.size(); ++i) varCopy(&copy[i], &src->items[i]);
    for (Var& v : copy) A(s)->items.push_back(v);
}
static void arrReverse(Var* s, const Var*const*, int, Var*) { std::reverse(A(s)->items.begin(), A(s)->items.end()); }
static void arrFill(Var* s, const Var* const* a, int, Var*) { for (Var& v : A(s)->items) varAssign(&v, a[0]); }
static void arrHash(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofInt(varHash(s))); }
static void arrSort(Var* s, const Var*const*, int, Var*) {
    std::stable_sort(A(s)->items.begin(), A(s)->items.end(), [](const Var& x, const Var& y) { bool ok; return varLess(&x, &y, &ok); });
}
static V dupVar(const Var* v, bool deep);
static void arrDuplicate(Var* s, const Var* const* a, int, Var* r) { ret(r, dupVar(s, a[0]->d[0])); }

static V dupVar(const Var* v, bool deep) {
    if (v->type == T_ARRAY) {
        V out = makeArray();
        ArrayData* o = A(&out.v);
        ArrayData* src = ld<ArrayData*>(v->d);
        o->elemType = src->elemType; o->elemClass = src->elemClass;
        for (const Var& e : src->items) {
            o->items.emplace_back();
            if (deep && (e.type == T_ARRAY || e.type == T_DICTIONARY)) { V c = dupVar(&e, true); o->items.back() = c.v; varNil(&c.v); }
            else varCopy(&o->items.back(), &e);
        }
        return out;
    }
    if (v->type == T_DICTIONARY) {
        V out = makeDict();
        DictData* o = D(&out.v);
        for (const auto& kv : ld<DictData*>(v->d)->items) {
            o->items.emplace_back();
            varCopy(&o->items.back().first, &kv.first);
            if (deep && (kv.second.type == T_ARRAY || kv.second.type == T_DICTIONARY)) { V c = dupVar(&kv.second, true); o->items.back().second = c.v; varNil(&c.v); }
            else varCopy(&o->items.back().second, &kv.second);
        }
        return out;
    }
    V out;
    varAssign(&out.v, v);
    return out;
}

static void dictSize(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofInt(int64_t(D(s)->items.size()))); }
static void dictIsEmpty(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofBool(D(s)->items.empty())); }
static void dictClear(Var* s, const Var*const*, int, Var*) { DictData* d = D(s); for (auto& kv : d->items) { varDestroy(&kv.first); varDestroy(&kv.second); } d->items.clear(); }
static void dictHas(Var* s, const Var* const* a, int, Var* r) { ret(r, V::ofBool(dictFind(D(s), a[0]) >= 0)); }
static void dictErase(Var* s, const Var* const* a, int, Var* r) {
    DictData* d = D(s);
    int i = dictFind(d, a[0]);
    if (i >= 0) { varDestroy(&d->items[size_t(i)].first); varDestroy(&d->items[size_t(i)].second); d->items.erase(d->items.begin() + i); }
    ret(r, V::ofBool(i >= 0));
}
static void dictKeys(Var* s, const Var*const*, int, Var* r) {
    V out = makeArray();
    for (auto& kv : D(s)->items) { A(&out.v)->items.emplace_back(); varCopy(&A(&out.v)->items.back(), &kv.first); }
    ret(r, std::move(out));
}
static void dictValues(Var* s, const Var*const*, int, Var* r) {
    V out = makeArray();
    for (auto& kv : D(s)->items) { A(&out.v)->items.emplace_back(); varCopy(&A(&out.v)->items.back(), &kv.second); }
    ret(r, std::move(out));
}
static void dictGet(Var* s, const Var* const* a, int, Var* r) {
    int i = dictFind(D(s), a[0]);
    varAssign(r, i >= 0 ? &D(s)->items[size_t(i)].second : a[1]);
}
static void dictDuplicate(Var* s, const Var* const* a, int, Var* r) { ret(r, dupVar(s, a[0]->d[0])); }
static void dictMerge(Var* s, const Var* const* a, int, Var*) {
    for (auto& kv : D(const_cast<Var*>(a[0]))->items) {
        int i = dictFind(D(s), &kv.first);
        if (i >= 0 && !a[1]->d[0]) continue;
        varAssign(dictSlot(D(s), &kv.first), &kv.second);
    }
}
static void dictHash(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofInt(varHash(s))); }

static PackedData* P(Var* s) { PackedData* p = ld<PackedData*>(s->d); packedDetach(p); st<PackedData*>(s->d, p); return p; }
static void pkSize(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofInt(int64_t(ld<PackedData*>(s->d)->count()))); }
static void pkIsEmpty(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofBool(ld<PackedData*>(s->d)->bytes.empty())); }
static void pkClear(Var* s, const Var*const*, int, Var*) {
    PackedData* p = P(s);
    if (p->type == T_PACKED_STRING) for (size_t i = 0; i < p->count(); ++i) delete ld<StrData*>(p->bytes.data() + i * 8);
    p->bytes.clear();
}
static void pkResize(Var* s, const Var* const* a, int, Var* r) {
    PackedData* p = P(s);
    int64_t n = inum(a[0]);
    if (n < 0) { ret(r, V::ofInt(31)); return; }
    if (p->type == T_PACKED_STRING)
        for (size_t i = size_t(n); i < p->count(); ++i) delete ld<StrData*>(p->bytes.data() + i * 8);
    p->bytes.resize(size_t(n) * p->esize, 0);
    ret(r, V::ofInt(0));
}
static void pkPushBack(Var* s, const Var* const* a, int, Var* r) {
    PackedData* p = P(s);
    p->bytes.resize(p->bytes.size() + p->esize, 0);
    bool oob;
    const bool ok = varSetIndexed(s, int64_t(p->count()) - 1, a[0], &oob);
    if (!ok) p->bytes.resize(p->bytes.size() - p->esize);
    ret(r, V::ofBool(ok));
}
static void pkSet(Var* s, const Var* const* a, int, Var*) { bool oob; varSetIndexed(s, inum(a[0]), a[1], &oob); }
static void pkGet(Var* s, const Var* const* a, int, Var* r) { bool oob; Var out; varNil(&out); if (varGetIndexed(s, inum(a[0]), &out, &oob)) { varDestroy(r); *r = out; } }
static void pkHas(Var* s, const Var* const* a, int, Var* r) {
    bool found = false;
    for (size_t i = 0; i < ld<PackedData*>(s->d)->count() && !found; ++i) {
        Var e; varNil(&e); bool oob;
        varGetIndexed(s, int64_t(i), &e, &oob);
        found = varEqual(&e, a[0]);
        varDestroy(&e);
    }
    ret(r, V::ofBool(found));
}
static void pkRemoveAt(Var* s, const Var* const* a, int, Var*) {
    PackedData* p = P(s);
    int64_t i = inum(a[0]);
    if (i < 0 || i >= int64_t(p->count())) return;
    if (p->type == T_PACKED_STRING) delete ld<StrData*>(p->bytes.data() + size_t(i) * 8);
    p->bytes.erase(p->bytes.begin() + i * int64_t(p->esize), p->bytes.begin() + (i + 1) * int64_t(p->esize));
}
static void pkAppendArray(Var* s, const Var* const* a, int, Var*) {
    const PackedData* src = ld<PackedData*>(a[0]->d);
    for (size_t i = 0; i < src->count(); ++i) {
        Var e; varNil(&e); bool oob;
        varGetIndexed(a[0], int64_t(i), &e, &oob);
        const Var* args[1] = {&e};
        V dummy;
        pkPushBack(s, args, 1, &dummy.v);
        varDestroy(&e);
    }
}
static void pkDuplicate(Var* s, const Var*const*, int, Var* r) {
    V out = makePacked(s->type);
    const Var* args[1] = {s};
    pkAppendArray(&out.v, args, 1, nullptr);
    ret(r, std::move(out));
}

// ---------------------------------------------------------------------------------------------------
// Callable / Signal

static void callCallableM(Var* s, const Var* const* a, int n, Var* r) {
    GDExtensionCallError err{};
    callCallable(s, a, n, r, &err);
}
static void callvCallable(Var* s, const Var* const* a, int, Var* r) {
    ArrayData* arr = A(const_cast<Var*>(a[0]));
    std::vector<const Var*> args;
    for (Var& v : arr->items) args.push_back(&v);
    GDExtensionCallError err{};
    callCallable(s, args.data(), int(args.size()), r, &err);
}
static void callableIsNull(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofBool(ld<CallableData*>(s->d) == nullptr)); }
static void callableIsValid(Var* s, const Var*const*, int, Var* r) {
    auto* c = ld<CallableData*>(s->d);
    bool ok = c && (c->custom ? (!c->info.is_valid_func || c->info.is_valid_func(c->info.callable_userdata)) : objectById(c->objectId) != nullptr);
    ret(r, V::ofBool(ok));
}
static void callableGetObject(Var* s, const Var*const*, int, Var* r) {
    auto* c = ld<CallableData*>(s->d);
    if (c) ret(r, V::ofObject(c->custom ? objectById(c->info.object_id) : objectById(c->objectId)));
}
static void callableGetMethod(Var* s, const Var*const*, int, Var* r) { auto* c = ld<CallableData*>(s->d); ret(r, V::ofName(c && !c->custom ? nameText(c->method) : "")); }
static void callableIsCustom(Var* s, const Var*const*, int, Var* r) { auto* c = ld<CallableData*>(s->d); ret(r, V::ofBool(c && c->custom)); }

static HostObject* sigObject(Var* s) { return objectById(ld<uint64_t>(s->d)); }
static std::string sigName(Var* s) { return nameText(ld<NameData*>(s->d + 8)); }
static void signalEmit(Var* s, const Var* const* a, int n, Var* r) {
    HostObject* o = sigObject(s);
    ret(r, V::ofInt(o ? emitSignal(o, sigName(s), a, n) : kErrUnavailable));
}
static void signalConnect(Var* s, const Var* const* a, int, Var* r) {
    HostObject* o = sigObject(s);
    ret(r, V::ofInt(o ? connectSignal(o, sigName(s), a[0], uint32_t(inum(a[1]))) : kErrUnavailable));
}
static void signalDisconnect(Var* s, const Var* const* a, int, Var*) { if (HostObject* o = sigObject(s)) disconnectSignal(o, sigName(s), a[0]); }
static void signalIsNull(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofBool(sigObject(s) == nullptr)); }
static void signalGetName(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofName(sigName(s))); }
static void signalGetObject(Var* s, const Var*const*, int, Var* r) { ret(r, V::ofObject(sigObject(s))); }

// ---------------------------------------------------------------------------------------------------
// the table

static std::vector<BuiltinMethodInfo>& table() {
    static std::vector<BuiltinMethodInfo> tbl = [] {
        std::vector<BuiltinMethodInfo> m;
        auto add = [&](int type, const char* name, BuiltinFn fn, const char* args, const char* r, bool isConst = true, bool vararg = false) {
            m.push_back({type, name, fn, args, r, isConst, vararg});
        };
        for (int t : {T_STRING, T_STRING_NAME}) {
            add(t, "length", strLength, "", "int");
            add(t, "is_empty", strIsEmpty, "", "bool");
            add(t, "to_upper", strUpper, "", "String");
            add(t, "to_lower", strLower, "", "String");
            add(t, "substr", strSubstr, "int int", "String");
            add(t, "begins_with", strBegins, "String", "bool");
            add(t, "ends_with", strEnds, "String", "bool");
            add(t, "find", strFind, "String int", "int");
            add(t, "contains", strContains, "String", "bool");
            add(t, "replace", strReplace, "String String", "String");
            add(t, "to_int", strToInt, "", "int");
            add(t, "to_float", strToFloat, "", "float");
            add(t, "strip_edges", strStrip, "bool bool", "String");
            add(t, "split", strSplit, "String bool int", "PackedStringArray");
            add(t, "hash", strHash, "", "int");
            add(t, "repeat", strRepeat, "int", "String");
            add(t, "left", strLeft, "int", "String");
            add(t, "right", strRight, "int", "String");
            add(t, "trim_prefix", strTrimPrefix, "String", "String");
            add(t, "trim_suffix", strTrimSuffix, "String", "String");
            add(t, "is_valid_int", strIsValidInt, "", "bool");
            add(t, "is_valid_float", strIsValidFloat, "", "bool");
            add(t, "to_utf8_buffer", strToUtf8, "", "PackedByteArray");
            add(t, "join", strJoin, "PackedStringArray", "String");
        }
        add(T_VECTOR2, "length", v2Length, "", "float");
        add(T_VECTOR2, "length_squared", v2LengthSq, "", "float");
        add(T_VECTOR2, "normalized", vNormalized<2>, "", "Vector2");
        add(T_VECTOR2, "dot", vDot<2>, "Vector2", "float");
        add(T_VECTOR2, "distance_to", vDistance<2>, "Vector2", "float");
        add(T_VECTOR2, "distance_squared_to", vDistanceSq<2>, "Vector2", "float");
        add(T_VECTOR2, "lerp", vLerp<2>, "Vector2 float", "Vector2");
        add(T_VECTOR2, "abs", vAbs<2>, "", "Vector2");
        add(T_VECTOR2, "floor", vFloor<2>, "", "Vector2");
        add(T_VECTOR2, "angle", v2Angle, "", "float");
        add(T_VECTOR2, "direction_to", v2DirectionTo, "Vector2", "Vector2");
        add(T_VECTOR2, "is_zero_approx", vIsZeroApprox<2>, "", "bool");
        add(T_VECTOR2, "is_equal_approx", vIsEqualApprox<2>, "Vector2", "bool");
        add(T_VECTOR3, "length", v3Length, "", "float");
        add(T_VECTOR3, "length_squared", v3LengthSq, "", "float");
        add(T_VECTOR3, "normalized", vNormalized<3>, "", "Vector3");
        add(T_VECTOR3, "dot", vDot<3>, "Vector3", "float");
        add(T_VECTOR3, "cross", v3Cross, "Vector3", "Vector3");
        add(T_VECTOR3, "distance_to", vDistance<3>, "Vector3", "float");
        add(T_VECTOR3, "distance_squared_to", vDistanceSq<3>, "Vector3", "float");
        add(T_VECTOR3, "lerp", vLerp<3>, "Vector3 float", "Vector3");
        add(T_VECTOR3, "abs", vAbs<3>, "", "Vector3");
        add(T_VECTOR3, "floor", vFloor<3>, "", "Vector3");
        add(T_VECTOR3, "direction_to", v3DirectionTo, "Vector3", "Vector3");
        add(T_VECTOR3, "is_zero_approx", vIsZeroApprox<3>, "", "bool");
        add(T_VECTOR3, "is_equal_approx", vIsEqualApprox<3>, "Vector3", "bool");
        add(T_VECTOR4, "length", v4Length, "", "float");
        add(T_VECTOR4, "length_squared", v4LengthSq, "", "float");
        add(T_VECTOR4, "normalized", vNormalized<4>, "", "Vector4");
        add(T_VECTOR4, "dot", vDot<4>, "Vector4", "float");
        add(T_QUATERNION, "length", v4Length, "", "float");
        add(T_QUATERNION, "length_squared", v4LengthSq, "", "float");
        add(T_QUATERNION, "normalized", vNormalized<4>, "", "Quaternion");
        add(T_QUATERNION, "dot", vDot<4>, "Quaternion", "float");
        add(T_QUATERNION, "inverse", qInverse, "", "Quaternion");
        add(T_ARRAY, "size", arrSize, "", "int");
        add(T_ARRAY, "is_empty", arrIsEmpty, "", "bool");
        add(T_ARRAY, "clear", arrClear, "", "", false);
        add(T_ARRAY, "push_back", arrPushBack, "Variant", "", false);
        add(T_ARRAY, "append", arrPushBack, "Variant", "", false);
        add(T_ARRAY, "push_front", arrPushFront, "Variant", "", false);
        add(T_ARRAY, "pop_back", arrPopBack, "", "Variant", false);
        add(T_ARRAY, "pop_front", arrPopFront, "", "Variant", false);
        add(T_ARRAY, "front", arrFront, "", "Variant");
        add(T_ARRAY, "back", arrBack, "", "Variant");
        add(T_ARRAY, "has", arrHas, "Variant", "bool");
        add(T_ARRAY, "find", arrFind, "Variant int", "int");
        add(T_ARRAY, "count", arrCount, "Variant", "int");
        add(T_ARRAY, "erase", arrErase, "Variant", "", false);
        add(T_ARRAY, "remove_at", arrRemoveAt, "int", "", false);
        add(T_ARRAY, "insert", arrInsert, "int Variant", "int", false);
        add(T_ARRAY, "resize", arrResize, "int", "int", false);
        add(T_ARRAY, "append_array", arrAppendArray, "Array", "", false);
        add(T_ARRAY, "reverse", arrReverse, "", "", false);
        add(T_ARRAY, "fill", arrFill, "Variant", "", false);
        add(T_ARRAY, "hash", arrHash, "", "int");
        add(T_ARRAY, "sort", arrSort, "", "", false);
        add(T_ARRAY, "duplicate", arrDuplicate, "bool", "Array");
        add(T_DICTIONARY, "size", dictSize, "", "int");
        add(T_DICTIONARY, "is_empty", dictIsEmpty, "", "bool");
        add(T_DICTIONARY, "clear", dictClear, "", "", false);
        add(T_DICTIONARY, "has", dictHas, "Variant", "bool");
        add(T_DICTIONARY, "erase", dictErase, "Variant", "bool", false);
        add(T_DICTIONARY, "keys", dictKeys, "", "Array");
        add(T_DICTIONARY, "values", dictValues, "", "Array");
        add(T_DICTIONARY, "get", dictGet, "Variant Variant", "Variant");
        add(T_DICTIONARY, "duplicate", dictDuplicate, "bool", "Dictionary");
        add(T_DICTIONARY, "merge", dictMerge, "Dictionary bool", "", false);
        add(T_DICTIONARY, "hash", dictHash, "", "int");
        struct PK { int type; const char* elem; const char* self; };
        const PK pks[] = {{T_PACKED_BYTE, "int", "PackedByteArray"}, {T_PACKED_INT32, "int", "PackedInt32Array"},
                          {T_PACKED_INT64, "int", "PackedInt64Array"}, {T_PACKED_FLOAT32, "float", "PackedFloat32Array"},
                          {T_PACKED_FLOAT64, "float", "PackedFloat64Array"}, {T_PACKED_STRING, "String", "PackedStringArray"},
                          {T_PACKED_VECTOR2, "Vector2", "PackedVector2Array"}, {T_PACKED_VECTOR3, "Vector3", "PackedVector3Array"},
                          {T_PACKED_COLOR, "Color", "PackedColorArray"}, {T_PACKED_VECTOR4, "Vector4", "PackedVector4Array"}};
        for (const PK& p : pks) {
            // Static argument strings: one literal per element type.
            const char* setSig = p.type == T_PACKED_BYTE || p.type == T_PACKED_INT32 || p.type == T_PACKED_INT64 ? "int int"
                : p.type == T_PACKED_FLOAT32 || p.type == T_PACKED_FLOAT64 ? "int float" : p.type == T_PACKED_STRING ? "int String"
                : p.type == T_PACKED_VECTOR2 ? "int Vector2" : p.type == T_PACKED_VECTOR3 ? "int Vector3" : p.type == T_PACKED_COLOR ? "int Color" : "int Vector4";
            add(p.type, "size", pkSize, "", "int");
            add(p.type, "is_empty", pkIsEmpty, "", "bool");
            add(p.type, "clear", pkClear, "", "", false);
            add(p.type, "resize", pkResize, "int", "int", false);
            add(p.type, "push_back", pkPushBack, p.elem, "bool", false);
            add(p.type, "append", pkPushBack, p.elem, "bool", false);
            add(p.type, "set", pkSet, setSig, "", false);
            add(p.type, "get", pkGet, "int", p.elem);
            add(p.type, "has", pkHas, p.elem, "bool");
            add(p.type, "remove_at", pkRemoveAt, "int", "", false);
            add(p.type, "append_array", pkAppendArray, p.self, "", false);
            add(p.type, "duplicate", pkDuplicate, "", p.self, false);
        }
        add(T_CALLABLE, "call", callCallableM, "", "Variant", true, true);
        add(T_CALLABLE, "callv", callvCallable, "Array", "Variant");
        add(T_CALLABLE, "is_null", callableIsNull, "", "bool");
        add(T_CALLABLE, "is_valid", callableIsValid, "", "bool");
        add(T_CALLABLE, "get_object", callableGetObject, "", "Object");
        add(T_CALLABLE, "get_method", callableGetMethod, "", "StringName");
        add(T_CALLABLE, "is_custom", callableIsCustom, "", "bool");
        add(T_SIGNAL, "emit", signalEmit, "", "", true, true);
        add(T_SIGNAL, "connect", signalConnect, "Callable int", "int", false);
        add(T_SIGNAL, "disconnect", signalDisconnect, "Callable", "", false);
        add(T_SIGNAL, "is_null", signalIsNull, "", "bool");
        add(T_SIGNAL, "get_name", signalGetName, "", "StringName");
        add(T_SIGNAL, "get_object", signalGetObject, "", "Object");
        return m;
    }();
    return tbl;
}

const BuiltinMethodInfo* findBuiltinMethod(int type, std::string_view name) {
    for (const BuiltinMethodInfo& m : table())
        if (m.type == type && name == m.name) return &m;
    return nullptr;
}

size_t builtinMethodCount() { return table().size(); }
const BuiltinMethodInfo* builtinMethodAt(size_t i) { return i < table().size() ? &table()[i] : nullptr; }

bool builtinCallByName(int type, Var* self, std::string_view name, const Var* const* args, int argc, Var* r, GDExtensionCallError* err) {
    err->error = GDEXTENSION_CALL_OK; err->argument = 0; err->expected = 0;
    const BuiltinMethodInfo* m = findBuiltinMethod(type, name);
    if (!m) { err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; return false; }
    std::string sig = m->args;
    std::vector<int> want;
    for (size_t i = 0; i < sig.size();) {
        size_t j = sig.find(' ', i);
        if (j == std::string::npos) j = sig.size();
        want.push_back(typeFromName(std::string_view(sig).substr(i, j - i)));
        i = j + 1;
    }
    if (!m->vararg && argc < int(want.size())) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = int32_t(want.size()); return false; }
    if (!m->vararg && argc > int(want.size())) { err->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS; err->expected = int32_t(want.size()); return false; }
    std::vector<V> conv(size_t(std::max(argc, 1)));
    std::vector<const Var*> ptrs(size_t(std::max(argc, 1)), nullptr);
    for (int i = 0; i < argc; ++i) {
        ptrs[size_t(i)] = args[i];
        if (i >= int(want.size())) continue;
        const int w = want[size_t(i)];
        if (w == kAnyType || w == args[i]->type) continue;
        if (!varCanConvert(args[i]->type, w, false) || !varConvert(args[i], w, &conv[size_t(i)].v)) {
            err->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT; err->argument = i; err->expected = w;
            return false;
        }
        ptrs[size_t(i)] = &conv[size_t(i)].v;
    }
    m->fn(self, ptrs.data(), argc, r);
    return true;
}

// ---------------------------------------------------------------------------------------------------
// utility functions

static uint64_t& rngState() { static uint64_t s = 0x9E3779B97F4A7C15ULL; return s; }
static uint32_t nextRand() {
    uint64_t& x = rngState();
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return uint32_t(x >> 11);
}

static std::string joined(const Var* const* a, int n, const char* sep) {
    std::string s;
    for (int i = 0; i < n; ++i) { if (i) s += sep; s += varStringify(a[i]); }
    return s;
}

static void uPrint(Var*, const Var* const* a, int n, Var*) { hostLog(0, joined(a, n, "")); }
static void uPrints(Var*, const Var* const* a, int n, Var*) { hostLog(0, joined(a, n, " ")); }
static void uPrintt(Var*, const Var* const* a, int n, Var*) { hostLog(0, joined(a, n, "\t")); }
static void uPrintErr(Var*, const Var* const* a, int n, Var*) { hostLog(2, joined(a, n, "")); }
static void uPushError(Var*, const Var* const* a, int n, Var*) { hostLog(2, joined(a, n, "")); }
static void uPushWarning(Var*, const Var* const* a, int n, Var*) { hostLog(1, joined(a, n, "")); }
static void uStr(Var*, const Var* const* a, int n, Var* r) { ret(r, V::ofString(joined(a, n, ""))); }
static void uTypeof(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(a[0]->type)); }
static void uIsValid(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofBool(varObject(a[0]) != nullptr)); }
static void uIsIdValid(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofBool(objectById(uint64_t(inum(a[0]))) != nullptr)); }
static void uHash(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(varHash(a[0]))); }
#define MATH1(NAME, EXPR) static void NAME(Var*, const Var* const* a, int, Var* r) { double x = num(a[0]); ret(r, V::ofFloat(EXPR)); }
MATH1(uSin, std::sin(x)) MATH1(uCos, std::cos(x)) MATH1(uTan, std::tan(x)) MATH1(uAsin, std::asin(x)) MATH1(uAcos, std::acos(x))
MATH1(uAtan, std::atan(x)) MATH1(uSqrt, std::sqrt(x)) MATH1(uFloor, std::floor(x)) MATH1(uCeil, std::ceil(x)) MATH1(uRound, std::round(x))
MATH1(uAbsf, std::fabs(x)) MATH1(uDeg2Rad, x * 0.017453292519943295) MATH1(uRad2Deg, x * 57.29577951308232)
#undef MATH1
static void uAtan2(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::atan2(num(a[0]), num(a[1])))); }
static void uPow(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::pow(num(a[0]), num(a[1])))); }
static void uFmod(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::fmod(num(a[0]), num(a[1])))); }
static void uAbsi(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(std::llabs(inum(a[0])))); }
static void uSignf(Var*, const Var* const* a, int, Var* r) { double x = num(a[0]); ret(r, V::ofFloat(x > 0 ? 1 : x < 0 ? -1 : 0)); }
static void uSigni(Var*, const Var* const* a, int, Var* r) { int64_t x = inum(a[0]); ret(r, V::ofInt(x > 0 ? 1 : x < 0 ? -1 : 0)); }
static void uLerpf(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(num(a[0]) + (num(a[1]) - num(a[0])) * num(a[2]))); }
static void uClampf(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::min(std::max(num(a[0]), num(a[1])), num(a[2])))); }
static void uClampi(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(std::min(std::max(inum(a[0]), inum(a[1])), inum(a[2])))); }
static void uMinf(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::min(num(a[0]), num(a[1])))); }
static void uMaxf(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofFloat(std::max(num(a[0]), num(a[1])))); }
static void uMini(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(std::min(inum(a[0]), inum(a[1])))); }
static void uMaxi(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofInt(std::max(inum(a[0]), inum(a[1])))); }
static void uMinMax(const Var* const* a, int n, bool wantMin, Var* r) {
    if (n < 1) return;
    const Var* best = a[0];
    for (int i = 1; i < n; ++i) { bool ok; if (wantMin ? varLess(a[i], best, &ok) : varLess(best, a[i], &ok)) best = a[i]; }
    varAssign(r, best);
}
static void uMin(Var*, const Var* const* a, int n, Var* r) { uMinMax(a, n, true, r); }
static void uMax(Var*, const Var* const* a, int n, Var* r) { uMinMax(a, n, false, r); }
static void uPosmod(Var*, const Var* const* a, int, Var* r) { int64_t b = inum(a[1]); if (!b) { ret(r, V::ofInt(0)); return; } int64_t m = inum(a[0]) % b; if (m != 0 && ((m < 0) != (b < 0))) m += b; ret(r, V::ofInt(m)); }
static void uFposmod(Var*, const Var* const* a, int, Var* r) { double b = num(a[1]), m = std::fmod(num(a[0]), b); if (m != 0 && ((m < 0) != (b < 0))) m += b; ret(r, V::ofFloat(m)); }
static void uIsEqualApprox(Var*, const Var* const* a, int, Var* r) { double x = num(a[0]), y = num(a[1]); ret(r, V::ofBool(x == y || std::fabs(x - y) < std::max(1e-5 * std::fabs(x), 1e-6))); }
static void uIsZeroApprox(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofBool(std::fabs(num(a[0])) < 1e-6)); }
static void uIsNan(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofBool(std::isnan(num(a[0])))); }
static void uIsInf(Var*, const Var* const* a, int, Var* r) { ret(r, V::ofBool(std::isinf(num(a[0])))); }
static void uRandf(Var*, const Var* const*, int, Var* r) { ret(r, V::ofFloat(nextRand() / double(1u << 21))); }
static void uRandi(Var*, const Var* const*, int, Var* r) { ret(r, V::ofInt(int64_t(nextRand()) & 0xFFFFFFFF)); }
static void uRandfRange(Var*, const Var* const* a, int, Var* r) { double u = nextRand() / double(1u << 21); ret(r, V::ofFloat(num(a[0]) + (num(a[1]) - num(a[0])) * u)); }
static void uRandiRange(Var*, const Var* const* a, int, Var* r) {
    int64_t lo = inum(a[0]), hi = inum(a[1]);
    if (hi < lo) std::swap(lo, hi);
    ret(r, V::ofInt(lo + int64_t(nextRand()) % (hi - lo + 1)));
}
static void uRandomize(Var*, const Var* const*, int, Var*) { rngState() = std::random_device{}() | (uint64_t(std::random_device{}()) << 32) | 1; }
static void uSeed(Var*, const Var* const* a, int, Var*) { rngState() = uint64_t(inum(a[0])) * 0x9E3779B97F4A7C15ULL | 1; }

static const UtilityInfo kUtilities[] = {
    {"print", uPrint, "", "", true}, {"prints", uPrints, "", "", true}, {"printt", uPrintt, "", "", true},
    {"print_rich", uPrint, "", "", true}, {"printerr", uPrintErr, "", "", true}, {"printraw", uPrint, "", "", true},
    {"push_error", uPushError, "", "", true}, {"push_warning", uPushWarning, "", "", true}, {"str", uStr, "", "String", true},
    {"typeof", uTypeof, "Variant", "int", false}, {"is_instance_valid", uIsValid, "Variant", "bool", false},
    {"is_instance_id_valid", uIsIdValid, "int", "bool", false}, {"hash", uHash, "Variant", "int", false},
    {"sin", uSin, "float", "float", false}, {"cos", uCos, "float", "float", false}, {"tan", uTan, "float", "float", false},
    {"asin", uAsin, "float", "float", false}, {"acos", uAcos, "float", "float", false}, {"atan", uAtan, "float", "float", false},
    {"atan2", uAtan2, "float float", "float", false}, {"sqrt", uSqrt, "float", "float", false}, {"pow", uPow, "float float", "float", false},
    {"fmod", uFmod, "float float", "float", false}, {"floor", uFloor, "Variant", "Variant", false}, {"floorf", uFloor, "float", "float", false},
    {"ceil", uCeil, "Variant", "Variant", false}, {"ceilf", uCeil, "float", "float", false}, {"round", uRound, "Variant", "Variant", false},
    {"roundf", uRound, "float", "float", false}, {"absf", uAbsf, "float", "float", false}, {"absi", uAbsi, "int", "int", false},
    {"signf", uSignf, "float", "float", false}, {"signi", uSigni, "int", "int", false}, {"lerpf", uLerpf, "float float float", "float", false},
    {"clampf", uClampf, "float float float", "float", false}, {"clampi", uClampi, "int int int", "int", false},
    {"minf", uMinf, "float float", "float", false}, {"maxf", uMaxf, "float float", "float", false},
    {"mini", uMini, "int int", "int", false}, {"maxi", uMaxi, "int int", "int", false},
    {"min", uMin, "Variant Variant", "Variant", true}, {"max", uMax, "Variant Variant", "Variant", true},
    {"posmod", uPosmod, "int int", "int", false}, {"fposmod", uFposmod, "float float", "float", false},
    {"deg_to_rad", uDeg2Rad, "float", "float", false}, {"rad_to_deg", uRad2Deg, "float", "float", false},
    {"is_equal_approx", uIsEqualApprox, "float float", "bool", false}, {"is_zero_approx", uIsZeroApprox, "float", "bool", false},
    {"is_nan", uIsNan, "float", "bool", false}, {"is_inf", uIsInf, "float", "bool", false},
    {"randf", uRandf, "", "float", false}, {"randi", uRandi, "", "int", false}, {"randf_range", uRandfRange, "float float", "float", false},
    {"randi_range", uRandiRange, "int int", "int", false}, {"randomize", uRandomize, "", "", false}, {"seed", uSeed, "int", "", false},
};

const UtilityInfo* findUtility(std::string_view name) {
    for (const UtilityInfo& u : kUtilities)
        if (name == u.name) return &u;
    return nullptr;
}
size_t utilityCount() { return sizeof kUtilities / sizeof kUtilities[0]; }
const UtilityInfo* utilityAt(size_t i) { return i < utilityCount() ? &kUtilities[i] : nullptr; }

} // namespace aver::gdext
