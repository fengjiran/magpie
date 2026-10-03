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
            --queue-backend "${MAGPIE_QUEUE_BACKEND}"
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

add_executable(magpie_pool_tests unit/pool_test.cpp unit/api_compile_test.cpp)
if(MAGPIE_ENABLE_TEST_HOOKS)
    target_link_libraries(magpie_pool_tests PRIVATE magpie_test_support GTest::gtest_main
        magpie_sanitizers)
    target_compile_definitions(magpie_pool_tests PRIVATE MAGPIE_ENABLE_TEST_HOOKS=1)
else()
    target_link_libraries(magpie_pool_tests PRIVATE magpie::magpie GTest::gtest_main
        magpie_sanitizers)
endif()
magpie_apply_project_defaults(magpie_pool_tests)
add_test(NAME magpie_pool_tests COMMAND magpie_pool_tests)
set_tests_properties(magpie_pool_tests PROPERTIES LABELS fast TIMEOUT 20)
magpie_add_test_suite(magpie_pool_stress_tests stress
    unit/pool_stress_test.cpp
)

if(NOT MAGPIE_SANITIZER STREQUAL "thread")
    magpie_add_test_suite(magpie_pool_death_tests fast
        unit/pool_death_test.cpp
    )
    set_tests_properties(magpie_pool_death_tests PROPERTIES TIMEOUT 20)
endif()

if(MAGPIE_BUILD_BENCHMARKS)
    magpie_add_test_suite(magpie_bench_tests fast
        bench/harness_test.cpp ../bench/harness.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(magpie_bench_tests PRIVATE _GNU_SOURCE=1)
    endif()
    add_test(NAME magpie.benchmark-runner
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench/test_runner.py")
    set_tests_properties(magpie.benchmark-runner PROPERTIES LABELS fast TIMEOUT 20)
endif()

# Header-only primitive test instantiations use uint64_t/int, not the pool's
# production Task* specialization. Hooks are confined to these test binaries.
magpie_add_test_suite(magpie_mpmc_tests fast unit/mpmc_queue_test.cpp)
target_compile_definitions(magpie_mpmc_tests PRIVATE MAGPIE_ENABLE_TEST_HOOKS=1)
set_tests_properties(magpie_mpmc_tests PROPERTIES TIMEOUT 20)
magpie_add_test_suite(magpie_mpmc_stress_tests stress unit/mpmc_queue_stress_test.cpp)
target_compile_definitions(magpie_mpmc_stress_tests PRIVATE MAGPIE_ENABLE_TEST_HOOKS=1)
if(NOT MAGPIE_SANITIZER STREQUAL "thread")
    magpie_add_test_suite(magpie_mpmc_death_tests fast unit/mpmc_queue_death_test.cpp)
    target_compile_definitions(magpie_mpmc_death_tests PRIVATE MAGPIE_ENABLE_TEST_HOOKS=1)
    set_tests_properties(magpie_mpmc_death_tests PROPERTIES TIMEOUT 20)
endif()

if(MAGPIE_ENABLE_TEST_HOOKS)
    add_executable(magpie_pool_mpmc_tests unit/pool_mpmc_test.cpp)
    target_link_libraries(magpie_pool_mpmc_tests PRIVATE magpie_test_support GTest::gtest_main magpie_sanitizers)
    target_compile_definitions(magpie_pool_mpmc_tests PRIVATE MAGPIE_ENABLE_TEST_HOOKS=1)
    magpie_apply_project_defaults(magpie_pool_mpmc_tests)
    add_test(NAME magpie_pool_mpmc_tests COMMAND magpie_pool_mpmc_tests)
    set_tests_properties(magpie_pool_mpmc_tests PROPERTIES LABELS fast TIMEOUT 20)
endif()

add_executable(magpie_watchdog_probe support/watchdog_probe.cpp)
target_link_libraries(magpie_watchdog_probe PRIVATE Threads::Threads magpie_sanitizers)
magpie_apply_project_defaults(magpie_watchdog_probe)
add_test(NAME magpie.watchdog-probe
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/support/check_watchdog.py"
            $<TARGET_FILE:magpie_watchdog_probe>)
set_tests_properties(magpie.watchdog-probe PROPERTIES LABELS fast TIMEOUT 10)

if(MAGPIE_BUILD_BENCHMARKS)
    add_test(NAME magpie.benchmark-queue-modes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench/test_queue_modes.py"
                $<TARGET_FILE:magpie_bench>)
    set_tests_properties(magpie.benchmark-queue-modes PROPERTIES LABELS fast TIMEOUT 20)
endif()
