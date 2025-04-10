add_executable(alex_example alex_example.cpp)
target_link_libraries(alex_example PUBLIC index Threads::Threads TBB::tbb)