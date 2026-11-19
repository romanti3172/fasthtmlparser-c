# Benchmarks of fasthtmlparser against lexbor and Gumbo, in their own project so that the library has no dependencies.
#   cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build
#   bench/build/fhp_bench            (--check: only check that every library extracts the same records)
cmake_minimum_required(VERSION 3.16)
project(fasthtmlparser_bench LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# lexbor: a fast HTML5 parser with CSS selectors (C99)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lexbor
  GIT_REPOSITORY https://github.com/lexbor/lexbor.git
  GIT_TAG v3.0.1
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(lexbor)

# Gumbo: the HTML5 parser of Google, now maintained on Codeberg; plain C99 sources, built here directly
FetchContent_Declare(gumbo
  GIT_REPOSITORY https://codeberg.org/gumbo-parser/gumbo-parser.git
  GIT_TAG 0.14.1
  GIT_SHALLOW ON
  SOURCE_SUBDIR no-cmake-project) # sources only: built below
FetchContent_MakeAvailable(gumbo)
file(GLOB GUMBO_SOURCES ${gumbo_SOURCE_DIR}/src/*.c)
add_library(gumbo STATIC ${GUMBO_SOURCES})
target_include_directories(gumbo PUBLIC ${gumbo_SOURCE_DIR}/src)
if(MSVC)
  target_include_directories(gumbo PUBLIC ${gumbo_SOURCE_DIR}/visualc/include)
  target_compile_options(gumbo PRIVATE /w)
else()
  target_compile_options(gumbo PRIVATE -w)
endif()

# fasthtmlparser from the parent directory
set(FHP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FHP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(.. fasthtmlparser)

add_executable(fhp_bench bench.c)
set_target_properties(fhp_bench PROPERTIES C_STANDARD 99)
target_link_libraries(fhp_bench PRIVATE fasthtmlparser lexbor_static gumbo)
target_compile_definitions(fhp_bench PRIVATE FHP_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/../testdata" LEXBOR_STATIC)
if(MSVC)
  target_compile_definitions(fhp_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
