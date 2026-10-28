// Operators, members, indexing and constructors of the builtin types (GdextValue.hpp).
#include "GdextHost.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

static void give(Var* r, V&& v) { *r = v.v; varNil(&v.v); }       // r uninitialised
static void giveNative(Var* r, int type, const void* native) { varSetNative(r, type, native); }

static bool isInt(const Var* v) { return v->type == T_INT; }
static bool isNum(const Var* v) { return v->type == T_INT || v->type == T_FLOAT; }
static double num(const Var* v) { return v->type == T_INT ? static_cast<double>(ld<int64_t>(v->d)) : v->type == T_FLOAT ? ld<double>(v->d) : v->type == T_BOOL ? v->d[0] : 0.0; }
static bool isText(int t) { return t == T_STRING || t == T_STRING_NAME || t == T_NODE_PATH; }
static std::u32string textU(const Var* v) {
    if (v->type == T_STRING_NAME) return utf8ToU32(nameText(ld<NameData*>(v->d)));
    auto* s = ld<StrData*>(v->d);
    return s ? s->s : std::u32string();
}

static std::string txt(const Var* v) { return isText(v->type) ? u32ToUtf8(textU(v)) : varStringify(v); }

struct VecKind { int comps; bool ints; };
static bool vecKind(int t, VecKind& k) {
    switch (t) {
        case T_VECTOR2: k = {2, false}; return true;
        case T_VECTOR2I: k = {2, true}; return true;
        case T_VECTOR3: k = {3, false}; return true;
        case T_VECTOR3I: k = {3, true}; return true;
        case T_VECTOR4: case T_QUATERNION: case T_COLOR: case T_PLANE: k = {4, false}; return true;
        case T_VECTOR4I: k = {4, true}; return true;
        default: return false;
    }
}

static void readVec(const Var* v, const VecKind& k, double* out) {
    for (int i = 0; i < k.comps; ++i)
        out[i] = k.ints ? static_cast<double>(ld<int32_t>(v->d + i * 4)) : static_cast<double>(ld<float>(v->d + i * 4));
}
static void giveVec(Var* r, int type, const VecKind& k, const double* c) {
    unsigned char buf[16] = {};
    for (int i = 0; i < k.comps; ++i) {
        if (k.ints) st<int32_t>(buf + i * 4, static_cast<int32_t>(c[i]));
        else st<float>(buf + i * 4, static_cast<float>(c[i]));
    }
    giveNative(r, type, buf);
}

// ---------------------------------------------------------------------------------------------------
// 3-D helpers (rows of a basis, Godot layout)

struct Vec3 { double x, y, z; };
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 sub3(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 norm3(Vec3 a) { double l = std::sqrt(dot(a, a)); return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a; }
static Vec3 v3(const void* p) { const float* f = static_cast<const float*>(p); return {f[0], f[1], f[2]}; }

struct Mat3 { double m[3][3]; };
static Mat3 mat3(const void* p) {
    const float* f = static_cast<const float*>(p);
    Mat3 r;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = f[i * 3 + j];
    return r;
}
static void putMat3(void* p, const Mat3& m) {
    float* f = static_cast<float*>(p);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) f[i * 3 + j] = static_cast<float>(m.m[i][j]);
}
static Vec3 xform(const Mat3& m, Vec3 v) {
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z, m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}
static Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
static Mat3 fromQuat(const float* q) {
    double d = double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3];
    double s = d > 0 ? 2.0 / d : 0.0;
    double xs = q[0] * s, ys = q[1] * s, zs = q[2] * s;
    double wx = q[3] * xs, wy = q[3] * ys, wz = q[3] * zs, xx = q[0] * xs, xy = q[0] * ys, xz = q[0] * zs, yy = q[1] * ys, yz = q[1] * zs, zz = q[2] * zs;
    return {{{1 - (yy + zz), xy - wz, xz + wy}, {xy + wz, 1 - (xx + zz), yz - wx}, {xz - wy, yz + wx, 1 - (xx + yy)}}};
}
static void toQuat(const Mat3& m, float* q) {
    double t = m.m[0][0] + m.m[1][1] + m.m[2][2], x, y, z, w;
    if (t > 0) {
        double s = std::sqrt(t + 1) * 2, i = 1 / s;
        x = (m.m[2][1] - m.m[1][2]) * i; y = (m.m[0][2] - m.m[2][0]) * i; z = (m.m[1][0] - m.m[0][1]) * i; w = s * 0.25;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        double s = std::sqrt(m.m[0][0] - m.m[1][1] - m.m[2][2] + 1) * 2, i = 1 / s;
        x = s * 0.25; y = (m.m[0][1] + m.m[1][0]) * i; z = (m.m[0][2] + m.m[2][0]) * i; w = (m.m[2][1] - m.m[1][2]) * i;
    } else if (m.m[1][1] > m.m[2][2]) {
        double s = std::sqrt(m.m[1][1] - m.m[0][0] - m.m[2][2] + 1) * 2, i = 1 / s;
        x = (m.m[0][1] + m.m[1][0]) * i; y = s * 0.25; z = (m.m[1][2] + m.m[2][1]) * i; w = (m.m[0][2] - m.m[2][0]) * i;
    } else {
        double s = std::sqrt(m.m[2][2] - m.m[0][0] - m.m[1][1] + 1) * 2, i = 1 / s;
        x = (m.m[0][2] + m.m[2][0]) * i; y = (m.m[1][2] + m.m[2][1]) * i; z = s * 0.25; w = (m.m[1][0] - m.m[0][1]) * i;
    }
    q[0] = float(x); q[1] = float(y); q[2] = float(z); q[3] = float(w);
}
static Mat3 axisAngle(Vec3 a, double phi) {
    double c = std::cos(phi), s = std::sin(phi), ax = a.x * a.x, ay = a.y * a.y, az = a.z * a.z;
    return {{{ax + c * (1 - ax), a.x * a.y * (1 - c) - a.z * s, a.z * a.x * (1 - c) + a.y * s},
             {a.x * a.y * (1 - c) + a.z * s, ay + c * (1 - ay), a.y * a.z * (1 - c) - a.x * s},
             {a.z * a.x * (1 - c) - a.y * s, a.y * a.z * (1 - c) + a.x * s, az + c * (1 - az)}}};
}

static void put3(Var* r, Vec3 v) { float f[3] = {float(v.x), float(v.y), float(v.z)}; giveNative(r, T_VECTOR3, f); }

// ---------------------------------------------------------------------------------------------------
// evaluate

