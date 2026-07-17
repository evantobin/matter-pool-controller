#pragma once
// Minimal test harness — no external dependencies.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

struct TestCase {
  const char *name;
  void (*fn)();
};

inline std::vector<TestCase> &tests() {
  static std::vector<TestCase> vec;
  return vec;
}

struct TestRegistrar {
  TestRegistrar(const char *name, void (*fn)()) {
    tests().push_back({name, fn});
  }
};

#define TEST_CASE(name) TEST_CASE_EXPAND(name, __COUNTER__)
#define TEST_CASE_EXPAND(name, ctr) TEST_CASE_CTR(name, ctr)
#define TEST_CASE_CTR(name, ctr) \
  static void DOCTEST_FN_##ctr(); \
  static TestRegistrar DOCTEST_REG_##ctr {name, DOCTEST_FN_##ctr}; \
  static void DOCTEST_FN_##ctr()

#define CHECK(expr) do { \
  if (!(expr)) { \
    std::fprintf(stderr, "  FAIL: %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #expr); \
    _failed++; \
  } else { _passed++; } \
} while(0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

extern int _passed;
extern int _failed;

#ifdef DOCTEST_MAIN_DEFINED
int main() {
  _passed = 0;
  _failed = 0;
  for (auto &t : tests()) {
    std::fprintf(stdout, "TEST: %s\n", t.name);
    t.fn();
  }
  std::fprintf(stdout, "\n%d passed, %d failed\n", _passed, _failed);
  return _failed ? 1 : 0;
}
#endif
