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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Route compilation through ccache when it is available. The Docker dev
# images ship it; a bare local build without it still configures fine.
# A compiler launcher composes with the explicit CMAKE_CXX_COMPILER the
# presets set, which a bare CC/CXX override would not.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
else()
    message(STATUS "ccache: not found - compiling without it")
endif()

# parser rule table uses C99 designator
add_compile_options(-Wno-c99-designator)

include_directories(${PROJECT_SOURCE_DIR}/src)

# The interpreter front-end and runtime, shared by the loxpp executable and,
# from the editor-tooling mission onward, the loxpp-lsp binary and the
# src/tooling analysers. main.cpp is the only source that stays on the
# executable. jvm_emitter.cpp / clr_emitter.cpp are added below, gated on
# their options.
set(LOXPP_CORE_SOURCES
    src/chunk.cpp
    src/debug.cpp
    src/backend/chunk_decoder.cpp
    src/backend/cfg.cpp
    src/backend/abstract_stack.cpp
    src/backend/capture_analysis.cpp
    src/backend/zero_depth_local.cpp
    src/backend/native_pops.cpp
    src/value.cpp
    src/object.cpp
    src/container_objects.cpp
    src/table.cpp
    src/memory_manager.cpp
    src/math.cpp
    src/vm.cpp
    src/scanner.cpp
    src/token.cpp
    src/compiler.cpp
    src/parser.cpp
    src/stdlib/stdlib_context.cpp
    src/stdlib/stdlib_registrar.cpp
    src/stdlib/globals.cpp
    src/stdlib/file_api.cpp
    src/stdlib/map_api.cpp
    src/stdlib/math_module.cpp
    src/stdlib/os_api.cpp
    src/stdlib/reflect_api.cpp
)

