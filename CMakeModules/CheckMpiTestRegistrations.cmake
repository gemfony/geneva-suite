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
# Driver of the test GenevaMpiTestsRegistered: every CTest entry of the build tree whose command
# names the MPI launcher -- directly, or as an argument of a driver (CheckJobEnds.cmake's COMMAND,
# the MPIRUN of the speed-up checks) -- runs under GenevaRunMpiTest.sh, that is, was registered with
# GENEVA_ADD_MPI_TEST (GenevaMpiTest.cmake), so that it has a directory of its own for Open MPI's
# shared-memory files; and that its TIMEOUT was not lowered afterwards below the runner's bound
# plus the margin the runner needs to end the command and remove the directory. The entries are
# read from "ctest --show-only=json-v1".
#
# The check sees the launcher where an entry's command names it. A driver that found it at run
# time (from PATH) would not be seen; the drivers of the tree are therefore given the launcher
# (the speed-up checks refuse to run without MPIRUN, CheckJobEnds.cmake takes the whole command).
#
# Inputs (via -D): CTEST (the ctest program), BUILD_DIR (the build tree), MPIEXEC (the launcher),
# RUNNER (GenevaRunMpiTest.sh), MARGIN (GENEVA_MPI_TEST_MARGIN of GenevaMpiTest.cmake).
#
################################################################################

FOREACH(_var CTEST BUILD_DIR MPIEXEC RUNNER MARGIN)
	IF(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
		MESSAGE(FATAL_ERROR "CheckMpiTestRegistrations.cmake needs -D${_var}=...")
	ENDIF()
ENDFOREACH()

EXECUTE_PROCESS(
	COMMAND "${CTEST}" --show-only=json-v1
	WORKING_DIRECTORY "${BUILD_DIR}"
	OUTPUT_VARIABLE _json
	ERROR_VARIABLE _err
	RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 0)
	MESSAGE(FATAL_ERROR "ctest --show-only=json-v1 failed (${_rc}): ${_err}")
ENDIF()

STRING(JSON _n LENGTH "${_json}" tests)
SET(_with_launcher 0)
SET(_without_runner "")
SET(_short_timeout "")
MATH(EXPR _last "${_n} - 1")
FOREACH(_i RANGE ${_last})
	STRING(JSON _name GET "${_json}" tests ${_i} name)
	STRING(JSON _command ERROR_VARIABLE _no_command GET "${_json}" tests ${_i} command)
	IF(_no_command)
		CONTINUE()
	ENDIF()
	STRING(FIND "${_command}" "${MPIEXEC}" _at)
	IF(_at EQUAL -1 OR _command MATCHES "CheckMpiTestRegistrations\\.cmake")
		CONTINUE() # no launcher; or this check, which names it to look for it
	ENDIF()
	MATH(EXPR _with_launcher "${_with_launcher} + 1")
	STRING(JSON _program GET "${_json}" tests ${_i} command 0)
	IF(NOT _program STREQUAL "${RUNNER}")
		LIST(APPEND _without_runner "${_name}")
		CONTINUE()
	ENDIF()
	STRING(JSON _bound GET "${_json}" tests ${_i} command 1)
	SET(_timeout 0)
	STRING(JSON _n_properties ERROR_VARIABLE _no_properties LENGTH "${_json}" tests ${_i} properties)
	IF(NOT _no_properties AND _n_properties GREATER 0)
		MATH(EXPR _last_property "${_n_properties} - 1")
		FOREACH(_p RANGE ${_last_property})
			STRING(JSON _property GET "${_json}" tests ${_i} properties ${_p} name)
			IF(_property STREQUAL "TIMEOUT")
				STRING(JSON _timeout GET "${_json}" tests ${_i} properties ${_p} value)
			ENDIF()
		ENDFOREACH()
	ENDIF()
	MATH(EXPR _needed "${_bound} + ${MARGIN}")
	IF(_timeout LESS _needed)
		LIST(APPEND _short_timeout "${_name} (bound ${_bound} s, TIMEOUT ${_timeout} s)")
	ENDIF()
ENDFOREACH()

IF(_with_launcher EQUAL 0)
	MESSAGE(FATAL_ERROR "No entry names the MPI launcher ${MPIEXEC}: the check sees nothing")
ENDIF()
IF(_without_runner)
	LIST(LENGTH _without_runner _bad)
	LIST(JOIN _without_runner "\n  " _names)
	MESSAGE(FATAL_ERROR "${_bad} of ${_with_launcher} entries that name the MPI launcher are not "
		"registered with GENEVA_ADD_MPI_TEST (GenevaMpiTest.cmake), so they share Open MPI's "
		"shared-memory directory with every other job:\n  ${_names}")
ENDIF()
IF(_short_timeout)
	LIST(JOIN _short_timeout "\n  " _names)
	MESSAGE(FATAL_ERROR "Entries whose TIMEOUT leaves the runner less than ${MARGIN} s after its "
		"bound, so that ctest may end them before the directory is removed (set the TIMEOUT "
		"through GENEVA_ADD_MPI_TEST only):\n  ${_names}")
ENDIF()
MESSAGE(STATUS "All ${_with_launcher} entries that name the MPI launcher run under ${RUNNER}")
