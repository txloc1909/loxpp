find_package(GTest REQUIRED)
include(GoogleTest)

# Propagate the NaN-tagging selection (inherited from the top-level option) to
# every test target, which compile their own copies of src/*.cpp.
if(LOXPP_NAN_TAGGING)
    add_compile_definitions(LOXPP_NAN_TAGGING)
endif()

set(ALLOC_SRCS
    ${PROJECT_SOURCE_DIR}/src/memory_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/object.cpp
    ${PROJECT_SOURCE_DIR}/src/container_objects.cpp
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
    ${PROJECT_SOURCE_DIR}/src/container_objects.cpp
    ${PROJECT_SOURCE_DIR}/src/table.cpp
    ${PROJECT_SOURCE_DIR}/src/memory_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/token.cpp
    ${PROJECT_SOURCE_DIR}/src/scanner.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/stdlib_context.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/stdlib_registrar.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/globals.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/file_api.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/map_api.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/math_module.cpp
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
add_executable(test_enum test_enum.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_enum_gc test_enum.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_class_pattern test_class_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_class_pattern_gc test_class_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_match_expr test_match_expr.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_slice test_slice.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_or_pattern test_or_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_or_pattern_gc test_or_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_seq_destructure test_seq_destructure.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_list_pattern test_list_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_list_pattern_gc test_list_pattern.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_at_binding test_at_binding.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_at_binding_gc test_at_binding.cpp test_harness.cpp ${FULL_SRCS})

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
target_include_directories(test_enum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_enum_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_class_pattern PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_class_pattern_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_match_expr PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_slice PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_or_pattern PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_or_pattern_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_seq_destructure PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_list_pattern PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_list_pattern_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_at_binding PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_at_binding_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(test_stress_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_gc_regression PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_list_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_map_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_enum_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_class_pattern_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_or_pattern_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_list_pattern_gc PRIVATE LOXPP_STRESS_GC)
target_compile_definitions(test_at_binding_gc PRIVATE LOXPP_STRESS_GC)

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
target_link_libraries(test_enum PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_enum_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_class_pattern PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_class_pattern_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_match_expr PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_slice PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_or_pattern PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_or_pattern_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_seq_destructure PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_list_pattern PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_list_pattern_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_at_binding PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_at_binding_gc PRIVATE GTest::GTest GTest::Main)

# test_main is a hand-rolled main() (asserts), not a GTest binary.
add_test(NAME TestMain COMMAND test_main)

# Register each GTest case as its own CTest test so `ctest -j` parallelizes
# within a binary, not just across binaries. The per-target prefix keeps names
# unique: the *_gc targets recompile the same sources, so their Suite.Case
# names would otherwise collide.
set(LOXPP_GTEST_TARGETS
    test_scanner test_parser_vm test_parser_bytecode test_table test_allocator
    test_local_variables test_control_flow test_vm_runtime test_closures
    test_stress_gc test_gc_regression test_inheritance test_list test_list_gc
    test_map test_map_gc test_var_destructure test_enum test_enum_gc
    test_class_pattern test_class_pattern_gc test_match_expr test_slice
    test_or_pattern test_or_pattern_gc test_seq_destructure test_list_pattern
    test_list_pattern_gc test_at_binding test_at_binding_gc)
foreach(t IN LISTS LOXPP_GTEST_TARGETS)
    gtest_discover_tests(${t} TEST_PREFIX "${t}.")
endforeach()
