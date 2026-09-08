#pragma once
// CHECK is independent of NDEBUG — tests must fail in Release builds.
#include <cstdio>
#include <cstdlib>

namespace hft::test {

inline int& failures() {
    static int f = 0;
    return f;
}
inline int& passes() {
    static int p = 0;
    return p;
}

} // namespace hft::test

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         (msg));                                               \
            ++hft::test::failures();                                           \
        } else {                                                               \
            ++hft::test::passes();                                             \
        }                                                                      \
    } while (0)

#define TEST_EXIT()                                                            \
    do {                                                                       \
        if (hft::test::failures() != 0) {                                      \
            std::fprintf(stderr, "FAILED: %d check(s) failed, %d passed\n",    \
                         hft::test::failures(), hft::test::passes());          \
            return 1;                                                          \
        }                                                                      \
        std::printf("OK: %d checks passed\n", hft::test::passes());            \
        return 0;                                                              \
    } while (0)
