################################################################################
#
# This file is part of the Geneva library collection. The following license
# applies to this file:
#
# ------------------------------------------------------------------------------
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# Note that other files in the Geneva library collection may use a different
# license. Please see the licensing information in each file.
#
################################################################################
#
# See the NOTICE file in the top-level directory of the Geneva library
# collection for a list of contributors and copyright information.
#
################################################################################
#
# Driver of the test GenevaMpiTestDirectories: GenevaRunMpiTest.sh (the runner of every entry
# registered with GENEVA_ADD_MPI_TEST) gives each run a directory of its own for Open MPI's
# shared-memory files and removes it, whichever way the run ends.
#
#  1. Two runs at the same time (execute_process starts the commands of a pipeline together);
#     each writes the directory it was given to a file, if that exists and is writable, and waits
#     two seconds, so that the runs overlap. The two directories must differ, lie below /dev/shm
#     where that is writable, and be gone afterwards.
#  2. A run whose command fails: its exit code is passed on, the directory is gone.
#  3. A run whose command outlasts the bound: it ends at the bound with timeout's 124 (where
#     coreutils' timeout exists), the directory is gone.
#
# Inputs (via -D): RUNNER (the script), WORK (a directory for the files of the runs).
#
################################################################################

FOREACH(_var RUNNER WORK)
	IF(NOT DEFINED ${_var})
		MESSAGE(FATAL_ERROR "CheckMpiTestDirectories.cmake needs -D${_var}=...")
	ENDIF()
ENDFOREACH()
FILE(REMOVE_RECURSE "${WORK}")
FILE(MAKE_DIRECTORY "${WORK}")

# The command of a run: writes the directory it was given to <file> if it exists and is writable
SET(_note [=[d=$OMPI_MCA_btl_sm_backing_directory
if [ -d "$d" ] && [ -w "$d" ]; then echo "$d" > "$1"; fi]=])

# Reads the directory a run noted in <file> into <out>; fails if there is none, or if it is still
# there
FUNCTION(cmtd_noted _file _out)
	IF(NOT EXISTS "${_file}")
		MESSAGE(FATAL_ERROR "The run behind ${_file} was given no directory that exists")
	ENDIF()
	FILE(STRINGS "${_file}" _dir LIMIT_COUNT 1)
	IF(_dir STREQUAL "")
		MESSAGE(FATAL_ERROR "The run behind ${_file} was given no directory")
	ENDIF()
	IF(EXISTS "${_dir}")
		MESSAGE(FATAL_ERROR "The directory ${_dir} is left after its run")
	ENDIF()
	SET(${_out} "${_dir}" PARENT_SCOPE)
ENDFUNCTION()

# 1. Two runs at the same time
EXECUTE_PROCESS(
	COMMAND "${RUNNER}" 60 sh -c "${_note}; sleep 2" sh "${WORK}/first"
	COMMAND "${RUNNER}" 60 sh -c "${_note}; sleep 2" sh "${WORK}/second"
	RESULTS_VARIABLE _rcs
)
IF(NOT _rcs STREQUAL "0;0")
	MESSAGE(FATAL_ERROR "Two runs at the same time ended with ${_rcs}, not 0;0")
ENDIF()
cmtd_noted("${WORK}/first" _first)
cmtd_noted("${WORK}/second" _second)
IF(_first STREQUAL _second)
	MESSAGE(FATAL_ERROR "Two runs at the same time were given the same directory ${_first}")
ENDIF()
IF(IS_DIRECTORY /dev/shm)
	FOREACH(_dir IN ITEMS "${_first}" "${_second}")
		IF(NOT _dir MATCHES "^/dev/shm/")
			MESSAGE(FATAL_ERROR "The directory ${_dir} does not lie below /dev/shm")
		ENDIF()
	ENDFOREACH()
ENDIF()
MESSAGE(STATUS "Two runs at the same time: ${_first} and ${_second}, both removed")

# 2. A run whose command fails
EXECUTE_PROCESS(
	COMMAND "${RUNNER}" 60 sh -c "${_note}; exit 3" sh "${WORK}/failed"
	RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 3)
	MESSAGE(FATAL_ERROR "A run whose command ended with 3 ended with ${_rc}")
ENDIF()
cmtd_noted("${WORK}/failed" _failed)
MESSAGE(STATUS "A run that fails: 3 passed on, ${_failed} removed")

# 3. A run that outlasts its bound
FIND_PROGRAM(_timeout_program timeout)
IF(_timeout_program)
	STRING(TIMESTAMP _start "%s")
	EXECUTE_PROCESS(
		COMMAND "${RUNNER}" 2 sh -c "${_note}; sleep 60" sh "${WORK}/bounded"
		RESULT_VARIABLE _rc
		TIMEOUT 40
	)
	STRING(TIMESTAMP _end "%s")
	MATH(EXPR _took "${_end} - ${_start}")
	IF(NOT _rc EQUAL 124)
		MESSAGE(FATAL_ERROR "A run that outlasts its bound of 2 s ended with ${_rc} after ${_took} s, "
			"not with 124")
	ENDIF()
	cmtd_noted("${WORK}/bounded" _bounded)
	MESSAGE(STATUS "A run that outlasts its bound: ended after ${_took} s, ${_bounded} removed")
ELSE()
	MESSAGE(STATUS "No timeout program: the runner has no bound to check")
ENDIF()
FILE(REMOVE_RECURSE "${WORK}")