static bool arithNumbers(int op, const Var* a, const Var* b, Var* r) {
    const bool ints = isInt(a) && isInt(b);
    if (ints) {
        int64_t x = ld<int64_t>(a->d), y = ld<int64_t>(b->d);
        auto ux = static_cast<uint64_t>(x), uy = static_cast<uint64_t>(y);
        switch (op) {
            case GDEXTENSION_VARIANT_OP_ADD: give(r, V::ofInt(static_cast<int64_t>(ux + uy))); return true;
            case GDEXTENSION_VARIANT_OP_SUBTRACT: give(r, V::ofInt(static_cast<int64_t>(ux - uy))); return true;
            case GDEXTENSION_VARIANT_OP_MULTIPLY: give(r, V::ofInt(static_cast<int64_t>(ux * uy))); return true;
            case GDEXTENSION_VARIANT_OP_DIVIDE: if (!y || (x == INT64_MIN && y == -1)) return false; give(r, V::ofInt(x / y)); return true;
            case GDEXTENSION_VARIANT_OP_MODULE: if (!y || (x == INT64_MIN && y == -1)) return false; give(r, V::ofInt(x % y)); return true;
            case GDEXTENSION_VARIANT_OP_POWER: give(r, V::ofInt(static_cast<int64_t>(std::pow(double(x), double(y))))); return true;
            default: return false;
        }
    }
    double x = num(a), y = num(b);
    switch (op) {
        case GDEXTENSION_VARIANT_OP_ADD: give(r, V::ofFloat(x + y)); return true;
        case GDEXTENSION_VARIANT_OP_SUBTRACT: give(r, V::ofFloat(x - y)); return true;
        case GDEXTENSION_VARIANT_OP_MULTIPLY: give(r, V::ofFloat(x * y)); return true;
        case GDEXTENSION_VARIANT_OP_DIVIDE: give(r, V::ofFloat(x / y)); return true;
        case GDEXTENSION_VARIANT_OP_POWER: give(r, V::ofFloat(std::pow(x, y))); return true;
        default: return false;
    }
}

static bool arithVectors(int op, const Var* a, const Var* b, Var* r) {
    VecKind ka{}, kb{};
    const bool va = vecKind(a->type, ka), vb = vecKind(b->type, kb);
    double x[4] = {}, y[4] = {}, o[4] = {};
    if (va && vb && a->type == b->type) {
        readVec(a, ka, x); readVec(b, kb, y);
        for (int i = 0; i < ka.comps; ++i) {
            switch (op) {
                case GDEXTENSION_VARIANT_OP_ADD: o[i] = x[i] + y[i]; break;
                case GDEXTENSION_VARIANT_OP_SUBTRACT: o[i] = x[i] - y[i]; break;
                case GDEXTENSION_VARIANT_OP_MULTIPLY: o[i] = x[i] * y[i]; break;
                case GDEXTENSION_VARIANT_OP_DIVIDE:
                    if (ka.ints && y[i] == 0) return false;
                    o[i] = x[i] / y[i];
                    break;
                default: return false;
            }
        }
        if (a->type == T_QUATERNION && op == GDEXTENSION_VARIANT_OP_MULTIPLY) {
            o[0] = x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1];
            o[1] = x[3] * y[1] + x[1] * y[3] + x[2] * y[0] - x[0] * y[2];
            o[2] = x[3] * y[2] + x[2] * y[3] + x[0] * y[1] - x[1] * y[0];
            o[3] = x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2];
        } else if (a->type == T_QUATERNION && op == GDEXTENSION_VARIANT_OP_DIVIDE) {
            return false;
        } else if (a->type == T_PLANE) {
            return false;
        }
        giveVec(r, a->type, ka, o);
        return true;
    }
    if (va && isNum(b) && a->type != T_PLANE && (op == GDEXTENSION_VARIANT_OP_MULTIPLY || op == GDEXTENSION_VARIANT_OP_DIVIDE)) {
        if (ka.ints && !isInt(b)) return false;
        readVec(a, ka, x);
        double s = num(b);
        if (op == GDEXTENSION_VARIANT_OP_DIVIDE && ka.ints && s == 0) return false;
        for (int i = 0; i < ka.comps; ++i) o[i] = op == GDEXTENSION_VARIANT_OP_MULTIPLY ? x[i] * s : x[i] / s;
        giveVec(r, a->type, ka, o);
        return true;
    }
    if (vb && isNum(a) && op == GDEXTENSION_VARIANT_OP_MULTIPLY && b->type != T_PLANE) {
        if (kb.ints && !isInt(a)) return false;
        readVec(b, kb, y);
        double s = num(a);
        for (int i = 0; i < kb.comps; ++i) o[i] = y[i] * s;
        giveVec(r, b->type, kb, o);
        return true;
    }
    return false;
}

static bool arithTransforms(int op, const Var* a, const Var* b, Var* r) {
    if (op != GDEXTENSION_VARIANT_OP_MULTIPLY) return false;
    if (a->type == T_BASIS && b->type == T_VECTOR3) { put3(r, xform(mat3(varPtr(a)), v3(varPtr(b)))); return true; }
    if (a->type == T_BASIS && b->type == T_BASIS) {
        unsigned char buf[36];
        putMat3(buf, mul(mat3(varPtr(a)), mat3(varPtr(b))));
        giveNative(r, T_BASIS, buf);
        return true;
    }
    if (a->type == T_TRANSFORM3D && b->type == T_VECTOR3) {
        const float* t = static_cast<const float*>(varPtr(a));
        Vec3 v = xform(mat3(t), v3(varPtr(b)));
        put3(r, {v.x + t[9], v.y + t[10], v.z + t[11]});
        return true;
    }
    if (a->type == T_TRANSFORM3D && b->type == T_TRANSFORM3D) {
        const float* ta = static_cast<const float*>(varPtr(a));
        const float* tb = static_cast<const float*>(varPtr(b));
        float out[12];
        putMat3(out, mul(mat3(ta), mat3(tb)));
        Vec3 o = xform(mat3(ta), {tb[9], tb[10], tb[11]});
        out[9] = float(o.x + ta[9]); out[10] = float(o.y + ta[10]); out[11] = float(o.z + ta[11]);
        giveNative(r, T_TRANSFORM3D, out);
        return true;
    }
    if (a->type == T_QUATERNION && b->type == T_VECTOR3) {
        const float* q = static_cast<const float*>(varPtr(a));
        Vec3 u{q[0], q[1], q[2]}, v = v3(varPtr(b));
        Vec3 uv = cross(u, v);
        Vec3 uuv = cross(u, uv);
        put3(r, {v.x + 2 * (q[3] * uv.x + uuv.x), v.y + 2 * (q[3] * uv.y + uuv.y), v.z + 2 * (q[3] * uv.z + uuv.z)});
        return true;
    }
    return false;
}

