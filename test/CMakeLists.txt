find_package(GTest REQUIRED)

set(ALLOC_SRCS
    ${PROJECT_SOURCE_DIR}/src/memory_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/object.cpp
    ${PROJECT_SOURCE_DIR}/src/table.cpp
    ${PROJECT_SOURCE_DIR}/src/value.cpp
)

set(FULL_SRCS
    ${PROJECT_SOURCE_DIR}/src/vm.cpp
    ${PROJECT_SOURCE_DIR}/src/math.cpp
    ${PROJECT_SOURCE_DIR}/src/compiler.cpp
    ${PROJECT_SOURCE_DIR}/src/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/chunk.cpp
    ${PROJECT_SOURCE_DIR}/src/debug.cpp
    ${PROJECT_SOURCE_DIR}/src/value.cpp
    ${PROJECT_SOURCE_DIR}/src/object.cpp
    ${PROJECT_SOURCE_DIR}/src/table.cpp
    ${PROJECT_SOURCE_DIR}/src/memory_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/token.cpp
    ${PROJECT_SOURCE_DIR}/src/scanner.cpp
)

add_executable(test_main test_main.cpp)
add_executable(test_scanner test_scanner.cpp ${PROJECT_SOURCE_DIR}/src/scanner.cpp ${PROJECT_SOURCE_DIR}/src/token.cpp)
add_executable(test_parser_vm test_parser_vm.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_parser_bytecode test_parser_bytecode.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_table test_table.cpp ${FULL_SRCS})
add_executable(test_allocator test_allocator.cpp ${FULL_SRCS})
add_executable(test_local_variables test_local_variables.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_control_flow test_control_flow.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_vm_runtime test_vm_runtime.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_closures test_closures.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_stress_gc test_stress_gc.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_gc_regression test_gc_regression.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_inheritance test_inheritance.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_list test_list.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_list_gc test_list.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_map test_map.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_map_gc test_map.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_var_destructure test_var_destructure.cpp test_harness.cpp ${FULL_SRCS})

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_scanner PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_parser_vm PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_parser_bytecode PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_table PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_allocator PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_local_variables PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_control_flow PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_vm_runtime PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_closures PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_stress_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_gc_regression PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_inheritance PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_list PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_list_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_map PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_map_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_include_directories(test_var_destructure PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(test_stress_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_gc_regression PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_list_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_map_gc PRIVATE LOXPP_STRESS_GC)

target_link_libraries(test_scanner PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_parser_vm PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_parser_bytecode PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_table PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_allocator PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_local_variables PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_control_flow PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_vm_runtime PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_closures PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_stress_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_gc_regression PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_inheritance PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_list PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_list_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_map PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_map_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_var_destructure PRIVATE GTest::GTest GTest::Main)

add_test(NAME TestMain COMMAND test_main)
add_test(NAME TestScanner COMMAND test_scanner)
add_test(NAME TestParserVM COMMAND test_parser_vm)
add_test(NAME TestParserBytecode COMMAND test_parser_bytecode)
add_test(NAME TestTable COMMAND test_table)
add_test(NAME TestAllocator COMMAND test_allocator)
add_test(NAME TestLocalVariables COMMAND test_local_variables)
add_test(NAME TestControlFlow COMMAND test_control_flow)
add_test(NAME TestVMRuntime COMMAND test_vm_runtime)
add_test(NAME TestClosures COMMAND test_closures)
add_test(NAME TestStressGC COMMAND test_stress_gc)
add_test(NAME TestGcRegression COMMAND test_gc_regression)
add_test(NAME TestInheritance COMMAND test_inheritance)
add_test(NAME TestList COMMAND test_list)
add_test(NAME TestListGC COMMAND test_list_gc)
add_test(NAME TestMap COMMAND test_map)
add_test(NAME TestMapGC COMMAND test_map_gc)
add_test(NAME TestVarDestructure COMMAND test_var_destructure)
