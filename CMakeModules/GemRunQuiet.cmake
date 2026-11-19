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
# Script mode (cmake -P) helper: runs a command with its stdout and stderr
# captured to a log file, so a chatty-but-successful run does not clobber the
# build output. If the command fails, the captured output is replayed in full
# and the build step fails. Used by GEM_MATERIALIZE_CONFIGS to run the
# freshly built config-owning binaries (whose GLogger notes and warnings are
# runtime output, intentionally unchanged at the code level).
#
# Expected -D definitions:
#   RQ_CMD  -- the command and its arguments, joined with '|' (an argument may
#              contain spaces but not '|'); pass executables as absolute paths
#              or $<TARGET_FILE:...> expansions, not bare target names
#   RQ_WD   -- the working directory for the command
#   RQ_LOG  -- absolute path of the log file receiving stdout+stderr
#   RQ_DESC -- one-line description of the step, used in the failure message
#
################################################################################

FOREACH(_rq_var RQ_CMD RQ_WD RQ_LOG RQ_DESC)
	IF(NOT DEFINED ${_rq_var})
		MESSAGE(FATAL_ERROR "GemRunQuiet.cmake: required definition ${_rq_var} is missing")
	ENDIF()
ENDFOREACH()

STRING(REPLACE "|" ";" _rq_cmd_list "${RQ_CMD}")

# Naming the same file for OUTPUT_FILE and ERROR_FILE merges the two streams.
EXECUTE_PROCESS(
	COMMAND ${_rq_cmd_list}
	WORKING_DIRECTORY "${RQ_WD}"
	OUTPUT_FILE "${RQ_LOG}"
	ERROR_FILE "${RQ_LOG}"
	RESULT_VARIABLE _rq_result
)

IF(NOT _rq_result EQUAL 0)
	SET(_rq_output "")
	IF(EXISTS "${RQ_LOG}")
		FILE(READ "${RQ_LOG}" _rq_output)
	ENDIF()
	MESSAGE(FATAL_ERROR
		"${RQ_DESC} failed (exit status: ${_rq_result}).\n"
		"Captured output (also in ${RQ_LOG}):\n"
		"${_rq_output}")
ENDIF()
