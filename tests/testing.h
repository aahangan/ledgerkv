#pragma once

// A minimal test harness, kept dependency-free so the project builds offline.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace ledgerkv::testing {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

// Returns a fresh, empty directory for one test.
std::string TempDir();

}  // namespace ledgerkv::testing

#define TEST(name)                                                              \
  static void test_##name();                                                    \
  static ::ledgerkv::testing::Registrar registrar_##name(#name, test_##name);      \
  static void test_##name()

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                             \
    }                                                                           \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define CHECK_OK(expr)                                                          \
  do {                                                                          \
    ::ledgerkv::Status _s = (expr);                                                \
    if (!_s.ok()) {                                                             \
      std::fprintf(stderr, "%s:%d: %s returned %s\n", __FILE__, __LINE__, #expr, \
                   _s.ToString().c_str());                                      \
      std::abort();                                                             \
    }                                                                           \
  } while (0)
