find_package(GTest REQUIRED)

set(ALLOC_SRCS
    ${PROJECT_SOURCE_DIR}/src/simple_allocator.cpp
    ${PROJECT_SOURCE_DIR}/src/object.cpp
    ${PROJECT_SOURCE_DIR}/src/table.cpp
    ${PROJECT_SOURCE_DIR}/src/value.cpp
)

set(FULL_SRCS
    ${PROJECT_SOURCE_DIR}/src/vm.cpp
    ${PROJECT_SOURCE_DIR}/src/compiler.cpp
    ${PROJECT_SOURCE_DIR}/src/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/chunk.cpp
    ${PROJECT_SOURCE_DIR}/src/debug.cpp
    ${PROJECT_SOURCE_DIR}/src/value.cpp
    ${PROJECT_SOURCE_DIR}/src/object.cpp
    ${PROJECT_SOURCE_DIR}/src/table.cpp
    ${PROJECT_SOURCE_DIR}/src/simple_allocator.cpp
    ${PROJECT_SOURCE_DIR}/src/token.cpp
    ${PROJECT_SOURCE_DIR}/src/scanner.cpp
)

add_executable(test_main test_main.cpp)
add_executable(test_scanner test_scanner.cpp ${PROJECT_SOURCE_DIR}/src/scanner.cpp ${PROJECT_SOURCE_DIR}/src/token.cpp)
add_executable(test_parser_vm test_parser_vm.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_parser_bytecode test_parser_bytecode.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_table test_table.cpp ${PROJECT_SOURCE_DIR}/src/table.cpp ${PROJECT_SOURCE_DIR}/src/object.cpp ${PROJECT_SOURCE_DIR}/src/value.cpp ${PROJECT_SOURCE_DIR}/src/simple_allocator.cpp)
add_executable(test_allocator test_allocator.cpp ${ALLOC_SRCS})

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_scanner PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_parser_vm PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_parser_bytecode PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_table PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_allocator PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(test_scanner PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_parser_vm PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_parser_bytecode PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_table PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_allocator PRIVATE GTest::GTest GTest::Main)

add_test(NAME TestMain COMMAND test_main)
add_test(NAME TestScanner COMMAND test_scanner)
add_test(NAME TestParserVM COMMAND test_parser_vm)
add_test(NAME TestParserBytecode COMMAND test_parser_bytecode)
add_test(NAME TestTable COMMAND test_table)
add_test(NAME TestAllocator COMMAND test_allocator)
