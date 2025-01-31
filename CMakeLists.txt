cmake_minimum_required(VERSION 3.23)
project(Lox++ VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${PROJECT_SOURCE_DIR}/src)

add_executable(loxpp src/main.cpp src/chunk.cpp src/value.cpp)

enable_testing()
add_subdirectory(test)
