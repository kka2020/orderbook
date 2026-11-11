add_executable(lob_latency latency.cpp workload.h)
target_link_libraries(lob_latency PRIVATE lob::lob lob_warnings)
