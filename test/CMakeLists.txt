find_package(GTest REQUIRED)

add_executable(test_main test_main.cpp)
add_executable(test_scanner test_scanner.cpp ${PROJECT_SOURCE_DIR}/src/scanner.cpp ${PROJECT_SOURCE_DIR}/src/token.cpp)
add_executable(test_allocator test_allocator.cpp)

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_scanner PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(test_scanner PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_allocator PRIVATE GTest::GTest GTest::Main)

add_test(NAME TestMain COMMAND test_main)
add_test(NAME TestScanner COMMAND test_scanner)
