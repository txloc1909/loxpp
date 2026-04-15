cmake_minimum_required(VERSION 3.23)
project(Lox++ VERSION 1.0 LANGUAGES CXX)

# Install git hooks on every configure so agents get them automatically.
execute_process(
    COMMAND git config core.hooksPath .githooks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE _git_config_result
)
if(NOT _git_config_result EQUAL 0)
    message(WARNING "Failed to set core.hooksPath — git hooks will not run automatically.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# parser rule table uses C99 designator
add_compile_options(-Wno-c99-designator)

include_directories(${PROJECT_SOURCE_DIR}/src)

add_executable(loxpp
    src/main.cpp
    src/chunk.cpp
    src/debug.cpp
    src/value.cpp
    src/object.cpp
    src/table.cpp
    src/memory_manager.cpp
    src/math.cpp
    src/vm.cpp
    src/scanner.cpp
    src/token.cpp
    src/compiler.cpp
    src/parser.cpp
)

# Debug output flags: default ON for Debug builds, OFF otherwise.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
if(_build_type_upper STREQUAL "DEBUG")
    set(_debug_default ON)
else()
    set(_debug_default OFF)
endif()

option(LOXPP_DEBUG_PRINT_CODE
    "Disassemble each compiled chunk after compilation" ${_debug_default})
option(LOXPP_DEBUG_TRACE_EXECUTION
    "Trace VM execution: print stack + disassemble each instruction" ${_debug_default})

if(LOXPP_DEBUG_PRINT_CODE)
    target_compile_definitions(loxpp PRIVATE LOXPP_DEBUG_PRINT_CODE)
endif()
if(LOXPP_DEBUG_TRACE_EXECUTION)
    target_compile_definitions(loxpp PRIVATE LOXPP_DEBUG_TRACE_EXECUTION)
endif()
option(LOXPP_DEBUG_LOG_GC
    "Log GC mark, trace, and sweep events to stderr" ${_debug_default})
if(LOXPP_DEBUG_LOG_GC)
    target_compile_definitions(loxpp PRIVATE LOXPP_DEBUG_LOG_GC)
endif()
option(LOXPP_STRESS_GC
    "Force garbage collection on every allocation (sets m_nextGC=0)" ${_debug_default})
if(LOXPP_STRESS_GC)
    target_compile_definitions(loxpp PRIVATE LOXPP_STRESS_GC)
endif()

# Readline support for the REPL.
find_library(READLINE_LIB readline)
find_path(READLINE_INCLUDE_DIR readline/readline.h)
if(READLINE_LIB AND READLINE_INCLUDE_DIR)
    set(_readline_found_default ON)
else()
    set(_readline_found_default OFF)
endif()
option(LOXPP_USE_READLINE "Enable GNU Readline for the REPL" ${_readline_found_default})
if(LOXPP_USE_READLINE)
    if(NOT READLINE_LIB OR NOT READLINE_INCLUDE_DIR)
        message(FATAL_ERROR "LOXPP_USE_READLINE=ON but readline library or headers not found.")
    endif()
    target_include_directories(loxpp PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(loxpp PRIVATE ${READLINE_LIB})
    target_compile_definitions(loxpp PRIVATE LOXPP_USE_READLINE)
    message(STATUS "Readline support: ON (${READLINE_LIB})")
else()
    message(STATUS "Readline support: OFF")
endif()

enable_testing()
add_subdirectory(test)
