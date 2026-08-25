#pragma once
// Minimal header-only test framework (no external deps).
//
// Every test translation unit includes this and defines a `suite()` that
// registers its test functions. A single `main` in test_main.cpp drives them.
//
// The global result counter is a function-local static in g_result() so it is
// shared across all TUs without a separate definition.
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace a2adtest {

struct Result {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

// Single shared result object for the whole test binary.
inline Result& g_result() {
    static Result r;
    return r;
}

struct TestFn {
    std::string name;
    std::function<void()> fn;
};

// Each test TU defines a `register_tests()` that pushes into this registry.
inline std::vector<TestFn>& registry() {
    static std::vector<TestFn> r;
    return r;
}

// Each test TU has a `static void my_suite()` that calls ADD_TEST(...). The
// TU also has a static initializer that calls register_one.
inline void register_one(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
}

// Runs all registered tests, printing a per-test line and the final tally.
// Returns 0 on success, 1 on any failure.
inline int run_all() {
    int exit_code = 0;
    for (auto& t : registry()) {
        std::fprintf(stderr, "  %s\n", t.name.c_str());
        t.fn();
    }
    std::fprintf(stderr, "\n%d passed, %d failed\n", g_result().passed, g_result().failed);
    for (auto& f : g_result().failures) std::fprintf(stderr, "  FAIL: %s\n", f.c_str());
    return g_result().failed == 0 ? 0 : 1;
}

}  // namespace a2adtest

// Macros used inside test TUs.
#define CHECK(cond) do { \
    if (cond) { ++a2adtest::g_result().passed; } \
    else { \
        ++a2adtest::g_result().failed; \
        std::ostringstream os; \
        os << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond; \
        a2adtest::g_result().failures.push_back(os.str()); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (_va == _vb) { ++a2adtest::g_result().passed; } \
    else { \
        ++a2adtest::g_result().failed; \
        std::ostringstream os; \
        os << __FILE__ << ":" << __LINE__ << " CHECK_EQ failed: " #a " != " #b; \
        a2adtest::g_result().failures.push_back(os.str()); \
    } \
} while (0)

// Define a test function and auto-register it.
#define ADD_TEST(fn) \
    static void fn(); \
    static const int _reg_##fn = (a2adtest::register_one(#fn, fn), 0); \
    static void fn()
