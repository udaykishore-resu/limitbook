include(GoogleTest)

add_executable(limitbook_tests
  test_object_pool.cpp
  test_intrusive_list.cpp
  test_order_index.cpp
  test_order_book.cpp
  test_matching_engine.cpp
  test_itch_codec.cpp
  test_book_builder.cpp
  test_latency_histogram.cpp)
target_link_libraries(limitbook_tests PRIVATE limitbook::limitbook GTest::gtest_main)
qc_set_warnings(limitbook_tests)
gtest_discover_tests(limitbook_tests DISCOVERY_TIMEOUT 60)

# Replaces global operator new/delete to count allocations, so it must be its
# own binary. Proves the steady-state hot path performs zero heap allocations.
add_executable(limitbook_zero_alloc_test test_zero_alloc.cpp)
target_link_libraries(limitbook_zero_alloc_test PRIVATE limitbook::limitbook GTest::gtest_main)
qc_set_warnings(limitbook_zero_alloc_test)
# Sanitizers interpose the allocator themselves; counting is meaningless there.
if(NOT QC_SANITIZER)
  gtest_discover_tests(limitbook_zero_alloc_test DISCOVERY_TIMEOUT 60)
endif()
