cmake_minimum_required(VERSION 3.16)
project(fasthtmlparser VERSION 0.1.1 LANGUAGES C)

option(FHP_BUILD_TESTS "Build the tests" ON)
option(FHP_BUILD_SHARED "Build the shared library too" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(FHP_SOURCES
  src/attributes.c
  src/css.c
  src/decode.c
  src/document.c
  src/links.c
  src/parse.c
  src/shortcuts.c
  src/tables_gen.c
  src/text.c
  src/unicode_gen.c
  src/util.c
  src/xpath.c
)

function(fhp_configure target)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                             C_VISIBILITY_PRESET hidden)
  target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                              $<INSTALL_INTERFACE:include>)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /wd4204 /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
  if(UNIX)
    target_link_libraries(${target} PRIVATE m)
  endif()
endfunction()

add_library(fasthtmlparser STATIC ${FHP_SOURCES})
fhp_configure(fasthtmlparser)
set_target_properties(fasthtmlparser PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(FHP_BUILD_SHARED)
  add_library(fasthtmlparser_shared SHARED ${FHP_SOURCES})
  fhp_configure(fasthtmlparser_shared)
  target_compile_definitions(fasthtmlparser_shared PUBLIC FHP_SHARED PRIVATE FHP_BUILDING)
  set_target_properties(fasthtmlparser_shared PROPERTIES OUTPUT_NAME fasthtmlparser
                                                         ARCHIVE_OUTPUT_NAME fasthtmlparser_dll)
endif()

if(FHP_BUILD_TESTS)
  enable_testing()
  add_executable(fhp_tests tests/main.c tests/json.c tests/sha256.c)
  set_target_properties(fhp_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
  target_link_libraries(fhp_tests PRIVATE fasthtmlparser)
  target_include_directories(fhp_tests PRIVATE src)
  if(MSVC)
    target_compile_options(fhp_tests PRIVATE /W3 /utf-8)
    target_compile_definitions(fhp_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  target_compile_definitions(fhp_tests PRIVATE FHP_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
  add_test(NAME fhp_tests COMMAND fhp_tests)
  add_executable(fhp_example tests/example.c)
  target_link_libraries(fhp_example PRIVATE fasthtmlparser)
  set_target_properties(fhp_example PROPERTIES C_STANDARD 99)
  if(MSVC)
    target_compile_options(fhp_example PRIVATE /utf-8)
  endif()
  add_test(NAME fhp_example COMMAND fhp_example)
  set_tests_properties(fhp_example PROPERTIES PASS_REGULAR_EXPRESSION "A Light in the Attic: .*51\\.77")
  if(FHP_BUILD_SHARED)
    add_executable(fhp_smoke_shared tests/smoke.c)
    target_link_libraries(fhp_smoke_shared PRIVATE fasthtmlparser_shared)
    set_target_properties(fhp_smoke_shared PROPERTIES C_STANDARD 99)
    add_test(NAME fhp_smoke_shared COMMAND fhp_smoke_shared)
    if(WIN32)
      set_tests_properties(fhp_smoke_shared PROPERTIES ENVIRONMENT_MODIFICATION
                           "PATH=path_list_prepend:$<TARGET_FILE_DIR:fasthtmlparser_shared>")
    endif()
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS fasthtmlparser EXPORT fasthtmlparserTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(FHP_BUILD_SHARED)
  install(TARGETS fasthtmlparser_shared EXPORT fasthtmlparserTargets
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES include/fasthtmlparser.h include/fasthtmlparser_names.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/fasthtmlparser)
install(EXPORT fasthtmlparserTargets NAMESPACE fasthtmlparser:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fasthtmlparser)