bool varEvaluate(int op, const Var* a, const Var* b, Var* r) {
    using namespace std::string_literals;
    switch (op) {
        case GDEXTENSION_VARIANT_OP_EQUAL:
        case GDEXTENSION_VARIANT_OP_NOT_EQUAL: {
            bool eq = a->type == T_NIL || b->type == T_NIL ? (a->type == b->type || (a->type == T_OBJECT && !varObject(a)) ||
                                                                (b->type == T_OBJECT && !varObject(b)))
                                                              : varEqual(a, b);
            give(r, V::ofBool(op == GDEXTENSION_VARIANT_OP_EQUAL ? eq : !eq));
            return true;
        }
        case GDEXTENSION_VARIANT_OP_LESS:
        case GDEXTENSION_VARIANT_OP_LESS_EQUAL:
        case GDEXTENSION_VARIANT_OP_GREATER:
        case GDEXTENSION_VARIANT_OP_GREATER_EQUAL: {
            bool valid = true;
            const bool swap = op == GDEXTENSION_VARIANT_OP_GREATER || op == GDEXTENSION_VARIANT_OP_GREATER_EQUAL;
            const bool orEq = op == GDEXTENSION_VARIANT_OP_LESS_EQUAL || op == GDEXTENSION_VARIANT_OP_GREATER_EQUAL;
            bool lt = swap ? varLess(b, a, &valid) : varLess(a, b, &valid);
            if (!valid) return false;
            give(r, V::ofBool(lt || (orEq && varEqual(a, b))));
            return true;
        }
        case GDEXTENSION_VARIANT_OP_AND: give(r, V::ofBool(varBooleanize(a) && varBooleanize(b))); return true;
        case GDEXTENSION_VARIANT_OP_OR: give(r, V::ofBool(varBooleanize(a) || varBooleanize(b))); return true;
        case GDEXTENSION_VARIANT_OP_XOR: give(r, V::ofBool(varBooleanize(a) != varBooleanize(b))); return true;
        case GDEXTENSION_VARIANT_OP_NOT: give(r, V::ofBool(!varBooleanize(a))); return true;
        case GDEXTENSION_VARIANT_OP_IN: {
            if (b->type == T_ARRAY) {
                for (const Var& e : ld<ArrayData*>(b->d)->items) if (varEqual(&e, a)) { give(r, V::ofBool(true)); return true; }
                give(r, V::ofBool(false));
                return true;
            }
            if (b->type == T_DICTIONARY) { give(r, V::ofBool(dictFind(ld<DictData*>(b->d), a) >= 0)); return true; }
            if (isText(b->type) && isText(a->type)) { give(r, V::ofBool(textU(b).find(textU(a)) != std::u32string::npos)); return true; }
            if (PackedData* p = packedOf(b)) {
                bool found = false;
                for (size_t i = 0; i < p->count() && !found; ++i) {
                    Var e; varNil(&e);
                    bool oob = false;
                    varGetIndexed(b, static_cast<int64_t>(i), &e, &oob);
                    found = varEqual(&e, a);
                    varDestroy(&e);
                }
                give(r, V::ofBool(found));
                return true;
            }
            return false;
        }
        case GDEXTENSION_VARIANT_OP_NEGATE:
        case GDEXTENSION_VARIANT_OP_POSITIVE: {
            const bool neg = op == GDEXTENSION_VARIANT_OP_NEGATE;
            if (a->type == T_INT) { int64_t x = ld<int64_t>(a->d); give(r, V::ofInt(neg ? static_cast<int64_t>(0 - static_cast<uint64_t>(x)) : x)); return true; }
            if (a->type == T_FLOAT) { double x = ld<double>(a->d); give(r, V::ofFloat(neg ? -x : x)); return true; }
            VecKind k;
            if (vecKind(a->type, k) && a->type != T_PLANE) {
                double c[4];
                readVec(a, k, c);
                if (neg) for (int i = 0; i < k.comps; ++i) c[i] = -c[i];
                giveVec(r, a->type, k, c);
                return true;
            }
            return false;
        }
        case GDEXTENSION_VARIANT_OP_BIT_NEGATE:
            if (!isInt(a)) return false;
            give(r, V::ofInt(~ld<int64_t>(a->d)));
            return true;
        case GDEXTENSION_VARIANT_OP_SHIFT_LEFT:
        case GDEXTENSION_VARIANT_OP_SHIFT_RIGHT:
        case GDEXTENSION_VARIANT_OP_BIT_AND:
        case GDEXTENSION_VARIANT_OP_BIT_OR:
        case GDEXTENSION_VARIANT_OP_BIT_XOR: {
            if (!isInt(a) || !isInt(b)) return false;
            int64_t x = ld<int64_t>(a->d), y = ld<int64_t>(b->d);
            switch (op) {
                case GDEXTENSION_VARIANT_OP_SHIFT_LEFT: if (y < 0 || y > 63) return false; give(r, V::ofInt(static_cast<int64_t>(static_cast<uint64_t>(x) << y))); break;
                case GDEXTENSION_VARIANT_OP_SHIFT_RIGHT: if (y < 0 || y > 63) return false; give(r, V::ofInt(x >> y)); break;
                case GDEXTENSION_VARIANT_OP_BIT_AND: give(r, V::ofInt(x & y)); break;
                case GDEXTENSION_VARIANT_OP_BIT_OR: give(r, V::ofInt(x | y)); break;
                default: give(r, V::ofInt(x ^ y)); break;
            }
            return true;
        }
        default: break;
    }
    // ADD SUBTRACT MULTIPLY DIVIDE MODULE POWER
    if (isNum(a) && isNum(b)) return arithNumbers(op, a, b, r);
    if (op == GDEXTENSION_VARIANT_OP_ADD) {
        if (isText(a->type) && isText(b->type) && (a->type == T_STRING || b->type == T_STRING)) {
            give(r, V::ofString(u32ToUtf8(textU(a) + textU(b))));
            return true;
        }
        if (a->type == T_ARRAY && b->type == T_ARRAY) {
            V out = makeArray();
            ArrayData* o = ld<ArrayData*>(out.v.d);
            for (const Var* src : {a, b})
                for (const Var& e : ld<ArrayData*>(src->d)->items) { o->items.emplace_back(); varCopy(&o->items.back(), &e); }
            give(r, std::move(out));
            return true;
        }
        if (a->type == b->type && packedOf(a)) {
            V out = makePacked(a->type);
            PackedData* o = ld<PackedData*>(out.v.d);
            const PackedData* x = packedOf(a); const PackedData* y = packedOf(b);
            o->bytes = x->bytes;
            o->bytes.insert(o->bytes.end(), y->bytes.begin(), y->bytes.end());
            if (a->type == T_PACKED_STRING)
                for (size_t i = 0; i < o->count(); ++i) {
                    auto* s = ld<StrData*>(o->bytes.data() + i * 8);
                    st<StrData*>(o->bytes.data() + i * 8, s ? new StrData{s->s} : nullptr);
                }
            give(r, std::move(out));
            return true;
        }
    }
    if (arithVectors(op, a, b, r)) return true;
    return arithTransforms(op, a, b, r);
}

// ---------------------------------------------------------------------------------------------------
// members

