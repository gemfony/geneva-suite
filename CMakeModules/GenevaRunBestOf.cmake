# Copyright (c) Gemfony scientific UG (haftungsbeschraenkt)
#
# Runs a stochastic test program up to ATTEMPTS times and succeeds as soon as one run exits with 0.
# Invoked with `cmake -P`:
#
#   cmake -DPROGRAM=<path> -DATTEMPTS=<n> [-DARGS=<a;b;...>] -P GenevaRunBestOf.cmake
#
# Geneva's random numbers are seeded from system entropy, so a program that checks a stochastic search
# against a known optimum fails now and then although nothing is wrong. Repeating the run keeps the
# program's own tolerance tight: a broken search misses in every attempt, a working one rarely misses
# twice in a row. Each attempt's output is passed through, so a failure stays readable.

if(NOT DEFINED PROGRAM OR NOT DEFINED ATTEMPTS)
	message(FATAL_ERROR "GenevaRunBestOf.cmake: PROGRAM and ATTEMPTS must be set")
endif()

foreach(_attempt RANGE 1 ${ATTEMPTS})
	execute_process(COMMAND ${PROGRAM} ${ARGS} RESULT_VARIABLE _rc)
	if(_rc EQUAL 0)
		if(_attempt GREATER 1)
			message(STATUS "GenevaRunBestOf: succeeded in attempt ${_attempt} of ${ATTEMPTS}")
		endif()
		return()
	endif()
	message(STATUS "GenevaRunBestOf: attempt ${_attempt} of ${ATTEMPTS} failed (exit ${_rc})")
endforeach()

message(FATAL_ERROR "GenevaRunBestOf: all ${ATTEMPTS} attempts of ${PROGRAM} failed")
