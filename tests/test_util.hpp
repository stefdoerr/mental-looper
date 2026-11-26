#pragma once
#include <cstdio>
#include <cmath>

// Every test binary runs under AddressSanitizer: cap it, so a runaway test (a wrapped-around sample count asking for
// gigabytes) dies at once instead of filling the machine's memory, which twice made the kernel kill the terminal it
// ran in. Normal peaks are about 400 MB. ASAN_OPTIONS from the environment still applies on top.
extern "C" __attribute__((weak)) const char* __asan_default_options() {
    return "allocator_may_return_null=1:max_allocation_size_mb=2048:hard_rss_limit_mb=2048";
}

inline int& failures() { static int f = 0; return f; }

#define CHECK(cond) do { if (!(cond)) { failures()++; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

inline bool approx(double a, double b, double eps = 1e-4) {
    return std::fabs(a - b) <= eps;
}

#define REPORT() do { \
    if (failures()) { std::printf("FAILED (%d failed checks)\n", failures()); return 1; } \
    std::printf("OK\n"); return 0; } while (0)