struct MemberDesc { int base; const char* name; int mtype; int off; };
static const MemberDesc kMembers[] = {
    {T_VECTOR2, "x", T_FLOAT, 0}, {T_VECTOR2, "y", T_FLOAT, 1},
    {T_VECTOR2I, "x", T_INT, 0}, {T_VECTOR2I, "y", T_INT, 1},
    {T_VECTOR3, "x", T_FLOAT, 0}, {T_VECTOR3, "y", T_FLOAT, 1}, {T_VECTOR3, "z", T_FLOAT, 2},
    {T_VECTOR3I, "x", T_INT, 0}, {T_VECTOR3I, "y", T_INT, 1}, {T_VECTOR3I, "z", T_INT, 2},
    {T_VECTOR4, "x", T_FLOAT, 0}, {T_VECTOR4, "y", T_FLOAT, 1}, {T_VECTOR4, "z", T_FLOAT, 2}, {T_VECTOR4, "w", T_FLOAT, 3},
    {T_VECTOR4I, "x", T_INT, 0}, {T_VECTOR4I, "y", T_INT, 1}, {T_VECTOR4I, "z", T_INT, 2}, {T_VECTOR4I, "w", T_INT, 3},
    {T_QUATERNION, "x", T_FLOAT, 0}, {T_QUATERNION, "y", T_FLOAT, 1}, {T_QUATERNION, "z", T_FLOAT, 2}, {T_QUATERNION, "w", T_FLOAT, 3},
    {T_COLOR, "r", T_FLOAT, 0}, {T_COLOR, "g", T_FLOAT, 1}, {T_COLOR, "b", T_FLOAT, 2}, {T_COLOR, "a", T_FLOAT, 3},
    {T_PLANE, "x", T_FLOAT, 0}, {T_PLANE, "y", T_FLOAT, 1}, {T_PLANE, "z", T_FLOAT, 2}, {T_PLANE, "d", T_FLOAT, 3},
    {T_PLANE, "normal", T_VECTOR3, 0},
    {T_RECT2, "position", T_VECTOR2, 0}, {T_RECT2, "size", T_VECTOR2, 2},
    {T_RECT2I, "position", T_VECTOR2I, 0}, {T_RECT2I, "size", T_VECTOR2I, 2},
    {T_AABB, "position", T_VECTOR3, 0}, {T_AABB, "size", T_VECTOR3, 3},
    {T_TRANSFORM2D, "x", T_VECTOR2, 0}, {T_TRANSFORM2D, "y", T_VECTOR2, 2}, {T_TRANSFORM2D, "origin", T_VECTOR2, 4},
    {T_TRANSFORM3D, "basis", T_BASIS, 0}, {T_TRANSFORM3D, "origin", T_VECTOR3, 9},
    {T_PROJECTION, "x", T_VECTOR4, 0}, {T_PROJECTION, "y", T_VECTOR4, 4}, {T_PROJECTION, "z", T_VECTOR4, 8}, {T_PROJECTION, "w", T_VECTOR4, 12},
};

static const MemberDesc* findMember(int base, std::string_view name) {
    for (const MemberDesc& m : kMembers)
        if (m.base == base && name == m.name) return &m;
    return nullptr;
}

static int basisColumn(std::string_view name) { return name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1; }

bool varGetMember(const Var* base, std::string_view name, Var* out) {
    if (base->type == T_BASIS) {
        int c = basisColumn(name);
        if (c < 0) return false;
        const float* f = static_cast<const float*>(varPtr(base));
        float col[3] = {f[c], f[3 + c], f[6 + c]};
        giveNative(out, T_VECTOR3, col);
        return true;
    }
    if ((base->type == T_RECT2 || base->type == T_RECT2I || base->type == T_AABB) && name == "end") {
        const bool ints = base->type == T_RECT2I;
        const int n = base->type == T_AABB ? 3 : 2;
        const unsigned char* p = static_cast<const unsigned char*>(varPtr(base));
        unsigned char buf[12];
        for (int i = 0; i < n; ++i) {
            if (ints) st<int32_t>(buf + i * 4, ld<int32_t>(p + i * 4) + ld<int32_t>(p + (n + i) * 4));
            else st<float>(buf + i * 4, ld<float>(p + i * 4) + ld<float>(p + (n + i) * 4));
        }
        giveNative(out, ints ? T_VECTOR2I : n == 3 ? T_VECTOR3 : T_VECTOR2, buf);
        return true;
    }
    const MemberDesc* m = findMember(base->type, name);
    if (!m) return false;
    const unsigned char* p = static_cast<const unsigned char*>(varPtr(base)) + m->off * 4;
    if (m->mtype == T_FLOAT) { give(out, V::ofFloat(ld<float>(p))); return true; }
    if (m->mtype == T_INT) { give(out, V::ofInt(ld<int32_t>(p))); return true; }
    giveNative(out, m->mtype, p);
    return true;
}

bool varSetMember(Var* base, std::string_view name, const Var* value) {
    if (base->type == T_BASIS) {
        int c = basisColumn(name);
        if (c < 0 || value->type != T_VECTOR3) return false;
        float* f = static_cast<float*>(varPtr(base));
        const float* v = static_cast<const float*>(varPtr(value));
        f[c] = v[0]; f[3 + c] = v[1]; f[6 + c] = v[2];
        return true;
    }
    const MemberDesc* m = findMember(base->type, name);
    if (!m) return false;
    unsigned char* p = static_cast<unsigned char*>(varPtr(base)) + m->off * 4;
    if (m->mtype == T_FLOAT) { if (!isNum(value)) return false; st<float>(p, static_cast<float>(num(value))); return true; }
    if (m->mtype == T_INT) { if (!isNum(value)) return false; st<int32_t>(p, static_cast<int32_t>(num(value))); return true; }
    if (value->type != m->mtype) return false;
    std::memcpy(p, varPtr(value), nativeSize(m->mtype));
    return true;
}

// ---------------------------------------------------------------------------------------------------
// indexing

static int64_t wrapIndex(int64_t i, size_t n) { return i < 0 ? i + static_cast<int64_t>(n) : i; }

