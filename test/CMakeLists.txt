find_package(GTest REQUIRED)
include(GoogleTest)

# Propagate the NaN-tagging selection (inherited from the top-level option) to
# every object compiled in this directory.
if(LOXPP_NAN_TAGGING)
    add_compile_definitions(LOXPP_NAN_TAGGING)
endif()

# Every GTest binary needs the same 29 interpreter sources plus
# test_harness.cpp. Listing them in each executable meant the 41 test targets
# that use them compiled the set ~41 times over; the shared objects below
# compile it twice, once per flag variant.
#
# There are two variants because LOXPP_PROFILE changes the object code and
# cannot be mixed at link time: it adds a member to MemoryManager and VM and
# pulls in profiler.h. Each test executable links exactly one variant, so no
# translation unit ever sees two definitions of the same symbol.
#
# GC stress (collect on every allocation) used to be a third variant
# (LOXPP_STRESS_GC). It is a runtime flag now: the *_gc test targets link the
# ordinary core and get LOXPP_STRESS_GC=1 in their ctest ENVIRONMENT, so the
# _gc executable stresses and its non-_gc twin, built from the same source,
# does not.
set(LOXPP_CORE_SRCS
    ${PROJECT_SOURCE_DIR}/src/vm.cpp
    ${PROJECT_SOURCE_DIR}/src/math.cpp
    ${PROJECT_SOURCE_DIR}/src/compiler.cpp
    ${PROJECT_SOURCE_DIR}/src/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/chunk.cpp
    ${PROJECT_SOURCE_DIR}/src/debug.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/chunk_decoder.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/cfg.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/abstract_stack.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/capture_analysis.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/zero_depth_local.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/native_pops.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/jvm_emitter.cpp
    ${PROJECT_SOURCE_DIR}/src/backend/clr_emitter.cpp
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
    ${PROJECT_SOURCE_DIR}/src/stdlib/os_api.cpp
    ${PROJECT_SOURCE_DIR}/src/stdlib/reflect_api.cpp
)

