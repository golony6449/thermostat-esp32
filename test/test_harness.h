#pragma once

// Minimal dependency-free C++17 test harness for host-side unit tests.
//
// Each test translation unit is compiled into its own executable by
// tools/test_host.py, so a single `main()` per test file is enough.
//
// Usage:
//   #include "test_harness.h"
//
//   TEST_CASE(my_case) {
//     EXPECT_EQ(2, 1 + 1);
//   }
//
//   THERMOSTAT_TEST_MAIN()

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace thermostat_test {

struct TestCase {
  const char* name;
  void (*function)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline int& current_failures() {
  static int failures = 0;
  return failures;
}

class Registrar {
 public:
  Registrar(const char* name, void (*function)()) {
    registry().push_back(TestCase{name, function});
  }
};

inline void report_failure(const char* file, int line, const char* message) {
  ++current_failures();
  std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, message);
}

template <typename T>
inline std::string value_to_string(const T& value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

inline std::string value_to_string(const char* value) {
  if (value == nullptr) {
    return "nullptr";
  }
  return std::string("\"") + value + "\"";
}

inline std::string value_to_string(const std::string& value) {
  return "\"" + value + "\"";
}

inline int run_all() {
  int passed = 0;
  int failed = 0;
  for (const TestCase& test_case : registry()) {
    current_failures() = 0;
    test_case.function();
    if (current_failures() == 0) {
      std::printf("PASS %s\n", test_case.name);
      ++passed;
    } else {
      std::printf("FAIL %s (%d check(s) failed)\n", test_case.name,
                  current_failures());
      ++failed;
    }
  }
  std::printf("Passed: %d, Failed: %d\n", passed, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace thermostat_test

#define TEST_CASE(name)                                                   \
  static void thermostat_test_##name();                                   \
  static ::thermostat_test::Registrar thermostat_test_registrar_##name(   \
      #name, &thermostat_test_##name);                                    \
  static void thermostat_test_##name()

#define EXPECT_TRUE(condition)                                          \
  do {                                                                  \
    if (!(condition)) {                                                 \
      ::thermostat_test::report_failure(__FILE__, __LINE__,             \
                                        "EXPECT_TRUE(" #condition ")"); \
    }                                                                   \
  } while (0)

#define EXPECT_FALSE(condition)                                          \
  do {                                                                   \
    if ((condition)) {                                                   \
      ::thermostat_test::report_failure(__FILE__, __LINE__,              \
                                        "EXPECT_FALSE(" #condition ")"); \
    }                                                                    \
  } while (0)

#define EXPECT_EQ(expected, actual)                                          \
  do {                                                                       \
    const auto& thermostat_test_expected = (expected);                       \
    const auto& thermostat_test_actual = (actual);                           \
    if (!(thermostat_test_expected == thermostat_test_actual)) {             \
      std::ostringstream thermostat_test_message;                            \
      thermostat_test_message                                                \
          << "EXPECT_EQ(" #expected ", " #actual ") expected "               \
          << ::thermostat_test::value_to_string(thermostat_test_expected)    \
          << " but got "                                                     \
          << ::thermostat_test::value_to_string(thermostat_test_actual);     \
      ::thermostat_test::report_failure(__FILE__, __LINE__,                  \
                                        thermostat_test_message.str().c_str()); \
    }                                                                        \
  } while (0)

#define EXPECT_STREQ(expected, actual)                                       \
  do {                                                                       \
    const char* thermostat_test_expected_c = (expected);                     \
    const char* thermostat_test_actual_c = (actual);                         \
    if (thermostat_test_expected_c == nullptr ||                             \
        thermostat_test_actual_c == nullptr ||                               \
        std::strcmp(thermostat_test_expected_c, thermostat_test_actual_c) != \
            0) {                                                             \
      std::ostringstream thermostat_test_message;                            \
      thermostat_test_message                                                \
          << "EXPECT_STREQ(" #expected ", " #actual ") expected "            \
          << ::thermostat_test::value_to_string(thermostat_test_expected_c)  \
          << " but got "                                                     \
          << ::thermostat_test::value_to_string(thermostat_test_actual_c);   \
      ::thermostat_test::report_failure(__FILE__, __LINE__,                  \
                                        thermostat_test_message.str().c_str()); \
    }                                                                        \
  } while (0)

#define EXPECT_NEAR(expected, actual, tolerance)                            \
  do {                                                                      \
    const double thermostat_test_expected_d = (expected);                   \
    const double thermostat_test_actual_d = (actual);                       \
    const double thermostat_test_tolerance_d = (tolerance);                 \
    if (!(std::fabs(thermostat_test_expected_d -                            \
                    thermostat_test_actual_d) <=                            \
          thermostat_test_tolerance_d)) {                                   \
      std::ostringstream thermostat_test_message;                           \
      thermostat_test_message                                               \
          << "EXPECT_NEAR(" #expected ", " #actual ") expected "            \
          << thermostat_test_expected_d << " but got "                      \
          << thermostat_test_actual_d << " (tolerance "                     \
          << thermostat_test_tolerance_d << ")";                            \
      ::thermostat_test::report_failure(__FILE__, __LINE__,                 \
                                        thermostat_test_message.str().c_str()); \
    }                                                                       \
  } while (0)

#define THERMOSTAT_TEST_MAIN()          \
  int main() {                          \
    return ::thermostat_test::run_all(); \
  }