add_library(loxpp_core OBJECT ${LOXPP_CORE_SOURCES})
target_include_directories(loxpp_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

# PUBLIC definitions of loxpp_core: they select a type layout in a header that
# main.cpp reaches, so every translation unit linked with the library must
# agree. Collected here and applied once below. The profiling loxpp does not
# link loxpp_core (see the executable section), so it reuses this same list to
# stay layout-compatible.
set(LOXPP_CORE_PUBLIC_DEFINITIONS "")

# The JVM code generator (backend node N4 onward). Default ON so CI builds
# and checks it; OFF drops --target jvm from the CLI for a build that wants
# the native VM only.
option(LOXPP_JVM_BACKEND
    "Build the JVM backend code generator (--target jvm)" ON)
if(LOXPP_JVM_BACKEND)
    target_sources(loxpp_core PRIVATE src/backend/jvm_emitter.cpp)
    # PUBLIC: main.cpp reads LOXPP_JVM_BACKEND for the --target dispatch and
    # the LOXPP_TARGET_USAGE_LIST message.
    list(APPEND LOXPP_CORE_PUBLIC_DEFINITIONS LOXPP_JVM_BACKEND)
    list(APPEND LOXPP_CORE_SOURCES src/backend/jvm_emitter.cpp)
endif()

# The CLR code generator. Default ON so CI builds and checks it; OFF drops
# --target clr from the CLI for a build that wants the native VM (and,
# independently, the JVM backend) only.
option(LOXPP_CLR_BACKEND
    "Build the CLR backend code generator (--target clr)" ON)
if(LOXPP_CLR_BACKEND)
    target_sources(loxpp_core PRIVATE src/backend/clr_emitter.cpp)
    # PUBLIC: main.cpp reads LOXPP_CLR_BACKEND for the --target dispatch and
    # the LOXPP_TARGET_USAGE_LIST message.
    list(APPEND LOXPP_CORE_PUBLIC_DEFINITIONS LOXPP_CLR_BACKEND)
    list(APPEND LOXPP_CORE_SOURCES src/backend/clr_emitter.cpp)
endif()

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

# The debug flags appear only in .cpp translation units (compiler.cpp,
# vm.cpp, memory_manager.cpp), never in a header main.cpp includes, so they
# are PRIVATE to loxpp_core.
set(LOXPP_CORE_PRIVATE_DEFINITIONS "")
if(LOXPP_DEBUG_PRINT_CODE)
    list(APPEND LOXPP_CORE_PRIVATE_DEFINITIONS LOXPP_DEBUG_PRINT_CODE)
endif()
if(LOXPP_DEBUG_TRACE_EXECUTION)
    list(APPEND LOXPP_CORE_PRIVATE_DEFINITIONS LOXPP_DEBUG_TRACE_EXECUTION)
endif()
option(LOXPP_DEBUG_LOG_GC
    "Log GC mark, trace, and sweep events to stderr" ${_debug_default})
if(LOXPP_DEBUG_LOG_GC)
    list(APPEND LOXPP_CORE_PRIVATE_DEFINITIONS LOXPP_DEBUG_LOG_GC)
endif()
if(LOXPP_CORE_PRIVATE_DEFINITIONS)
    target_compile_definitions(loxpp_core PRIVATE ${LOXPP_CORE_PRIVATE_DEFINITIONS})
endif()

# GC stress (collect on every allocation) is a runtime switch now, not a build
# flag: set the LOXPP_STRESS_GC environment variable to any non-empty value
# when running loxpp or a test binary. MemoryManager reads it once in its
# constructor.

# Runtime profiler: per-opcode counts, function timing, GC stats. It adds data
# members to VM and MemoryManager (src/vm.h, src/memory_manager.h), so the
# whole program must be compiled with it or without it — the layouts cannot be
# link-mixed. loxpp_core is the non-profile variant and stays that way for the
# tooling binaries that link it, so a profiling loxpp is built straight from
# the core sources with the definition instead of linking loxpp_core (see the
# executable section below). benchmarks/profile.py needs this build.
option(LOXPP_PROFILE
    "Enable runtime profiler: per-opcode counts, function timing, GC stats" OFF)

# Value representation: NaN-tagged 8-byte word (default) vs portable
# std::variant (16 bytes). Turn OFF for platforms where heap pointers may
# exceed 48 bits or where the IEEE 754 bit-pattern assumptions do not hold.
option(LOXPP_NAN_TAGGING
    "Use NaN-tagged 8-byte Value instead of std::variant" ON)
if(LOXPP_NAN_TAGGING)
    # PUBLIC: LOXPP_NAN_TAGGING selects the Value layout in src/value.h, a
    # header main.cpp pulls in transitively (vm.h). Every translation unit
    # linked together must agree.
    list(APPEND LOXPP_CORE_PUBLIC_DEFINITIONS LOXPP_NAN_TAGGING)
endif()

if(LOXPP_CORE_PUBLIC_DEFINITIONS)
    target_compile_definitions(loxpp_core PUBLIC ${LOXPP_CORE_PUBLIC_DEFINITIONS})
endif()

# The loxpp executable. main.cpp is the only source it owns; everything else
# comes from loxpp_core.
add_executable(loxpp src/main.cpp)

if(LOXPP_PROFILE)
    # Profiling build: compile main.cpp and the core sources together with
    # LOXPP_PROFILE so VM and MemoryManager have one layout across the whole
    # program. loxpp_core (non-profile) is not linked here, so this build
    # reuses the core definition lists to stay ABI-compatible with the
    # headers.
    target_sources(loxpp PRIVATE ${LOXPP_CORE_SOURCES})
    target_compile_definitions(loxpp PRIVATE
        LOXPP_PROFILE
        ${LOXPP_CORE_PUBLIC_DEFINITIONS}
        ${LOXPP_CORE_PRIVATE_DEFINITIONS})
else()
    target_link_libraries(loxpp PRIVATE loxpp_core)
endif()

# Readline support for the REPL. main.cpp is the only user, so the library,
# the include directory, and the definition stay on the executable.
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
