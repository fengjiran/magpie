cmake_minimum_required(VERSION 3.21)

project(magpie
    VERSION 0.1.0
    DESCRIPTION "A C++20 work-stealing thread pool"
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)
include(cmake/MagpieToolchain.cmake)
include(cmake/MagpieOptions.cmake)

set(MAGPIE_SANITIZER "none" CACHE STRING "Sanitizer configuration: none, thread, or address-undefined")
set_property(CACHE MAGPIE_SANITIZER PROPERTY STRINGS none thread address-undefined)

magpie_validate_toolchain()
magpie_validate_options()

string(TOLOWER "${CMAKE_SYSTEM_NAME}" MAGPIE_SYSTEM_NAME_LOWER)
if(MAGPIE_SYSTEM_NAME_LOWER STREQUAL "darwin")
    set(MAGPIE_PLATFORM_NAME "macOS")
elseif(MAGPIE_SYSTEM_NAME_LOWER STREQUAL "linux")
    set(MAGPIE_PLATFORM_NAME "Linux")
elseif(MAGPIE_SYSTEM_NAME_LOWER STREQUAL "windows")
    set(MAGPIE_PLATFORM_NAME "Windows")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(MAGPIE_COMPILER_DESCRIPTION "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(MAGPIE_SANITIZER_DESCRIPTION "${MAGPIE_SANITIZER}")
set(MAGPIE_CACHE_LINE_VALUE "${MAGPIE_CACHE_LINE}")
configure_file(
    cmake/build_config.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/magpie/build_config.hpp"
    @ONLY
)

add_library(magpie_sanitizers INTERFACE)
if(MAGPIE_SANITIZER STREQUAL "thread")
    target_compile_options(magpie_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(magpie_sanitizers INTERFACE -fsanitize=thread)
elseif(MAGPIE_SANITIZER STREQUAL "address-undefined")
    target_compile_options(magpie_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(magpie_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

if(MAGPIE_BUILD_SHARED)
    add_library(magpie SHARED src/build_info.cpp)
else()
    add_library(magpie STATIC src/build_info.cpp)
    target_compile_definitions(magpie PUBLIC MAGPIE_STATIC_DEFINE)
endif()
add_library(magpie::magpie ALIAS magpie)

target_compile_features(magpie PUBLIC cxx_std_20)
target_include_directories(magpie
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)
target_compile_definitions(magpie PRIVATE MAGPIE_BUILDING_LIBRARY)
target_compile_definitions(magpie PUBLIC "MAGPIE_BUILD_TYPE=\"$<CONFIG>\"")
target_link_libraries(magpie PUBLIC magpie_sanitizers)
if(MSVC)
    target_compile_options(magpie PUBLIC /Zc:__cplusplus)
endif()
set_target_properties(magpie PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE YES
)
magpie_apply_project_defaults(magpie)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(MAGPIE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
