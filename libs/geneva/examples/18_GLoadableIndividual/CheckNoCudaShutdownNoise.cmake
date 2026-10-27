# Regression guard for the GCudaRNG teardown race (libs/hap/src/GCUDARng.cu).
#
# At process exit the CUDA runtime unloads (atexit) while the GRandomFactory's producer threads may still be
# refilling on the GPU. cuRAND reports that as a generic CURAND_STATUS_LAUNCH_FAILURE which -- before the fix
# -- was printed to stderr as "GCudaRNG: cuRAND call failed (...)", because the cuRAND error path could not
# recognise shutdown on its own. The race is non-deterministic (about 40% of runs on the reference machine),
# so this guard runs the reproducer many times and fails if the noise appears in ANY run. It is registered
# only when the CUDA RNG backend is built (GENEVA_USE_CUDA_RNG_ENABLED) -- the defect cannot occur otherwise.
#
# Driven via `cmake -P` with -DBIN, -DPLUGIN, -DWORKDIR and optionally -DRUNS.

if(NOT DEFINED RUNS)
    set(RUNS 20)
endif()

set(_bad_runs 0)
foreach(_i RANGE 1 ${RUNS})
    execute_process(
        COMMAND "${BIN}" --individual "${PLUGIN}"
        WORKING_DIRECTORY "${WORKDIR}"
        OUTPUT_QUIET
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    if(_err MATCHES "cuRAND call failed")
        math(EXPR _bad_runs "${_bad_runs}+1")
    endif()
endforeach()

if(_bad_runs GREATER 0)
    message(FATAL_ERROR
        "GCudaRNG teardown regression: 'cuRAND call failed' appeared on stderr in "
        "${_bad_runs} of ${RUNS} runs. The cuRAND shutdown-noise suppression in "
        "libs/hap/src/GCUDARng.cu (checkCurand) has regressed.")
endif()

message(STATUS "GCudaRngShutdownNoise: clean -- no cuRAND teardown noise across ${RUNS} runs")
