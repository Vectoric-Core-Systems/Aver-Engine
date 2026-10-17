// Arabic contextual shaping: joining, presentation forms, lam-alef. See UiShaping.hpp.
#include "aver/ui/UiShaping.hpp"

namespace aver::ui {
namespace {

bool joinsToNext(UiJoin t) { return t == UiJoin::D || t == UiJoin::L || t == UiJoin::C; }
bool joinsToPrev(UiJoin t) { return t == UiJoin::D || t == UiJoin::R || t == UiJoin::C; }

bool isAlefForLigature(u32 cp) { return cp == 0x0622 || cp == 0x0623 || cp == 0x0625 || cp == 0x0627; }

UiShapedCp passThrough(u32 cp, usize src) {
    UiShapedCp u;
    u.cp = cp;
    u.base = cp;
    u.src = static_cast<u32>(src);
    return u;
}

void shapeRange(const u32* cps, usize a, usize b, std::vector<UiShapedCp>& out) {
    const usize m = b - a;
    std::vector<UiJoin> jt(m);
    for (usize k = 0; k < m; ++k) jt[k] = uiJoiningType(cps[a + k]);

    // Nearest non-transparent neighbours, as indices into the range (-1 = none).
    const auto left = [&](usize k) -> long {
        for (usize j = k; j-- > 0;) if (jt[j] != UiJoin::T) return static_cast<long>(j);
        return -1;
    };
    const auto right = [&](usize k) -> long {
        for (usize j = k + 1; j < m; ++j) if (jt[j] != UiJoin::T) return static_cast<long>(j);
        return -1;
    };
    const auto prevJoins = [&](usize k) {
        const long l = left(k);
        return l >= 0 && joinsToNext(jt[static_cast<usize>(l)]);
    };
    const auto nextJoins = [&](usize k) {
        const long r = right(k);
        return r >= 0 && joinsToPrev(jt[static_cast<usize>(r)]);
    };

    for (usize k = 0; k < m;) {
        const u32 cp = cps[a + k];
        const UiJoin t = jt[k];

        if (cp == 0x0644) {   // lam: maybe a lam-alef ligature
            const long r = right(k);
            if (r >= 0 && isAlefForLigature(cps[a + static_cast<usize>(r)])) {
                const bool final = prevJoins(k);
                UiShapedCp lig;
                lig.cp = uiArabicLamAlef(cps[a + static_cast<usize>(r)], final);
                lig.base = cp;
                lig.base2 = cps[a + static_cast<usize>(r)];
                lig.src = static_cast<u32>(a + k);
                lig.srcCount = 2;
                lig.flags = kShapeJoined | kShapeLigature;
                out.push_back(lig);
                for (usize j = k + 1; j < static_cast<usize>(r); ++j) out.push_back(passThrough(cps[a + j], a + j));   // marks between
                k = static_cast<usize>(r) + 1;
                continue;
            }
        }

        if (t == UiJoin::D || t == UiJoin::R || (t == UiJoin::U && uiArabicForm(cp, 0) != 0)) {   // U with a form row: hamza, isolated only
            const bool prev = t != UiJoin::U && prevJoins(k);
            const bool next = t == UiJoin::D && nextJoins(k);
            const int form = t == UiJoin::U ? 0 : t == UiJoin::R ? (prev ? 1 : 0) : (prev ? (next ? 3 : 1) : (next ? 2 : 0));
            UiShapedCp u = passThrough(cp, a + k);
            if (uiArabicForm(cp, 0) != 0) {   // the letter has presentation forms at all
                const u32 shaped = uiArabicForm(cp, form);
                if (shaped != 0) { u.cp = shaped; u.flags = kShapeJoined; }
                else u.flags = kShapeNoForm;
            }
            out.push_back(u);
        } else {
            out.push_back(passThrough(cp, a + k));
        }
        ++k;
    }
}

} // namespace

bool uiIsArabicShapable(u32 cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) ||
           cp == 0x200C || cp == 0x200D;
}

void uiShapeText(const u32* cps, usize n, std::vector<UiShapedCp>& out) {
    out.clear();
    out.reserve(n);
    for (usize i = 0; i < n;) {
        if (!uiIsArabicShapable(cps[i])) {
            out.push_back(passThrough(cps[i], i));
            ++i;
            continue;
        }
        usize b = i;
        while (b < n && uiIsArabicShapable(cps[b])) ++b;
        shapeRange(cps, i, b, out);
        i = b;
    }
}

} // namespace aver::ui
