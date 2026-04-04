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
    src/value.cpp 
    src/object.cpp
    src/table.cpp
    src/simple_allocator.cpp
    src/vm.cpp 
    src/scanner.cpp 
    src/token.cpp 
    src/compiler.cpp
    src/parser.cpp
)

enable_testing()
add_subdirectory(test)
