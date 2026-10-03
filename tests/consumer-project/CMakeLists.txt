cmake_minimum_required(VERSION 3.21)
project(magpie_external_consumer LANGUAGES CXX)

if(NOT DEFINED MAGPIE_SOURCE_DIR)
    message(FATAL_ERROR "MAGPIE_SOURCE_DIR must point to a magpie source checkout.")
endif()

set(BUILD_TESTING OFF CACHE BOOL "Keep the nested magpie build production-only" FORCE)
set(MAGPIE_ENABLE_TEST_HOOKS OFF CACHE BOOL "Keep the nested magpie build production-only" FORCE)
add_subdirectory("${MAGPIE_SOURCE_DIR}" magpie-source EXCLUDE_FROM_ALL)

add_executable(magpie_external_consumer main.cpp)
target_link_libraries(magpie_external_consumer PRIVATE magpie::magpie)
magpie_apply_project_defaults(magpie_external_consumer)
if(MAGPIE_EXPECT_SHARED)
    target_compile_definitions(magpie_external_consumer PRIVATE MAGPIE_EXPECT_SHARED=1)
endif()

enable_testing()
add_test(NAME external-consumer-runs COMMAND magpie_external_consumer)
