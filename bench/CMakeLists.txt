# Benchmarks remain opt-in. M2 will add benchmark targets through this
# directory; keeping it separate prevents benchmark-only dependencies and
# flags from entering the production library or test targets.
function(magpie_add_benchmark target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE magpie::magpie)
endfunction()

message(STATUS "Benchmark support is enabled; no benchmark programs are defined in M0.")
