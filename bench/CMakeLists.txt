add_executable(limitbook_bench bench_book.cpp)
target_link_libraries(limitbook_bench PRIVATE limitbook::limitbook benchmark::benchmark_main)
qc_set_warnings(limitbook_bench)
