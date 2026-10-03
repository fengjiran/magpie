option(MAGPIE_BUILD_SHARED "Build magpie as a shared library" OFF)
option(MAGPIE_BUILD_BENCHMARKS "Enable benchmark targets" OFF)
set(MAGPIE_CACHE_LINE "64" CACHE STRING "Configured cache-line alignment in bytes")

function(magpie_apply_project_defaults target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-O2>")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:/O2>")
        target_compile_options(${target} PRIVATE /W4 /WX)
    endif()
endfunction()

function(magpie_validate_options)
    if(NOT MAGPIE_SANITIZER MATCHES "^(none|thread|address-undefined)$")
        message(FATAL_ERROR
            "MAGPIE_SANITIZER must be one of: none, thread, address-undefined; got '${MAGPIE_SANITIZER}'.")
    endif()

    if(NOT MAGPIE_CACHE_LINE MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "MAGPIE_CACHE_LINE must be a positive integer.")
    endif()
    if(MAGPIE_CACHE_LINE LESS 16 OR MAGPIE_CACHE_LINE GREATER 4096)
        message(FATAL_ERROR "MAGPIE_CACHE_LINE must be between 16 and 4096 bytes.")
    endif()
    math(EXPR _magpie_cacheline_mask "${MAGPIE_CACHE_LINE} & (${MAGPIE_CACHE_LINE} - 1)")
    if(NOT _magpie_cacheline_mask EQUAL 0)
        message(FATAL_ERROR "MAGPIE_CACHE_LINE must be a power of two between 16 and 4096 bytes.")
    endif()

    if(MAGPIE_SANITIZER STREQUAL "thread" OR MAGPIE_SANITIZER STREQUAL "address-undefined")
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
            message(FATAL_ERROR "The requested sanitizer is supported only with GCC or Clang-family compilers.")
        endif()
    endif()
endfunction()
