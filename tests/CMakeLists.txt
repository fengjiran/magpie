include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

find_package(GTest 1.14.0 EXACT CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared CRT on Windows" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(magpie_consumer_smoke consumer/main.cpp)
target_link_libraries(magpie_consumer_smoke PRIVATE magpie::magpie)
magpie_apply_project_defaults(magpie_consumer_smoke)
add_test(NAME magpie.consumer-smoke COMMAND magpie_consumer_smoke)
set_tests_properties(magpie.consumer-smoke PROPERTIES LABELS fast TIMEOUT 300)

foreach(_linkage IN ITEMS static shared)
    if(_linkage STREQUAL "shared")
        set(_shared_option ON)
    else()
        set(_shared_option OFF)
    endif()
    add_test(
        NAME magpie.consumer-project.${_linkage}
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/consumer-project/build_consumer.py"
            --consumer-source "${CMAKE_CURRENT_SOURCE_DIR}/consumer-project"
            --magpie-source "${PROJECT_SOURCE_DIR}"
            --binary-dir "${PROJECT_BINARY_DIR}/consumer-project-${_linkage}"
            --generator "${CMAKE_GENERATOR}"
            --compiler "${CMAKE_CXX_COMPILER}"
            --configuration "$<CONFIG>"
            --sanitizer "${MAGPIE_SANITIZER}"
            --cache-line "${MAGPIE_CACHE_LINE}"
            --shared "${_shared_option}"
    )
    set_tests_properties(magpie.consumer-project.${_linkage} PROPERTIES LABELS fast TIMEOUT 300)
endforeach()

add_executable(magpie_unit_tests unit/test_build_info.cpp)
target_link_libraries(magpie_unit_tests PRIVATE magpie::magpie GTest::gtest_main)
target_link_libraries(magpie_unit_tests PRIVATE magpie_sanitizers)
magpie_apply_project_defaults(magpie_unit_tests)
add_test(NAME magpie.unit COMMAND magpie_unit_tests)
set_tests_properties(magpie.unit PROPERTIES LABELS fast TIMEOUT 300)

# Each future test executable is registered through this helper so the CTest
# category, per-process timeout, and sanitizer linkage remain explicit.
function(magpie_add_test_suite target label)
    if(NOT label MATCHES "^(fast|stress|soak)$")
        message(FATAL_ERROR "Unknown test label '${label}' for ${target}.")
    endif()
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE magpie::magpie GTest::gtest_main magpie_sanitizers)
    magpie_apply_project_defaults(${target})
    if(label STREQUAL "fast")
        set(_timeout 300)
    elseif(label STREQUAL "stress")
        set(_timeout 3600)
    else()
        set(_timeout 86400)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS ${label} TIMEOUT ${_timeout})
endfunction()
