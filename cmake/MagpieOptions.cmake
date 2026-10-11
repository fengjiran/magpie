option(MAGPIE_BUILD_SHARED "Build magpie as a shared library" OFF)
option(MAGPIE_BUILD_BENCHMARKS "Enable benchmark targets" OFF)
set(MAGPIE_CACHE_LINE "64" CACHE STRING "Configured cache-line alignment in bytes")

# M6 标定参数：默认空 = 使用 src/thread_pool.cpp 内置默认（主设计 §16.2）。
# 取值空间与 design/benchmark-plan.md §5 一致，只允许枚举白名单。
set(MAGPIE_BULK_LIMIT "" CACHE STRING "M6 calibration: global dequeue_bulk limit {1,4,16,32,64,128}; empty = built-in default")
set(MAGPIE_STEAL_CAP "" CACHE STRING "M6 calibration: per-victim single-item steal attempts {1,4,16,32,64,128}; empty = built-in default")
set(MAGPIE_SPIN_LIMIT "" CACHE STRING "M6 calibration: cpu_relax spin count {0,16,64,256,1024}; empty = built-in default")
set(MAGPIE_YIELD_LIMIT "" CACHE STRING "M6 calibration: yield count {0,4,8,16}; empty = built-in default")
set(MAGPIE_VICTIM_TRIES "" CACHE STRING "M6 calibration: random victim probes per round {1,2,4}; empty = built-in default")

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

function(magpie_apply_calibration_defs target)
    foreach(_macro IN ITEMS
            MAGPIE_BULK_LIMIT MAGPIE_STEAL_CAP MAGPIE_SPIN_LIMIT
            MAGPIE_YIELD_LIMIT MAGPIE_VICTIM_TRIES)
        if(NOT "${${_macro}}" STREQUAL "")
            target_compile_definitions(${target} PRIVATE "${_macro}=${${_macro}}")
        endif()
    endforeach()
endfunction()

function(magpie_validate_options)
    if(NOT MAGPIE_SANITIZER MATCHES "^(none|thread|address-undefined)$")
        message(FATAL_ERROR
            "MAGPIE_SANITIZER must be one of: none, thread, address-undefined; got '${MAGPIE_SANITIZER}'.")
    endif()

    # M6 标定枚举白名单（benchmark-plan §5）。空值表示使用内置默认。
    foreach(_name IN ITEMS BULK_LIMIT STEAL_CAP SPIN_LIMIT YIELD_LIMIT VICTIM_TRIES)
        string(TOUPPER "${_name}" _upper)
        set(_value "${MAGPIE_${_upper}}")
        if(NOT _value STREQUAL "" AND NOT _value MATCHES "^[0-9]+$")
            message(FATAL_ERROR "MAGPIE_${_upper} must be an integer from the calibration space or empty; got '${_value}'.")
        endif()
    endforeach()
    if(NOT MAGPIE_BULK_LIMIT MATCHES "^(|1|4|16|32|64|128)$")
        message(FATAL_ERROR "MAGPIE_BULK_LIMIT must be one of: 1,4,16,32,64,128; got '${MAGPIE_BULK_LIMIT}'.")
    endif()
    if(NOT MAGPIE_STEAL_CAP MATCHES "^(|1|4|16|32|64|128)$")
        message(FATAL_ERROR "MAGPIE_STEAL_CAP must be one of: 1,4,16,32,64,128; got '${MAGPIE_STEAL_CAP}'.")
    endif()
    if(NOT MAGPIE_SPIN_LIMIT MATCHES "^(|0|16|64|256|1024)$")
        message(FATAL_ERROR "MAGPIE_SPIN_LIMIT must be one of: 0,16,64,256,1024; got '${MAGPIE_SPIN_LIMIT}'.")
    endif()
    if(NOT MAGPIE_YIELD_LIMIT MATCHES "^(|0|4|8|16)$")
        message(FATAL_ERROR "MAGPIE_YIELD_LIMIT must be one of: 0,4,8,16; got '${MAGPIE_YIELD_LIMIT}'.")
    endif()
    if(NOT MAGPIE_VICTIM_TRIES MATCHES "^(|1|2|4)$")
        message(FATAL_ERROR "MAGPIE_VICTIM_TRIES must be one of: 1,2,4; got '${MAGPIE_VICTIM_TRIES}'.")
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
