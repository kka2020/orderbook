include(GoogleTest)

# Add test sources here as they are written.
add_executable(lob_tests
        test_rules.cpp
        test_concepts.cpp
        test_differential.cpp
        book_test_support.h)
target_link_libraries(lob_tests PRIVATE lob::lob lob_warnings GTest::gtest_main)
# The differential test reuses the benchmark's workload generator (bench/workload.h).
target_include_directories(lob_tests PRIVATE ${PROJECT_SOURCE_DIR}/bench)

gtest_discover_tests(lob_tests)
