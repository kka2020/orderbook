add_executable(lob_latency latency.cpp workload.h)
target_link_libraries(lob_latency PRIVATE lob::lob lob_warnings)

# Separate binary: it replaces the global operator new to count allocations.
add_executable(lob_allocs allocs.cpp workload.h)
target_link_libraries(lob_allocs PRIVATE lob::lob lob_warnings)
