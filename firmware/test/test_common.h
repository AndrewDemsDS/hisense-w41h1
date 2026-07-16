// test_common.h -- shared pass/fail counters + CHECK macro for the host tests
// (test_codec.cpp, test_matter_map.cpp). Each test is its own translation unit /
// binary, so the internal-linkage counters below get a fresh copy per test.
#pragma once
#include <cstdio>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
