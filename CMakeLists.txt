cmake_minimum_required(VERSION 3.23)
project(Lox++ VERSION 0.1.0 LANGUAGES CXX C)

# Install git hooks on every configure so agents get them automatically.
execute_process(
    COMMAND git config core.hooksPath .githooks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE _git_config_result
)
if(NOT _git_config_result EQUAL 0)
    message(WARNING "Failed to set core.hooksPath — git hooks will not run automatically.")
endif()

# Generate the version header at configure time. The version string comes
# from `git describe --tags --always --dirty`, run once at configure time
# only (not on every build). When the source is not a git checkout, fall
# back to the CMake project version.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LOXPP_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_describe_result
)
if(NOT _git_describe_result EQUAL 0)
    # Not a git checkout; use the project version fallback.
    set(LOXPP_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "loxpp version: ${LOXPP_VERSION}")

# Read the isocline version from third_party/isocline/README.md. The
# version is on the line "| Release tag | `v<version>` |".
file(READ "${CMAKE_SOURCE_DIR}/third_party/isocline/README.md" _isocline_readme)
if(_isocline_readme MATCHES "Release tag.*`v([0-9.]+)`")
    set(LOXPP_ISOCLINE_VERSION "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Could not extract isocline version from third_party/isocline/README.md")
endif()
message(STATUS "bundled isocline version: ${LOXPP_ISOCLINE_VERSION}")

# Configure the version header template into the build directory.
configure_file(
    "${CMAKE_SOURCE_DIR}/src/loxpp_version.h.in"
    "${CMAKE_BINARY_DIR}/loxpp_version.h"
    @ONLY
)
message(STATUS "loxpp_version.h configured at ${CMAKE_BINARY_DIR}/loxpp_version.h")

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

# The interpreter front-end and runtime, shared by the loxpp executable, the
# loxpp-lsp binary, and the src/tooling analysers. main.cpp is the only
# source that stays on the executable. jvm_emitter.cpp / clr_emitter.cpp are
# added below, gated on their options.
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
    src/analyze.cpp
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

# Wire the generated version header as an explicit dependency of loxpp_core.
# loxpp_core is an OBJECT library; its TUs #include the header, so it must
# be an explicit dependency to avoid race conditions in clean parallel builds.
target_sources(loxpp_core PRIVATE "${CMAKE_BINARY_DIR}/loxpp_version.h")
target_include_directories(loxpp_core PUBLIC "${CMAKE_BINARY_DIR}")

# PUBLIC definitions of loxpp_core: they select a type layout in a header that
# main.cpp reaches, so every translation unit linked with the library must
# agree. Collected here and applied once below. The profiling loxpp does not
# link loxpp_core (see the executable section), so it reuses this same list to
# stay layout-compatible.
set(LOXPP_CORE_PUBLIC_DEFINITIONS "")

# The JVM code generator. Default ON so CI builds and checks it; OFF drops
# --target jvm from the CLI for a build that wants the native VM only.
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

# Standalone editor-tooling analysers. The tooling parser builds its own AST
# with source spans; it reuses only the Scanner and Token from the core, so
# it links loxpp_core rather than recompiling scanner.cpp / token.cpp -- that
# keeps one copy of those objects in the build graph, which the loxpp-lsp
# binary needs when it links loxpp_core and the src/tooling objects together.
# loxpp_tooling carries only the new tooling sources.
add_library(loxpp_tooling OBJECT
    src/tooling/tooling_parser.cpp
    src/tooling/symbol_table.cpp
    src/tooling/stdlib_names.cpp
    src/tooling/resolver.cpp
    src/tooling/document_model.cpp
)
target_include_directories(loxpp_tooling PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(loxpp_tooling PUBLIC loxpp_core)

# The loxpp executable. main.cpp is the only source it owns; everything else
# comes from loxpp_core.
add_executable(loxpp src/main.cpp)

if(LOXPP_PROFILE)
    # Profiling build: compile main.cpp and the core sources together with
    # LOXPP_PROFILE so VM and MemoryManager have one layout across the whole
    # program. loxpp_core (non-profile) is not linked here, so this build
    # reuses the core definition lists to stay ABI-compatible with the
    # headers. The generated version header is needed on the include path.
    target_sources(loxpp PRIVATE ${LOXPP_CORE_SOURCES})
    target_include_directories(loxpp PRIVATE "${CMAKE_BINARY_DIR}")
    target_compile_definitions(loxpp PRIVATE
        LOXPP_PROFILE
        ${LOXPP_CORE_PUBLIC_DEFINITIONS}
        ${LOXPP_CORE_PRIVATE_DEFINITIONS})
else()
    target_link_libraries(loxpp PRIVATE loxpp_core)
endif()

# isocline line editor for the REPL. Single translation unit build from
# third_party/isocline/src/isocline.c. See third_party/isocline/README.md.
add_library(loxpp_isocline STATIC
    third_party/isocline/src/isocline.c
)
target_include_directories(loxpp_isocline PRIVATE third_party/isocline/include)
target_include_directories(loxpp PRIVATE third_party/isocline/include)
target_link_libraries(loxpp PRIVATE loxpp_isocline)

# The loxpp-lsp language server. A second binary in the same build. It links
# loxpp_core (Scanner + analyze()) and the src/tooling analysers, and vendors
# nlohmann/json from third_party/. Default ON so CI builds it; OFF drops it
# for a build that wants the interpreter only.
option(LOXPP_LSP "Build the loxpp-lsp language server" ON)
if(LOXPP_LSP)
    find_package(Threads REQUIRED)
    add_executable(loxpp-lsp
        src/lsp/main.cpp
        src/lsp/json_rpc.cpp
        src/lsp/document_store.cpp
        src/lsp/server.cpp
        src/lsp/stdlib_docs.cpp
    )
    # loxpp_tooling is an OBJECT library that PUBLIC-links loxpp_core; CMake
    # does not carry one OBJECT library's objects through another, so the
    # executable links loxpp_core directly too (same as test/CMakeLists.txt).
    target_link_libraries(loxpp-lsp PRIVATE
        loxpp_tooling loxpp_core Threads::Threads)
    target_include_directories(loxpp-lsp PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
endif()

option(LOXPP_BUILD_TESTS "Build the GTest suite" ON)

if(LOXPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Install rules for the release channel. Use GNUInstallDirs for all paths.
include(GNUInstallDirs)

# Install the loxpp binary.
install(TARGETS loxpp RUNTIME COMPONENT runtime)

# Install the man page.
install(FILES man/loxpp.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT runtime)

# Install shell completions to the conventional locations.
install(FILES completions/loxpp.bash
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
        RENAME loxpp
        COMPONENT runtime)
install(FILES completions/loxpp.zsh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions
        RENAME _loxpp
        COMPONENT runtime)
install(FILES completions/loxpp.fish
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d
        COMPONENT runtime)

# Install documentation.
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples COMPONENT runtime)
install(FILES LICENSE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime)

# CPack configuration for source and binary packages.
include(CPack)
set(CPACK_PACKAGE_NAME "loxpp")
set(CPACK_PACKAGE_VERSION "${LOXPP_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lox++ interpreter")
set(CPACK_PACKAGE_VENDOR "txloc1909")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/txloc1909/loxpp")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TXZ")
# Add source generator (default is ZIP on Windows, TGZ elsewhere).
list(APPEND CPACK_GENERATOR "TGZ")
# Component-aware packaging.
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
