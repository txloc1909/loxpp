add_executable(test_main test_main.cpp)

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_test(NAME TestMain COMMAND test_main)
