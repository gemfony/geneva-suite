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
# Driver of the target "coverage" (GEM_BUILD_WITH_COVERAGE, top-level CMakeLists.txt): runs the
# test suite without the XL tests in an instrumented build, merges the raw profiles and reports the
# line coverage of each library below libs/ (its include/ and src/ trees). Writes below
# <build>/coverage/:
#
#   summary.txt           one line per library (regions, functions, lines, branches)
#   report-<library>.txt  llvm-cov report of the library, per file
#   html/                 llvm-cov show, the annotated sources
#
# There are no thresholds. The target fails when a test failed (after reporting) or a tool fails;
# llvm-cov's warnings (e.g. about a binary not rebuilt since a source changed) go to the terminal.
#
# Input (passed via -D):
#   SOURCE_DIR     the top of the source tree
#   BINARY_DIR     the top of the build tree
#   OBJECTS_FILE   the programs and libraries of the build, one per line (written at configure time)
#   CTEST          ctest
#   LLVM_PROFDATA  llvm-profdata
#   LLVM_COV       llvm-cov
#
################################################################################

# The libraries: the directories below libs/ that have a src/.
FILE(GLOB _libraries LIST_DIRECTORIES true RELATIVE "${SOURCE_DIR}/libs" "${SOURCE_DIR}/libs/*/src")
LIST(TRANSFORM _libraries REPLACE "/src$" "")

SET(_cov "${BINARY_DIR}/coverage")
FILE(REMOVE_RECURSE "${_cov}")
FILE(MAKE_DIRECTORY "${_cov}/profiles")

# The tests. %p keeps the profiles of concurrent processes apart, %m those of different binaries.
SET(ENV{LLVM_PROFILE_FILE} "${_cov}/profiles/%p-%m.profraw")
EXECUTE_PROCESS(COMMAND ${CTEST} -LE XL --output-on-failure
	WORKING_DIRECTORY "${BINARY_DIR}" RESULT_VARIABLE _ctest_rc)
UNSET(ENV{LLVM_PROFILE_FILE})

FILE(GLOB _profiles "${_cov}/profiles/*.profraw")
LIST(JOIN _profiles "\n" _profile_lines)
FILE(WRITE "${_cov}/profiles.txt" "${_profile_lines}\n")
EXECUTE_PROCESS(COMMAND ${LLVM_PROFDATA} merge -sparse --input-files=${_cov}/profiles.txt
	-o ${_cov}/merged.profdata COMMAND_ERROR_IS_FATAL ANY)

# The binaries the coverage mappings are read from (all must be built): the first is named plainly,
# the others with -object.
FILE(STRINGS "${OBJECTS_FILE}" _objects)
LIST(JOIN _objects ";-object;" _objects)

# Per library: the table of its files, and its last line (TOTAL) as the line of the summary. The
# first column of a table is as wide as its longest file name; the summary keeps the numeric columns,
# which have fixed widths, from the header "Regions" on (up to seven digits).
SET(_summary "")
SET(_all_paths "")
FOREACH(_lib IN LISTS _libraries)
	SET(_paths "${SOURCE_DIR}/libs/${_lib}/include" "${SOURCE_DIR}/libs/${_lib}/src")
	LIST(APPEND _all_paths ${_paths})
	EXECUTE_PROCESS(COMMAND ${LLVM_COV} report -instr-profile=${_cov}/merged.profdata ${_objects} ${_paths}
		OUTPUT_VARIABLE _report COMMAND_ERROR_IS_FATAL ANY)
	FILE(WRITE "${_cov}/report-${_lib}.txt" "${_report}")
	STRING(REGEX MATCH "^Filename[^\n]*" _header "${_report}")
	STRING(REGEX MATCH "\nTOTAL[^\n]*" _total "${_report}")
	STRING(SUBSTRING "${_total}" 1 -1 _total)
	STRING(FIND "${_header}" "Regions" _p)
	STRING(SUBSTRING "${_header}" ${_p} -1 _header)
	STRING(SUBSTRING "${_total}" ${_p} -1 _total)
	STRING(SUBSTRING "${_lib}          " 0 10 _name)
	STRING(APPEND _summary "${_name}${_total}\n")
ENDFOREACH()
SET(_summary "Library   ${_header}\n${_summary}")
FILE(WRITE "${_cov}/summary.txt" "${_summary}")

# The annotated sources of all libraries.
EXECUTE_PROCESS(COMMAND ${LLVM_COV} show -format=html -output-dir=${_cov}/html
	-instr-profile=${_cov}/merged.profdata ${_objects} ${_all_paths} COMMAND_ERROR_IS_FATAL ANY)

MESSAGE("\nCoverage per library (tests without XL):\n\n${_summary}\n"
	"Per file: ${_cov}/report-<library>.txt, annotated sources: ${_cov}/html/index.html")
IF(NOT _ctest_rc EQUAL 0)
	MESSAGE(FATAL_ERROR "Some tests failed (ctest returned ${_ctest_rc}); the figures above "
		"include their runs up to the failure.")
ENDIF()