add_library(loxpp_test_core OBJECT ${LOXPP_CORE_SRCS} test_harness.cpp)
target_include_directories(loxpp_test_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_library(loxpp_test_core_profile OBJECT ${LOXPP_CORE_SRCS} test_harness.cpp)
target_include_directories(loxpp_test_core_profile PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(loxpp_test_core_profile PUBLIC LOXPP_PROFILE)

# loxpp_add_test(<name> <source>... CORE <object-library> [PROBE])
#
# CORE names the interpreter object library the executable links.
# PROBE adds LOXPP_PROJECT_SOURCE_DIR, the repo path a handful of backend
# probe tests read to locate example programs at run time. Only those test
# .cpp files use the macro, so it belongs on the executable, not the library.
function(loxpp_add_test name)
    cmake_parse_arguments(ARG "PROBE" "CORE" "" ${ARGN})
    add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE GTest::GTest GTest::Main ${ARG_CORE})
    if(ARG_PROBE)
        target_compile_definitions(${name} PRIVATE
            LOXPP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    endif()
endfunction()

# test_main is a hand-rolled main() (asserts), not a GTest binary, and needs
# none of the interpreter sources.
add_executable(test_main test_main.cpp)
target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)

# test_scanner needs only the scanner and its tokens, not the whole core.
add_executable(test_scanner test_scanner.cpp
    ${PROJECT_SOURCE_DIR}/src/scanner.cpp ${PROJECT_SOURCE_DIR}/src/token.cpp)
target_include_directories(test_scanner PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_scanner PRIVATE GTest::GTest GTest::Main)

loxpp_add_test(test_parser_vm       test_parser_vm.cpp       CORE loxpp_test_core)
loxpp_add_test(test_parser_bytecode test_parser_bytecode.cpp CORE loxpp_test_core)
loxpp_add_test(test_table           test_table.cpp           CORE loxpp_test_core)
loxpp_add_test(test_allocator       test_allocator.cpp       CORE loxpp_test_core)
loxpp_add_test(test_local_variables test_local_variables.cpp CORE loxpp_test_core)
loxpp_add_test(test_control_flow    test_control_flow.cpp    CORE loxpp_test_core)
loxpp_add_test(test_vm_runtime      test_vm_runtime.cpp      CORE loxpp_test_core)
loxpp_add_test(test_print_nul       test_print_nul.cpp       CORE loxpp_test_core)
loxpp_add_test(test_os_api          test_os_api.cpp          CORE loxpp_test_core)
loxpp_add_test(test_reflect_api     test_reflect_api.cpp     CORE loxpp_test_core)
loxpp_add_test(test_reflect_api_gc  test_reflect_api.cpp     CORE loxpp_test_core)
loxpp_add_test(test_closures        test_closures.cpp        CORE loxpp_test_core)
loxpp_add_test(test_stress_gc       test_stress_gc.cpp       CORE loxpp_test_core)
loxpp_add_test(test_gc_regression   test_gc_regression.cpp   CORE loxpp_test_core)
loxpp_add_test(test_inheritance     test_inheritance.cpp     CORE loxpp_test_core)
loxpp_add_test(test_list            test_list.cpp            CORE loxpp_test_core)
loxpp_add_test(test_list_gc         test_list.cpp            CORE loxpp_test_core)
loxpp_add_test(test_map             test_map.cpp             CORE loxpp_test_core)
loxpp_add_test(test_map_gc          test_map.cpp             CORE loxpp_test_core)
loxpp_add_test(test_var_destructure test_var_destructure.cpp CORE loxpp_test_core)
loxpp_add_test(test_enum            test_enum.cpp            CORE loxpp_test_core)
loxpp_add_test(test_enum_gc         test_enum.cpp            CORE loxpp_test_core)
loxpp_add_test(test_class_pattern    test_class_pattern.cpp   CORE loxpp_test_core)
loxpp_add_test(test_class_pattern_gc test_class_pattern.cpp   CORE loxpp_test_core)
loxpp_add_test(test_match_expr      test_match_expr.cpp      CORE loxpp_test_core)
loxpp_add_test(test_slice          test_slice.cpp           CORE loxpp_test_core)
loxpp_add_test(test_or_pattern      test_or_pattern.cpp      CORE loxpp_test_core)
loxpp_add_test(test_or_pattern_gc   test_or_pattern.cpp      CORE loxpp_test_core)
loxpp_add_test(test_seq_destructure test_seq_destructure.cpp CORE loxpp_test_core)
loxpp_add_test(test_list_pattern    test_list_pattern.cpp    CORE loxpp_test_core)
loxpp_add_test(test_list_pattern_gc test_list_pattern.cpp    CORE loxpp_test_core)
loxpp_add_test(test_at_binding      test_at_binding.cpp      CORE loxpp_test_core)
loxpp_add_test(test_at_binding_gc   test_at_binding.cpp      CORE loxpp_test_core)
loxpp_add_test(test_profiler        profiler_test.cpp        CORE loxpp_test_core_profile)
loxpp_add_test(test_chunk_decoder   test_chunk_decoder.cpp   CORE loxpp_test_core PROBE)
loxpp_add_test(test_backend_cfg     test_backend_cfg.cpp     CORE loxpp_test_core PROBE)
loxpp_add_test(test_backend_abstract_stack test_backend_abstract_stack.cpp
                                                            CORE loxpp_test_core PROBE)
loxpp_add_test(test_backend_capture test_backend_capture.cpp CORE loxpp_test_core PROBE)
loxpp_add_test(test_jvm_emit        test_jvm_emit.cpp        CORE loxpp_test_core PROBE)
loxpp_add_test(test_clr_emit        test_clr_emit.cpp        CORE loxpp_test_core PROBE)
loxpp_add_test(test_stringify_depth_guard test_stringify_depth_guard.cpp
                                                            CORE loxpp_test_core)

add_test(NAME TestMain COMMAND test_main)

# The *_gc targets share their source with a non-_gc twin. They run under GC
# stress through the ctest ENVIRONMENT below, which MemoryManager reads once
# per VM; the twin, run with no such variable, keeps the normal GC schedule.
set(LOXPP_GC_STRESS_TARGETS
    test_reflect_api_gc test_stress_gc test_gc_regression test_list_gc
    test_map_gc test_enum_gc test_class_pattern_gc test_or_pattern_gc
    test_list_pattern_gc test_at_binding_gc)

# Register each GTest case as its own CTest test so `ctest -j` parallelizes
# within a binary, not just across binaries. The per-target prefix keeps names
# unique: the *_gc targets reuse the same test sources, so their Suite.Case
# names would otherwise collide.
set(LOXPP_GTEST_TARGETS
    test_scanner test_parser_vm test_parser_bytecode test_table test_allocator
    test_local_variables test_control_flow test_vm_runtime test_print_nul
    test_os_api test_reflect_api test_reflect_api_gc test_closures
    test_stress_gc test_gc_regression test_inheritance test_list test_list_gc
    test_map test_map_gc test_var_destructure test_enum test_enum_gc
    test_class_pattern test_class_pattern_gc test_match_expr test_slice
    test_or_pattern test_or_pattern_gc test_seq_destructure test_list_pattern
    test_list_pattern_gc test_at_binding test_at_binding_gc test_profiler
    test_chunk_decoder test_backend_cfg test_backend_abstract_stack
    test_backend_capture test_jvm_emit test_clr_emit test_stringify_depth_guard)
foreach(t IN LISTS LOXPP_GTEST_TARGETS)
    if(t IN_LIST LOXPP_GC_STRESS_TARGETS)
        gtest_discover_tests(${t} TEST_PREFIX "${t}."
            PROPERTIES ENVIRONMENT "LOXPP_STRESS_GC=1")
    else()
        gtest_discover_tests(${t} TEST_PREFIX "${t}.")
    endif()
endforeach()
