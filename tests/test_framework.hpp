#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

/// Dependency-free test registry. Each TEST macro registers a case; `runAllTests` runs them and
/// reports failures with file and line. Keeping this in-tree avoids pulling a test framework onto
/// the edge device's build.
namespace anpr_test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();
int runAllTests();
void recordFailure(const std::string& message);

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else {
        std::ostringstream out;
        out << value;
        return out.str();
    }
}

}  // namespace anpr_test

#define ANPR_TEST_CONCAT_INNER(a, b) a##b
#define ANPR_TEST_CONCAT(a, b) ANPR_TEST_CONCAT_INNER(a, b)

#define TEST(name)                                                                    \
    static void ANPR_TEST_CONCAT(anpr_test_body_, __LINE__)();                        \
    static const anpr_test::Registrar ANPR_TEST_CONCAT(anpr_test_registrar_, __LINE__)( \
        name, &ANPR_TEST_CONCAT(anpr_test_body_, __LINE__));                          \
    static void ANPR_TEST_CONCAT(anpr_test_body_, __LINE__)()

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            anpr_test::recordFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": CHECK(" #condition ") failed");                   \
            return;                                                                       \
        }                                                                                 \
    } while (false)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        const auto& anpr_actual = (actual);                                                \
        const auto& anpr_expected = (expected);                                            \
        if (!(anpr_actual == anpr_expected)) {                                             \
            anpr_test::recordFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": expected " #actual " == " #expected " but got '" + \
                                     anpr_test::describe(anpr_actual) + "' vs '" +          \
                                     anpr_test::describe(anpr_expected) + "'");             \
            return;                                                                        \
        }                                                                                  \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                            \
    do {                                                                                   \
        const double anpr_actual = static_cast<double>(actual);                            \
        const double anpr_expected = static_cast<double>(expected);                        \
        if (std::fabs(anpr_actual - anpr_expected) > (tolerance)) {                        \
            anpr_test::recordFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": expected " #actual " near " #expected " but got " + \
                                     anpr_test::describe(anpr_actual));                     \
            return;                                                                        \
        }                                                                                  \
    } while (false)
