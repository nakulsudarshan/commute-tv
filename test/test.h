// Minimal zero-dependency test harness. Deliberately not a framework: the
// whole point of this layer is that it builds anywhere with just g++.
#pragma once
#include <cstdio>
#include <cstring>
#include <cmath>

namespace t {
inline int g_pass = 0;
inline int g_fail = 0;
inline const char* g_suite = "";

inline void suite(const char* n) { g_suite = n; std::printf("\n[%s]\n", n); }

inline void ok(bool cond, const char* what, const char* file, int line) {
    if (cond) { ++g_pass; }
    else { ++g_fail; std::printf("  FAIL %s\n       at %s:%d\n", what, file, line); }
}

inline void eqi(long long a, long long b, const char* what, const char* file, int line) {
    if (a == b) { ++g_pass; }
    else {
        ++g_fail;
        std::printf("  FAIL %s\n       expected %lld, got %lld\n       at %s:%d\n",
                    what, b, a, file, line);
    }
}

inline void eqf(double a, double b, double tol, const char* what, const char* file, int line) {
    if (std::fabs(a - b) <= tol) { ++g_pass; }
    else {
        ++g_fail;
        std::printf("  FAIL %s\n       expected %.4f, got %.4f\n       at %s:%d\n",
                    what, b, a, file, line);
    }
}

inline void eqs(const char* a, const char* b, const char* what, const char* file, int line) {
    if (a && b && std::strcmp(a, b) == 0) { ++g_pass; }
    else {
        ++g_fail;
        std::printf("  FAIL %s\n       expected \"%s\"\n       got      \"%s\"\n       at %s:%d\n",
                    what, b ? b : "(null)", a ? a : "(null)", file, line);
    }
}

inline int report() {
    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
} // namespace t

#define CHECK(c)          t::ok((c), #c, __FILE__, __LINE__)
#define CHECK_EQ(a,b)     t::eqi((long long)(a), (long long)(b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a,b,e) t::eqf((double)(a), (double)(b), (e), #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_STR(a,b)    t::eqs((a), (b), #a " == " #b, __FILE__, __LINE__)
