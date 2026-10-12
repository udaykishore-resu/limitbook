add_executable(limitbook-replay replay_itch.cpp)
target_link_libraries(limitbook-replay PRIVATE limitbook::limitbook)
qc_set_warnings(limitbook-replay)
qc_hot_path(limitbook-replay)

add_executable(limitbook-gen-synthetic gen_synthetic.cpp)
target_link_libraries(limitbook-gen-synthetic PRIVATE limitbook::limitbook)
qc_set_warnings(limitbook-gen-synthetic)
