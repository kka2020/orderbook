include(GoogleTest)

# Add test sources here as they are written (test_rules.cpp, test_differential.cpp, ...).
add_executable(lob_tests
        test_rules.cpp
        book_test_support.h)
target_link_libraries(lob_tests PRIVATE lob::lob lob_warnings GTest::gtest_main)

gtest_discover_tests(lob_tests)
