add_executable(lob_latency latency.cpp workload.h)
target_link_libraries(lob_latency PRIVATE lob::lob lob_warnings)

# Separate binary: it replaces the global operator new to count allocations.
add_executable(lob_allocs allocs.cpp workload.h)
target_link_libraries(lob_allocs PRIVATE lob::lob lob_warnings)
# allocs.cpp replaces the global operator new/delete with malloc/free. Once GCC inlines the
# replacement delete into container code, -Wmismatched-new-delete reports free() on memory from
# operator new: a false positive here, because this program's operator new is malloc.
target_compile_options(lob_allocs PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