bool varGetIndexed(const Var* base, int64_t index, Var* out, bool* oob) {
    *oob = false;
    VecKind k;
    if (vecKind(base->type, k)) {
        if (index < 0 || index >= k.comps) { *oob = true; return false; }
        if (k.ints) give(out, V::ofInt(ld<int32_t>(base->d + index * 4)));
        else give(out, V::ofFloat(ld<float>(base->d + index * 4)));
        return true;
    }
    switch (base->type) {
        case T_BASIS: {
            if (index < 0 || index > 2) { *oob = true; return false; }
            const float* f = static_cast<const float*>(varPtr(base));
            float col[3] = {f[index], f[3 + index], f[6 + index]};
            giveNative(out, T_VECTOR3, col);
            return true;
        }
        case T_TRANSFORM2D: {
            if (index < 0 || index > 2) { *oob = true; return false; }
            giveNative(out, T_VECTOR2, static_cast<const float*>(varPtr(base)) + index * 2);
            return true;
        }
        case T_PROJECTION: {
            if (index < 0 || index > 3) { *oob = true; return false; }
            giveNative(out, T_VECTOR4, static_cast<const float*>(varPtr(base)) + index * 4);
            return true;
        }
        case T_ARRAY: {
            const ArrayData* a = ld<ArrayData*>(base->d);
            int64_t i = wrapIndex(index, a->items.size());
            if (i < 0 || i >= static_cast<int64_t>(a->items.size())) { *oob = true; return false; }
            varCopy(out, &a->items[static_cast<size_t>(i)]);
            return true;
        }
        case T_STRING: case T_STRING_NAME: case T_NODE_PATH: {
            std::u32string s = textU(base);
            int64_t i = wrapIndex(index, s.size());
            if (i < 0 || i >= static_cast<int64_t>(s.size())) { *oob = true; return false; }
            give(out, V::ofString(u32ToUtf8(std::u32string(1, s[static_cast<size_t>(i)]))));
            return true;
        }
        default: break;
    }
    if (const PackedData* p = packedOf(base)) {
        int64_t i = wrapIndex(index, p->count());
        if (i < 0 || i >= static_cast<int64_t>(p->count())) { *oob = true; return false; }
        const unsigned char* e = p->bytes.data() + static_cast<size_t>(i) * p->esize;
        switch (p->type) {
            case T_PACKED_BYTE: give(out, V::ofInt(*e)); break;
            case T_PACKED_INT32: give(out, V::ofInt(ld<int32_t>(e))); break;
            case T_PACKED_INT64: give(out, V::ofInt(ld<int64_t>(e))); break;
            case T_PACKED_FLOAT32: give(out, V::ofFloat(ld<float>(e))); break;
            case T_PACKED_FLOAT64: give(out, V::ofFloat(ld<double>(e))); break;
            case T_PACKED_STRING: { auto* s = ld<StrData*>(e); give(out, V::ofString(s ? u32ToUtf8(s->s) : std::string())); break; }
            case T_PACKED_VECTOR2: giveNative(out, T_VECTOR2, e); break;
            case T_PACKED_VECTOR3: giveNative(out, T_VECTOR3, e); break;
            case T_PACKED_COLOR: giveNative(out, T_COLOR, e); break;
            default: giveNative(out, T_VECTOR4, e); break;
        }
        return true;
    }
    return false;
}

bool varSetIndexed(Var* base, int64_t index, const Var* value, bool* oob) {
    *oob = false;
    VecKind k;
    if (vecKind(base->type, k)) {
        if (index < 0 || index >= k.comps) { *oob = true; return false; }
        if (!isNum(value)) return false;
        if (k.ints) st<int32_t>(base->d + index * 4, static_cast<int32_t>(num(value)));
        else st<float>(base->d + index * 4, static_cast<float>(num(value)));
        return true;
    }
    if (base->type == T_ARRAY) {
        ArrayData* a = ld<ArrayData*>(base->d);
        int64_t i = wrapIndex(index, a->items.size());
        if (i < 0 || i >= static_cast<int64_t>(a->items.size())) { *oob = true; return false; }
        varAssign(&a->items[static_cast<size_t>(i)], value);
        return true;
    }
    if (PackedData* p = packedOf(base)) {
        int64_t i = wrapIndex(index, p->count());
        if (i < 0 || i >= static_cast<int64_t>(p->count())) { *oob = true; return false; }
        PackedData* w = p;
        packedDetach(w);
        if (w != p) st<PackedData*>(base->d, w);
        unsigned char* e = w->bytes.data() + static_cast<size_t>(i) * w->esize;
        switch (w->type) {
            case T_PACKED_BYTE: if (!isNum(value)) return false; *e = static_cast<unsigned char>(num(value)); break;
            case T_PACKED_INT32: if (!isNum(value)) return false; st<int32_t>(e, static_cast<int32_t>(num(value))); break;
            case T_PACKED_INT64: if (!isNum(value)) return false; st<int64_t>(e, static_cast<int64_t>(num(value))); break;
            case T_PACKED_FLOAT32: if (!isNum(value)) return false; st<float>(e, static_cast<float>(num(value))); break;
            case T_PACKED_FLOAT64: if (!isNum(value)) return false; st<double>(e, num(value)); break;
            case T_PACKED_STRING: {
                if (!isText(value->type)) return false;
                delete ld<StrData*>(e);
                st<StrData*>(e, new StrData{textU(value)});
                break;
            }
            default: {
                static const int want[] = {0, 0, 0, 0, 0, 0, T_VECTOR2, T_VECTOR3, T_COLOR, T_VECTOR4};
                if (value->type != want[w->type - T_PACKED_BYTE]) return false;
                std::memcpy(e, varPtr(value), w->esize);
                break;
            }
        }
        return true;
    }
    return false;
}

bool varGetKeyed(const Var* base, const Var* key, Var* out) {
    if (base->type == T_DICTIONARY) {
        const DictData* d = ld<DictData*>(base->d);
        int i = dictFind(d, key);
        if (i < 0) return false;
        varCopy(out, &d->items[static_cast<size_t>(i)].second);
        return true;
    }
    if (base->type == T_OBJECT) {
        HostObject* o = varObject(base);
        return o && isText(key->type) && getProperty(o, txt(key), out);
    }
    if (isNum(key)) { bool oob; return varGetIndexed(base, static_cast<int64_t>(num(key)), out, &oob); }
    if (isText(key->type)) return varGetMember(base, txt(key), out);
    return false;
}

bool varSetKeyed(Var* base, const Var* key, const Var* value) {
    if (base->type == T_DICTIONARY) {
        varAssign(dictSlot(ld<DictData*>(base->d), key), value);
        return true;
    }
    if (base->type == T_OBJECT) {
        HostObject* o = varObject(base);
        return o && isText(key->type) && setProperty(o, txt(key), value);
    }
    if (isNum(key)) { bool oob; return varSetIndexed(base, static_cast<int64_t>(num(key)), value, &oob); }
    if (isText(key->type)) return varSetMember(base, txt(key), value);
    return false;
}

bool varHasKey(const Var* base, const Var* key, bool* valid) {
    *valid = true;
    if (base->type == T_DICTIONARY) return dictFind(ld<DictData*>(base->d), key) >= 0;
    if (base->type == T_ARRAY) {
        if (!isNum(key)) { *valid = false; return false; }
        int64_t i = static_cast<int64_t>(num(key)), n = static_cast<int64_t>(ld<ArrayData*>(base->d)->items.size());
        return i >= -n && i < n;
    }
    *valid = false;
    return false;
}

// ---------------------------------------------------------------------------------------------------
// constructors

