#pragma once

// Minimal assertion for the host tests: stops at the first failure and names
// the source line. Tests are plain programs so that they build with nothing
// but a C++17 compiler.

#include <cstdio>
#include <cstdlib>

namespace test_support {

inline void check(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    static_cast<void>(std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, message));
    // _Exit skips static destructors, so a failing check is safe while a
    // second test thread still runs.
    std::_Exit(1);
  }
}

}  // namespace test_support

#define CHECK(condition, message) \
  ::test_support::check(static_cast<bool>(condition), (message), __FILE__, __LINE__)
