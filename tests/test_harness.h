#pragma once

// A dependency-free assertion harness. The project deliberately has no external
// dependencies, and a test framework would be its first.

#include <cstdio>
#include <string>
#include <vector>

namespace harness {

inline int& failures() { static int value = 0; return value; }
inline int& checks() { static int value = 0; return value; }
inline std::string& group() { static std::string value; return value; }

inline void begin(const char* name) {
    group() = name;
    std::printf("\n== %s ==\n", name);
}

inline void record(bool passed, const char* what, const std::string& detail) {
    ++checks();
    if (passed) {
        std::printf("  ok   %s\n", what);
        return;
    }
    ++failures();
    std::printf("  FAIL %s\n", what);
    if (!detail.empty()) {
        std::printf("       %s\n", detail.c_str());
    }
}

inline int summary() {
    std::printf("\n%d check(s), %d failure(s)\n", checks(), failures());
    return failures() == 0 ? 0 : 1;
}

template <typename A, typename B>
inline void equal(const char* what, const A& got, const B& want) {
    const bool passed = (got == want);
    std::string detail;
    if (!passed) {
        detail = "got " + std::to_string(got) + ", want " + std::to_string(want);
    }
    record(passed, what, detail);
}

inline void equal_text(const char* what, const std::string& got, const std::string& want) {
    record(got == want, what, got == want ? "" : "got \"" + got + "\", want \"" + want + "\"");
}

inline void is_true(const char* what, bool value) { record(value, what, ""); }
inline void is_false(const char* what, bool value) { record(!value, what, ""); }

}  // namespace harness

#define CHECK(what, value) harness::is_true(what, (value))
#define CHECK_FALSE(what, value) harness::is_false(what, (value))
#define CHECK_EQ(what, got, want) harness::equal(what, (got), (want))
#define CHECK_TEXT(what, got, want) harness::equal_text(what, (got), (want))
