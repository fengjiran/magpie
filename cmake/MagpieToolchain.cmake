function(magpie_validate_toolchain)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_magpie_minimum_version "11.0")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_magpie_minimum_version "14.0")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(_magpie_minimum_version "14.0")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_magpie_minimum_version "19.34")
    else()
        message(FATAL_ERROR
            "Unsupported C++ compiler '${CMAKE_CXX_COMPILER_ID}'. "
            "Supported compiler families are GCC, Clang, AppleClang, and MSVC.")
    endif()

    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _magpie_minimum_version)
        message(FATAL_ERROR
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} is too old; "
            "magpie requires at least ${_magpie_minimum_version}.")
    endif()

    include(CheckCXXSourceCompiles)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(CMAKE_REQUIRED_FLAGS "/std:c++20")
    else()
        set(CMAKE_REQUIRED_FLAGS "-std=c++20")
    endif()
    check_cxx_source_compiles([[#include <concepts>
#include <span>
static_assert(std::same_as<int, int>);
int main() { int values[1]{}; return std::span{values}.empty(); }
]] MAGPIE_HAS_REQUIRED_CXX20)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT MAGPIE_HAS_REQUIRED_CXX20)
        message(FATAL_ERROR "The selected compiler and standard library do not provide the required C++20 facilities.")
    endif()
endfunction()