struct CtorRow { int type; const char* sigs[13]; };
static const CtorRow kCtors[] = {
    {T_NIL, {"", "Variant"}},
    {T_BOOL, {"", "bool", "int", "float"}},
    {T_INT, {"", "int", "float", "bool", "String"}},
    {T_FLOAT, {"", "float", "int", "bool", "String"}},
    {T_STRING, {"", "String", "StringName", "NodePath"}},
    {T_VECTOR2, {"", "Vector2", "Vector2i", "float float"}},
    {T_VECTOR2I, {"", "Vector2i", "Vector2", "int int"}},
    {T_RECT2, {"", "Rect2", "Rect2i", "Vector2 Vector2", "float float float float"}},
    {T_RECT2I, {"", "Rect2i", "Rect2", "Vector2i Vector2i", "int int int int"}},
    {T_VECTOR3, {"", "Vector3", "Vector3i", "float float float"}},
    {T_VECTOR3I, {"", "Vector3i", "Vector3", "int int int"}},
    {T_TRANSFORM2D, {"", "Transform2D", "float Vector2", "float Vector2 float Vector2", "Vector2 Vector2 Vector2"}},
    {T_VECTOR4, {"", "Vector4", "Vector4i", "float float float float"}},
    {T_VECTOR4I, {"", "Vector4i", "Vector4", "int int int int"}},
    {T_PLANE, {"", "Plane", "Vector3", "Vector3 float", "Vector3 Vector3", "Vector3 Vector3 Vector3", "float float float float"}},
    {T_QUATERNION, {"", "Quaternion", "Basis", "Vector3 float", "Vector3 Vector3", "float float float float"}},
    {T_AABB, {"", "AABB", "Vector3 Vector3"}},
    {T_BASIS, {"", "Basis", "Quaternion", "Vector3 float", "Vector3 Vector3 Vector3"}},
    {T_TRANSFORM3D, {"", "Transform3D", "Basis Vector3", "Vector3 Vector3 Vector3 Vector3", "Projection"}},
    {T_PROJECTION, {"", "Projection", "Transform3D", "Vector4 Vector4 Vector4 Vector4"}},
    {T_COLOR, {"", "Color", "Color float", "float float float", "float float float float", "String", "String float"}},
    {T_STRING_NAME, {"", "StringName", "String"}},
    {T_NODE_PATH, {"", "NodePath", "String"}},
    {T_RID, {"", "RID"}},
    {T_CALLABLE, {"", "Callable", "Object StringName"}},
    {T_SIGNAL, {"", "Signal", "Object StringName"}},
    {T_DICTIONARY, {"", "Dictionary", "Dictionary int StringName Variant int StringName Variant"}},
    {T_ARRAY, {"", "Array", "Array int StringName Variant", "PackedByteArray", "PackedInt32Array", "PackedInt64Array",
               "PackedFloat32Array", "PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array",
               "PackedColorArray", "PackedVector4Array"}},
    {T_PACKED_BYTE, {"", "PackedByteArray", "Array"}},
    {T_PACKED_INT32, {"", "PackedInt32Array", "Array"}},
    {T_PACKED_INT64, {"", "PackedInt64Array", "Array"}},
    {T_PACKED_FLOAT32, {"", "PackedFloat32Array", "Array"}},
    {T_PACKED_FLOAT64, {"", "PackedFloat64Array", "Array"}},
    {T_PACKED_STRING, {"", "PackedStringArray", "Array"}},
    {T_PACKED_VECTOR2, {"", "PackedVector2Array", "Array"}},
    {T_PACKED_VECTOR3, {"", "PackedVector3Array", "Array"}},
    {T_PACKED_COLOR, {"", "PackedColorArray", "Array"}},
    {T_PACKED_VECTOR4, {"", "PackedVector4Array", "Array"}},
};

const char* constructorArgs(int type, int index) {
    for (const CtorRow& r : kCtors) {
        if (r.type != type) continue;
        if (index < 0 || index >= 13) return nullptr;
        return r.sigs[index];
    }
    return nullptr;
}

static std::vector<int> sigTypes(const char* sig) {
    std::vector<int> t;
    std::string s = sig ? sig : "";
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) t.push_back(typeFromName(std::string_view(s).substr(i, j - i)));
        i = j + 1;
    }
    return t;
}

static double F(const Var* v) { return num(v); }
static void putF(Var* out, int type, std::initializer_list<double> c) {
    float buf[16] = {};
    size_t i = 0;
    for (double d : c) buf[i++] = static_cast<float>(d);
    giveNative(out, type, buf);
}
static void putI(Var* out, int type, std::initializer_list<int64_t> c) {
    int32_t buf[4] = {};
    size_t i = 0;
    for (int64_t d : c) buf[i++] = static_cast<int32_t>(d);
    giveNative(out, type, buf);
}
static void copyAs(Var* out, const Var* a) { varCopy(out, a); }
static void defaultOf(Var* out, int type) {
    std::vector<unsigned char> buf(std::max<size_t>(nativeSize(type), 16));
    nativeInit(type, buf.data());
    varSetNative(out, type, buf.data());
    if (!nativeIsHeap(type) && type != T_NIL) nativeDestroy(type, buf.data()); // varSetNative copied; drop the temporary's reference
}
static void convertVec(Var* out, const Var* a, int toType) {
    VecKind ka{}, kb{};
    vecKind(a->type, ka);
    vecKind(toType, kb);
    double c[4];
    readVec(a, ka, c);
    if (kb.ints) for (int i = 0; i < kb.comps; ++i) c[i] = std::trunc(c[i]);
    giveVec(out, toType, kb, c);
}

static int parseHexDigit(char32_t c) { return c >= '0' && c <= '9' ? int(c - '0') : c >= 'a' && c <= 'f' ? int(c - 'a' + 10) : c >= 'A' && c <= 'F' ? int(c - 'A' + 10) : -1; }

static bool colorFromText(const std::u32string& s0, double alpha, float* out) {
    std::u32string s = s0;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) return false;
    int v[8];
    for (size_t i = 0; i < s.size(); ++i) if ((v[i] = parseHexDigit(s[i])) < 0) return false;
    auto two = [&](size_t i) { return (v[i] * 16 + v[i + 1]) / 255.0f; };
    if (s.size() <= 4) {
        out[0] = v[0] * 17 / 255.0f; out[1] = v[1] * 17 / 255.0f; out[2] = v[2] * 17 / 255.0f;
        out[3] = s.size() == 4 ? v[3] * 17 / 255.0f : static_cast<float>(alpha);
    } else {
        out[0] = two(0); out[1] = two(2); out[2] = two(4);
        out[3] = s.size() == 8 ? two(6) : static_cast<float>(alpha);
    }
    return true;
}

static void packedFromArray(Var* out, int type, const ArrayData* a) {
    V p = makePacked(type);
    PackedData* d = ld<PackedData*>(p.v.d);
    d->bytes.assign(a->items.size() * d->esize, 0);
    for (size_t i = 0; i < a->items.size(); ++i) {
        bool oob;
        if (!varSetIndexed(&p.v, static_cast<int64_t>(i), &a->items[i], &oob)) continue;
        d = ld<PackedData*>(p.v.d);
    }
    give(out, std::move(p));
}

