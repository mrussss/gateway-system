#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class TestFailure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                          \
    do                                                                                            \
    {                                                                                             \
        if (!(condition))                                                                         \
        {                                                                                         \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +           \
                              ": CHECK failed: " #condition);                                    \
        }                                                                                         \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                \
    do                                                                                            \
    {                                                                                             \
        const auto test_actual = (actual);                                                        \
        const auto test_expected = (expected);                                                    \
        if (!(test_actual == test_expected))                                                      \
        {                                                                                         \
            std::ostringstream message;                                                          \
            message << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #actual            \
                    << " != " #expected;                                                         \
            throw TestFailure(message.str());                                                     \
        }                                                                                         \
    } while (false)

using TestCase = std::pair<std::string, std::function<void()>>;

inline int runTests(const std::vector<TestCase> &tests)
{
    size_t failures = 0;
    for (const auto &[name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
