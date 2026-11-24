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
# Driver of the test GemMpiTestDirectories: GemRunMpiTest.sh (the runner of every entry
# registered with GEM_ADD_MPI_TEST) gives each run a directory of its own for Open MPI's
# files and removes it, whichever way the run ends.
#
#  1. Two runs at the same time (execute_process starts the commands of a pipeline together);
#     each writes the directory it was given to a file, if that exists and is writable, and waits
#     two seconds, so that the runs overlap. The two directories must differ, lie below /dev/shm
#     where that is writable, and be gone afterwards.
#  2. A run whose command fails: its exit code is passed on, the directory is gone.
#  3. A run whose command outlasts the bound: it ends at the bound with 124, the directory is gone.
#  4. A run sent SIGINT, and one sent SIGTERM, while its command runs (a Ctrl-C at a terminal, the
#     end of a ctest): the run ends within seconds with 130 or 143, its command is gone, and so is
#     the directory.
#  5. Where there is an MPI launcher: a job that was killed has left its session directory
#     (ompi.<pid> with pmix.<host>.tool, the script says why) in the launcher's default place,
#     here TMPDIR, and a later launcher gets its pid. It must start all the same. The command
#     leaves that directory for its own pid and then becomes the launcher (exec keeps the pid).
#
# Cases 3 and 4 run twice where coreutils' timeout exists: once with it, once with a PATH that
# holds only the few programs the runner and the commands need, so that the runner bounds the
# command without it. Where there is no timeout, they run once, without it.
#
# Inputs (via -D): RUNNER (the script), WORK (a directory for the files of the runs), MPIEXEC (the
# MPI launcher, if there is one).
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

# 3. A run that outlasts its bound; <how> names the variant in the files and messages
FUNCTION(cmtd_bounded _how)
	STRING(TIMESTAMP _start "%s")
	EXECUTE_PROCESS(
		COMMAND "${RUNNER}" 2 sh -c "${_note}; sleep 60" sh "${WORK}/bounded-${_how}"
		RESULT_VARIABLE _rc
		TIMEOUT 40
	)
	STRING(TIMESTAMP _end "%s")
	MATH(EXPR _took "${_end} - ${_start}")
	IF(NOT _rc EQUAL 124)
		MESSAGE(FATAL_ERROR "A run ${_how} that outlasts its bound of 2 s ended with ${_rc} after "
			"${_took} s, not with 124")
	ENDIF()
	cmtd_noted("${WORK}/bounded-${_how}" _bounded)
	MESSAGE(STATUS
		"A run ${_how} that outlasts its bound: ended after ${_took} s, ${_bounded} removed")
ENDFUNCTION()

# 4. A run sent <signal> (INT or TERM) while its command runs; it must end with <code>. The first
#    command of the pipeline notes the runner's pid and becomes the runner (exec), so that the
#    signal reaches it as it would from a terminal or ctest; the second sends the signal once the
#    command runs. The command notes its own pid and becomes a sleep of 60 s (exec).
FUNCTION(cmtd_interrupted _how _signal _code)
	SET(_base "${WORK}/interrupted-${_how}-${_signal}")
	STRING(TIMESTAMP _start "%s")
	EXECUTE_PROCESS(
		COMMAND sh -c [=[echo $$ > "$1"; shift; exec "$@"]=] sh "${_base}.runner"
			"${RUNNER}" 60 sh -c "${_note}; echo \$\$ > \"\$2\"; exec sleep 60" sh "${_base}"
			"${_base}.command"
		COMMAND sh -c [=[i=0
while [ ! -s "$1" ] && [ $i -lt 200 ]; do sleep 0.1; i=$((i + 1)); done
kill -$3 "$(cat "$2")"]=] sh "${_base}.command" "${_base}.runner" ${_signal}
		RESULTS_VARIABLE _rcs
		TIMEOUT 40
	)
	STRING(TIMESTAMP _end "%s")
	MATH(EXPR _took "${_end} - ${_start}")
	IF(NOT _rcs STREQUAL "${_code};0")
		MESSAGE(FATAL_ERROR "A run ${_how} sent SIG${_signal} ended with ${_rcs} after ${_took} s, "
			"not with ${_code} (the runner) and 0 (the sender) within seconds")
	ENDIF()
	IF(_took GREATER 20)
		MESSAGE(FATAL_ERROR "A run ${_how} sent SIG${_signal} took ${_took} s to end")
	ENDIF()
	FILE(STRINGS "${_base}.command" _command LIMIT_COUNT 1)
	EXECUTE_PROCESS(COMMAND sh -c [=[kill -0 "$1" 2> /dev/null]=] sh "${_command}"
		RESULT_VARIABLE _alive)
	IF(_alive EQUAL 0)
		EXECUTE_PROCESS(COMMAND sh -c [=[kill -KILL "$1"]=] sh "${_command}")
		MESSAGE(FATAL_ERROR "The command (pid ${_command}) of a run ${_how} sent SIG${_signal} "
			"is still running after the run ended")
	ENDIF()
	cmtd_noted("${_base}" _interrupted)
	MESSAGE(STATUS "A run ${_how} sent SIG${_signal}: ended with ${_code} after ${_took} s, its "
		"command is gone, ${_interrupted} removed")
ENDFUNCTION()

# 5. A launcher whose pid a killed job had
IF(MPIEXEC)
	FILE(MAKE_DIRECTORY "${WORK}/stale")
	EXECUTE_PROCESS(
		COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${WORK}/stale" "${RUNNER}" 60
			sh -c [=[h=$(uname -n)
mkdir "$TMPDIR/ompi.$$" && : > "$TMPDIR/ompi.$$/pmix.${h%%.*}.tool" && exec "$1" -n 1 true]=]
			sh "${MPIEXEC}"
		RESULT_VARIABLE _rc
		OUTPUT_VARIABLE _out
		ERROR_VARIABLE _out
		TIMEOUT 40
	)
	IF(NOT _rc EQUAL 0)
		MESSAGE(FATAL_ERROR "A launcher whose pid a killed job had ended with ${_rc}:\n${_out}")
	ENDIF()
	MESSAGE(STATUS "A launcher whose pid a killed job had: started and ended with 0")
ENDIF()

FIND_PROGRAM(_timeout_program timeout)
IF(_timeout_program)
	cmtd_bounded(with-timeout)
	cmtd_interrupted(with-timeout INT 130)
	cmtd_interrupted(with-timeout TERM 143)
	# The same without timeout: a PATH that holds only what the runner and the commands call
	FILE(MAKE_DIRECTORY "${WORK}/bin")
	FOREACH(_tool bash sh mktemp rm sleep date cat)
		FIND_PROGRAM(_tool_path_${_tool} ${_tool})
		IF(NOT _tool_path_${_tool})
			MESSAGE(FATAL_ERROR "No ${_tool} for the run without timeout")
		ENDIF()
		FILE(CREATE_LINK "${_tool_path_${_tool}}" "${WORK}/bin/${_tool}" SYMBOLIC)
	ENDFOREACH()
	SET(ENV{PATH} "${WORK}/bin")
ENDIF()
cmtd_bounded(without-timeout)
cmtd_interrupted(without-timeout INT 130)
cmtd_interrupted(without-timeout TERM 143)
FILE(REMOVE_RECURSE "${WORK}")