static void arrayFromPacked(Var* out, const Var* p) {
    V arr = makeArray();
    ArrayData* a = ld<ArrayData*>(arr.v.d);
    const size_t n = packedOf(p)->count();
    for (size_t i = 0; i < n; ++i) {
        a->items.emplace_back();
        varNil(&a->items.back());
        bool oob;
        varGetIndexed(p, static_cast<int64_t>(i), &a->items.back(), &oob);
    }
    give(out, std::move(arr));
}

static bool construct(int type, int idx, const Var* const* a, Var* out) {
    switch (type) {
        case T_NIL: varNil(out); return true;
        case T_BOOL:
            if (idx == 0) { give(out, V::ofBool(false)); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            give(out, V::ofBool(num(a[0]) != 0));
            return true;
        case T_INT:
            if (idx == 0) { give(out, V::ofInt(0)); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { double d = ld<double>(a[0]->d); give(out, V::ofInt(std::isfinite(d) ? static_cast<int64_t>(d) : 0)); return true; }
            if (idx == 3) { give(out, V::ofInt(a[0]->d[0])); return true; }
            give(out, V::ofInt(std::strtoll(txt(a[0]).c_str(), nullptr, 0)));
            return true;
        case T_FLOAT:
            if (idx == 0) { give(out, V::ofFloat(0)); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { give(out, V::ofFloat(double(ld<int64_t>(a[0]->d)))); return true; }
            if (idx == 3) { give(out, V::ofFloat(a[0]->d[0])); return true; }
            give(out, V::ofFloat(std::strtod(txt(a[0]).c_str(), nullptr)));
            return true;
        case T_STRING:
            if (idx == 0) { give(out, V::ofString("")); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            give(out, V::ofString(u32ToUtf8(textU(a[0]))));
            return true;
        case T_STRING_NAME:
            if (idx == 0) { give(out, V::ofName("")); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            give(out, V::ofName(u32ToUtf8(textU(a[0]))));
            return true;
        case T_NODE_PATH:
            if (idx == 0) { varNil(out); out->type = T_NODE_PATH; return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            give(out, V::ofString(u32ToUtf8(textU(a[0]))));
            out->type = T_NODE_PATH;
            return true;
        case T_VECTOR2: case T_VECTOR3: case T_VECTOR4: case T_VECTOR2I: case T_VECTOR3I: case T_VECTOR4I: {
            VecKind k{};
            vecKind(type, k);
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { convertVec(out, a[0], type); return true; }
            double c[4];
            for (int i = 0; i < k.comps; ++i) c[i] = num(a[i]);
            giveVec(out, type, k, c);
            return true;
        }
        case T_RECT2: case T_RECT2I: {
            const bool ints = type == T_RECT2I;
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) {
                if (ints) { const float* f = static_cast<const float*>(varPtr(a[0])); putI(out, type, {int64_t(f[0]), int64_t(f[1]), int64_t(f[2]), int64_t(f[3])}); }
                else { const int32_t* f = static_cast<const int32_t*>(varPtr(a[0])); putF(out, type, {double(f[0]), double(f[1]), double(f[2]), double(f[3])}); }
                return true;
            }
            if (idx == 3) { unsigned char buf[16]; std::memcpy(buf, varPtr(a[0]), 8); std::memcpy(buf + 8, varPtr(a[1]), 8); giveNative(out, type, buf); return true; }
            if (ints) putI(out, type, {int64_t(num(a[0])), int64_t(num(a[1])), int64_t(num(a[2])), int64_t(num(a[3]))});
            else putF(out, type, {num(a[0]), num(a[1]), num(a[2]), num(a[3])});
            return true;
        }
        case T_TRANSFORM2D: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2 || idx == 3) {
                const double r = F(a[0]);
                const float* pos = static_cast<const float*>(varPtr(a[idx == 2 ? 1 : 3]));
                double sx = 1, sy = 1, skew = 0;
                if (idx == 3) { const float* s = static_cast<const float*>(varPtr(a[1])); sx = s[0]; sy = s[1]; skew = F(a[2]); }
                putF(out, type, {std::cos(r) * sx, std::sin(r) * sx, -std::sin(r + skew) * sy, std::cos(r + skew) * sy, pos[0], pos[1]});
                return true;
            }
            unsigned char buf[24];
            for (int i = 0; i < 3; ++i) std::memcpy(buf + i * 8, varPtr(a[i]), 8);
            giveNative(out, type, buf);
            return true;
        }
        case T_PLANE: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { Vec3 n = v3(varPtr(a[0])); putF(out, type, {n.x, n.y, n.z, 0}); return true; }
            if (idx == 3) { Vec3 n = v3(varPtr(a[0])); putF(out, type, {n.x, n.y, n.z, F(a[1])}); return true; }
            if (idx == 4) { Vec3 n = v3(varPtr(a[0])), p = v3(varPtr(a[1])); putF(out, type, {n.x, n.y, n.z, dot(n, p)}); return true; }
            if (idx == 5) {
                Vec3 p1 = v3(varPtr(a[0])), p2 = v3(varPtr(a[1])), p3 = v3(varPtr(a[2]));
                Vec3 n = norm3(cross(sub3(p1, p3), sub3(p1, p2)));
                putF(out, type, {n.x, n.y, n.z, dot(n, p1)});
                return true;
            }
            putF(out, type, {F(a[0]), F(a[1]), F(a[2]), F(a[3])});
            return true;
        }
        case T_QUATERNION: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { float q[4]; toQuat(mat3(varPtr(a[0])), q); giveNative(out, type, q); return true; }
            if (idx == 3) {
                Vec3 ax = norm3(v3(varPtr(a[0])));
                double h = F(a[1]) * 0.5, s = std::sin(h);
                putF(out, type, {ax.x * s, ax.y * s, ax.z * s, std::cos(h)});
                return true;
            }
            if (idx == 4) {
                Vec3 v0 = norm3(v3(varPtr(a[0]))), v1 = norm3(v3(varPtr(a[1])));
                Vec3 c = cross(v0, v1);
                double d = dot(v0, v1);
                if (d < -1 + 1e-5) {
                    if (std::fabs(v0.x) > 1e-5 || std::fabs(v0.y) > 1e-5) putF(out, type, {v0.y, -v0.x, 0, 0});
                    else putF(out, type, {0, v0.z, -v0.y, 0});
                    const float* q = static_cast<const float*>(varPtr(out));
                    double l = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2]);
                    putF(out, type, {q[0] / l, q[1] / l, q[2] / l, 0});
                } else {
                    double s = std::sqrt((1 + d) * 2), inv = 1 / s;
                    putF(out, type, {c.x * inv, c.y * inv, c.z * inv, s * 0.5});
                }
                return true;
            }
            putF(out, type, {F(a[0]), F(a[1]), F(a[2]), F(a[3])});
            return true;
        }
        case T_AABB:
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            else { unsigned char buf[24]; std::memcpy(buf, varPtr(a[0]), 12); std::memcpy(buf + 12, varPtr(a[1]), 12); giveNative(out, type, buf); return true; }
        case T_BASIS: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            unsigned char buf[36];
            if (idx == 2) putMat3(buf, fromQuat(static_cast<const float*>(varPtr(a[0]))));
            else if (idx == 3) putMat3(buf, axisAngle(norm3(v3(varPtr(a[0]))), F(a[1])));
            else {
                Vec3 x = v3(varPtr(a[0])), y = v3(varPtr(a[1])), z = v3(varPtr(a[2]));
                putMat3(buf, {{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}});
            }
            giveNative(out, type, buf);
            return true;
        }
        case T_TRANSFORM3D: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            float buf[12];
            if (idx == 2) { std::memcpy(buf, varPtr(a[0]), 36); std::memcpy(buf + 9, varPtr(a[1]), 12); }
            else if (idx == 3) {
                Vec3 x = v3(varPtr(a[0])), y = v3(varPtr(a[1])), z = v3(varPtr(a[2])), o = v3(varPtr(a[3]));
                putMat3(buf, {{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}});
                buf[9] = float(o.x); buf[10] = float(o.y); buf[11] = float(o.z);
            } else {
                const float* p = static_cast<const float*>(varPtr(a[0])); // columns of a projection
                putMat3(buf, {{{p[0], p[4], p[8]}, {p[1], p[5], p[9]}, {p[2], p[6], p[10]}}});
                buf[9] = p[12]; buf[10] = p[13]; buf[11] = p[14];
            }
            giveNative(out, type, buf);
            return true;
        }
        case T_PROJECTION: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            float buf[16] = {};
            if (idx == 2) {
                const float* t = static_cast<const float*>(varPtr(a[0]));
                float cols[16] = {t[0], t[3], t[6], 0, t[1], t[4], t[7], 0, t[2], t[5], t[8], 0, t[9], t[10], t[11], 1};
                std::memcpy(buf, cols, sizeof buf);
            } else for (int i = 0; i < 4; ++i) std::memcpy(buf + i * 4, varPtr(a[i]), 16);
            giveNative(out, type, buf);
            return true;
        }
        case T_COLOR: {
            if (idx == 0) { defaultOf(out, type); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) { float c[4]; std::memcpy(c, varPtr(a[0]), 16); c[3] = float(F(a[1])); giveNative(out, type, c); return true; }
            if (idx == 3) { putF(out, type, {F(a[0]), F(a[1]), F(a[2]), 1}); return true; }
            if (idx == 4) { putF(out, type, {F(a[0]), F(a[1]), F(a[2]), F(a[3])}); return true; }
            float c[4] = {0, 0, 0, 1};
            if (!colorFromText(textU(a[0]), idx == 6 ? F(a[1]) : 1.0, c)) {
                logOnce("color-text", "Color(String): not a hex colour; using transparent black");
                c[0] = c[1] = c[2] = c[3] = 0;
            }
            giveNative(out, type, c);
            return true;
        }
        case T_RID:
            if (idx == 0) { varNil(out); out->type = T_RID; return true; }
            copyAs(out, a[0]);
            return true;
        case T_CALLABLE:
            if (idx == 0) { varNil(out); out->type = T_CALLABLE; return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            else { HostObject* o = varObject(a[0]); if (!o) { varNil(out); out->type = T_CALLABLE; return true; } give(out, makeMethodCallable(o, txt(a[1]))); return true; }
        case T_SIGNAL: {
            varNil(out);
            out->type = T_SIGNAL;
            if (idx == 1) { std::memcpy(out->d, a[0]->d, 16); return true; }
            if (idx == 2) {
                HostObject* o = varObject(a[0]);
                st<uint64_t>(out->d, o ? o->id : 0);
                st<NameData*>(out->d + 8, ld<NameData*>(a[1]->d));
            }
            return true;
        }
        case T_DICTIONARY:
            if (idx == 0) { give(out, makeDict()); return true; }
            copyAs(out, a[0]);
            return true;
        case T_ARRAY: {
            if (idx == 0) { give(out, makeArray()); return true; }
            if (idx == 1) { copyAs(out, a[0]); return true; }
            if (idx == 2) {
                copyAs(out, a[0]);
                ArrayData* d = ld<ArrayData*>(out->d);
                d->elemType = static_cast<int>(ld<int64_t>(a[1]->d));
                d->elemClass = nameText(ld<NameData*>(a[2]->d));
                return true;
            }
            arrayFromPacked(out, a[0]);
            return true;
        }
        default: break;
    }
    if (type >= T_PACKED_BYTE && type <= T_PACKED_VECTOR4) {
        if (idx == 0) { give(out, makePacked(type)); return true; }
        if (idx == 1) { copyAs(out, a[0]); return true; }
        packedFromArray(out, type, ld<ArrayData*>(a[0]->d));
        return true;
    }
    return false;
}

