#include <gtest/gtest.h>

// Proves the toolchain, GoogleTest and CI work. Delete once test_rules.cpp exists.
TEST(Toolchain, Builds) {
    EXPECT_EQ(1 + 1, 2);
}
