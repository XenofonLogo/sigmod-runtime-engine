#pragma once
#include <iostream>
#include <string>

inline int tests_failed = 0;

#define TEST(name) \
    std::cout << "[TEST] " << name << "\n";

#define CHECK(expr) \
    if (!(expr)) { \
        std::cout << "  FAILED: " << #expr << " at line " << __LINE__ << "\n"; \
        tests_failed++; \
    } else { \
        std::cout << "  OK: " << #expr << "\n"; \
    }

#define END_TESTS() \
    if (tests_failed == 0) { \
        std::cout << "\nAll tests PASSED.\n"; \
    } else { \
        std::cout << "\n" << tests_failed << " tests FAILED.\n"; \
    }
