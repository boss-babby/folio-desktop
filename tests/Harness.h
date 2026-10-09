// tests/Harness.h
//
// Twenty lines of test harness. See tests/CMakeLists.txt for why there is no
// GoogleTest here. Usage:
//
//     FOLIO_TEST_MAIN("raster cache") {
//         section("C2  evicts the right thing");
//         check(cache.stats().entries == 3, "three survivors");
//     }

#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace folio::test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        std::printf("    FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

inline void section(const std::string& title) {
    std::printf("  %s\n", title.c_str());
}

inline void note(const std::string& text) {
    std::printf("        %s\n", text.c_str());
}

inline bool nearly(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

inline int report(const char* suite) {
    std::printf("\n  %s  (%d check%s, %d failure%s)  [%s]\n\n",
                g_failures == 0 ? "ALL PASS" : "FAILED",
                g_checks, g_checks == 1 ? "" : "s",
                g_failures, g_failures == 1 ? "" : "s", suite);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace folio::test

#define FOLIO_TEST_MAIN(NAME)                                             \
    static void folio_test_body();                                        \
    int main() {                                                          \
        std::printf("\nFolio tests: %s\n", NAME);                         \
        std::printf("-------------------------------------------\n");     \
        folio_test_body();                                                \
        return ::folio::test::report(NAME);                               \
    }                                                                     \
    static void folio_test_body()
