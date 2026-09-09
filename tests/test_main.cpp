#include "test_framework.hpp"

namespace anpr_test {
namespace {

std::vector<std::string>& failures() {
    static std::vector<std::string> messages;
    return messages;
}

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

void recordFailure(const std::string& message) {
    failures().push_back(message);
}

int runAllTests() {
    int failed_cases = 0;
    for (const TestCase& test : registry()) {
        const std::size_t before = failures().size();
        test.body();
        const bool passed = failures().size() == before;
        if (!passed) {
            ++failed_cases;
            std::cout << "FAIL " << test.name << '\n';
            for (std::size_t i = before; i < failures().size(); ++i) {
                std::cout << "     " << failures()[i] << '\n';
            }
        }
    }
    std::cout << registry().size() - static_cast<std::size_t>(failed_cases) << '/'
              << registry().size() << " tests passed\n";
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace anpr_test

int main() {
    return anpr_test::runAllTests();
}
