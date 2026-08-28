#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace NHyprTest {

    class CSuite {
      public:
        explicit CSuite(std::string_view name) : m_name(name) {}

        void expect(bool condition, std::string_view description, int line = 0) {
            if (condition)
                return;
            std::cerr << "FAIL: " << description;
            if (line > 0)
                std::cerr << " (line " << line << ')';
            std::cerr << '\n';
            ++m_failures;
        }

        int finish() const {
            if (m_failures != 0)
                return EXIT_FAILURE;
            std::cout << m_name << ": all checks passed\n";
            return EXIT_SUCCESS;
        }

      private:
        std::string_view m_name;
        int              m_failures = 0;
    };

} // namespace NHyprTest

#define HYPR_EXPECT(suite, expression) (suite).expect(static_cast<bool>(expression), #expression, __LINE__)