bool varConstructIndex(int type, int index, const Var* const* args, Var* out) { return construct(type, index, args, out); }

bool varConstruct(int type, const Var* const* args, int argc, Var* out, GDExtensionCallError* err) {
    err->error = GDEXTENSION_CALL_OK;
    err->argument = 0;
    err->expected = 0;
    if (type == T_OBJECT || type < 0 || type >= kTypeCount) { err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; varNil(out); return false; }
    // First the constructor whose parameter types match exactly, then the first one the arguments convert to.
    for (int pass = 0; pass < 2; ++pass) {
        for (int idx = 0; idx < 13; ++idx) {
            const char* sig = constructorArgs(type, idx);
            if (!sig) break;
            std::vector<int> want = sigTypes(sig);
            if (static_cast<int>(want.size()) != argc) continue;
            bool ok = true;
            for (int i = 0; i < argc && ok; ++i) {
                if (want[static_cast<size_t>(i)] == kAnyType || want[static_cast<size_t>(i)] == args[i]->type) continue;
                ok = pass == 1 && varCanConvert(args[i]->type, want[static_cast<size_t>(i)], false) &&
                     !(want[static_cast<size_t>(i)] == T_STRING && args[i]->type != T_STRING_NAME && args[i]->type != T_NODE_PATH) ;
            }
            if (!ok) continue;
            std::vector<V> conv(static_cast<size_t>(argc));
            std::vector<const Var*> ptrs(static_cast<size_t>(argc));
            for (int i = 0; i < argc; ++i) {
                const int w = want[static_cast<size_t>(i)];
                if (w == kAnyType || w == args[i]->type) ptrs[static_cast<size_t>(i)] = args[i];
                else if (varConvert(args[i], w, &conv[static_cast<size_t>(i)].v)) ptrs[static_cast<size_t>(i)] = &conv[static_cast<size_t>(i)].v;
                else { ok = false; break; }
            }
            if (!ok) continue;
            return construct(type, idx, ptrs.data(), out);
        }
    }
    err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    varNil(out);
    return false;
}

} // namespace aver::gdext
