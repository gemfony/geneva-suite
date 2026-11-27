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
# Driver for the test FormatClean: every C++ and CUDA file under libs/ is laid out as the
# repository's .clang-format says. The formatter runs in --dry-run -Werror mode, so it changes
# nothing; the test fails with the list of the files it would change. Regions between
# "// clang-format off" and "// clang-format on" are left out by the formatter itself.
#
# Input (passed via -D):
#   CLANG_FORMAT   the formatter (its major version is checked at configure time)
#   SOURCE_DIR     the libs/ directory
#
################################################################################

IF(NOT CLANG_FORMAT OR NOT SOURCE_DIR)
	MESSAGE(FATAL_ERROR "CheckFormatClean.cmake needs -DCLANG_FORMAT=<formatter> -DSOURCE_DIR=<the libs directory>")
ENDIF()
IF(NOT IS_DIRECTORY "${SOURCE_DIR}")
	MESSAGE(FATAL_ERROR "No directory ${SOURCE_DIR}: the search is broken.")
ENDIF()

FILE(GLOB_RECURSE _files
	"${SOURCE_DIR}/*.hpp" "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h"
	"${SOURCE_DIR}/*.cu" "${SOURCE_DIR}/*.cuh")
LIST(SORT _files)
LIST(LENGTH _files _n_files)
IF(_n_files EQUAL 0)
	MESSAGE(FATAL_ERROR "No source files under ${SOURCE_DIR}: the search is broken.")
ENDIF()

EXECUTE_PROCESS(
	COMMAND "${CLANG_FORMAT}" --dry-run -Werror --style=file ${_files}
	RESULT_VARIABLE _status
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
)

IF(NOT _status EQUAL 0)
	# Each deviation is reported as "<file>:<line>:<column>: error: ..."; one line per file is enough
	STRING(REGEX MATCHALL "[^\n]+:[0-9]+:[0-9]+: error:" _hits "${_err}")
	SET(_deviating "")
	FOREACH(_hit IN LISTS _hits)
		STRING(REGEX REPLACE ":[0-9]+:[0-9]+: error:$" "" _file "${_hit}")
		FILE(RELATIVE_PATH _file "${SOURCE_DIR}" "${_file}")
		LIST(APPEND _deviating "${_file}")
	ENDFOREACH()
	LIST(REMOVE_DUPLICATES _deviating)
	LIST(LENGTH _deviating _n_deviating)
	IF(_n_deviating EQUAL 0)
		MESSAGE(FATAL_ERROR "${CLANG_FORMAT} failed (status ${_status}):\n${_err}")
	ENDIF()
	LIST(JOIN _deviating "\n  " _list)
	MESSAGE(FATAL_ERROR "${_n_deviating} of ${_n_files} files deviate from .clang-format "
		"(fix with: clang-format -i <file>):\n  ${_list}")
ENDIF()
MESSAGE(STATUS "${_n_files} files under libs/: all formatted as .clang-format says")
