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
# Driver for the test DoxygenWarningsOnlyAllowed.
#
# Runs Doxygen over every library, with the configuration the doc-<library> targets use and in
# their order (each library reads the tag files of the libraries below it), and collects every
# warning and error Doxygen reports. The test fails if
#
#   * Doxygen reports a warning or an error that DoxygenWarningsAllowed.txt does not list, or
#   * a warning listed there no longer occurs (the list says exactly what is allowed), or
#   * Doxygen itself fails.
#
# A warning is compared without its line number, as "<path below the source tree>: <message>",
# so that an unrelated edit above it does not change it.
#
# Input (passed via -D):
#   DOXYGEN      the doxygen executable
#   BUILD_DIR    the top-level build directory (holds libs/<library>/doc/Doxyfile)
#   SOURCE_DIR   the top-level source directory
#   ALLOWED      the list of allowed warnings
#   LIBS         the libraries in build order, separated by commas
#
################################################################################

FOREACH(_var DOXYGEN BUILD_DIR SOURCE_DIR ALLOWED LIBS)
	IF(NOT ${_var})
		MESSAGE(FATAL_ERROR "CheckDoxygenWarnings.cmake needs -D${_var}=...")
	ENDIF()
ENDFOREACH()

# Turns a text into a CMake list of its lines. Semicolons and brackets, which a CMake list does not
# carry, are replaced by placeholders and restored by _restore().
FUNCTION(_to_lines _text _out)
	STRING(REPLACE ";" "<SEMICOLON>" _text "${_text}")
	STRING(REPLACE "[" "<LBRACKET>" _text "${_text}")
	STRING(REPLACE "]" "<RBRACKET>" _text "${_text}")
	STRING(REPLACE "\n" ";" _text "${_text}")
	SET(${_out} "${_text}" PARENT_SCOPE)
ENDFUNCTION()

FUNCTION(_restore _text _out)
	STRING(REPLACE "<SEMICOLON>" ";" _text "${_text}")
	STRING(REPLACE "<LBRACKET>" "[" _text "${_text}")
	STRING(REPLACE "<RBRACKET>" "]" _text "${_text}")
	SET(${_out} "${_text}" PARENT_SCOPE)
ENDFUNCTION()

# The allowed warnings: one per line; empty lines and lines starting with # are skipped.
FILE(READ "${ALLOWED}" _allowed_text)
_to_lines("${_allowed_text}" _allowed_lines)
SET(_allowed "")
FOREACH(_line IN LISTS _allowed_lines)
	STRING(STRIP "${_line}" _line)
	IF(_line STREQUAL "" OR _line MATCHES "^#")
		CONTINUE()
	ENDIF()
	LIST(APPEND _allowed "${_line}")
ENDFOREACH()

# Runs Doxygen for each library and collects its reports, normalized.
STRING(REPLACE "," ";" _libs "${LIBS}")
SET(_found "")
SET(_problems "")
FOREACH(_lib IN LISTS _libs)
	SET(_dir "${BUILD_DIR}/libs/${_lib}/doc")
	IF(NOT EXISTS "${_dir}/Doxyfile")
		MESSAGE(FATAL_ERROR "No Doxyfile in ${_dir}: configure the build with Doxygen available.")
	ENDIF()
	SET(_log "${_dir}/doxygen-warnings.log")
	FILE(REMOVE "${_log}")
	FILE(READ "${_dir}/Doxyfile" _cfg)
	# The check writes its own output (beside, not over, that of doc-<library>) and leaves out what
	# reports no documentation problem: the dot graphs and the XML. Formulas are left to MathJax:
	# rendering them as images through LaTeX takes more than two minutes in a fresh build tree,
	# against seconds for everything else; doc-<library> still renders them and reports a formula
	# LaTeX cannot set. The tag file is the one doc-<library> writes, with the same content, so the
	# next library reads it as usual.
	FILE(WRITE "${_dir}/Doxyfile.check" "${_cfg}\n"
		"OUTPUT_DIRECTORY = ${_dir}/check\n"
		"HAVE_DOT = NO\n"
		"GENERATE_XML = NO\n"
		"USE_MATHJAX = YES\n"
		"WARN_LOGFILE = ${_log}\n")
	EXECUTE_PROCESS(
		COMMAND "${DOXYGEN}" Doxyfile.check
		WORKING_DIRECTORY "${_dir}"
		RESULT_VARIABLE _rc
		OUTPUT_VARIABLE _out
		ERROR_VARIABLE _err
	)
	IF(NOT _rc EQUAL 0)
		STRING(APPEND _problems "\n  doxygen failed for ${_lib} (exit code ${_rc}): ${_err}")
	ENDIF()
	SET(_reports "${_err}")
	IF(EXISTS "${_log}")
		FILE(READ "${_log}" _log_text)
		STRING(APPEND _reports "\n${_log_text}")
	ENDIF()
	_to_lines("${_reports}" _report_lines)
	FOREACH(_line IN LISTS _report_lines)
		IF(NOT _line MATCHES "^([^ ]*:[0-9]+: )?(warning|error): ")
			CONTINUE()
		ENDIF()
		STRING(REPLACE "${SOURCE_DIR}/" "" _line "${_line}")
		STRING(REGEX REPLACE "^([^:]*):[0-9]+: " "\\1: " _line "${_line}")
		STRING(STRIP "${_line}" _line)
		LIST(APPEND _found "${_line}")
	ENDFOREACH()
ENDFOREACH()
LIST(REMOVE_DUPLICATES _found)

SET(_new "")
FOREACH(_line IN LISTS _found)
	LIST(FIND _allowed "${_line}" _idx)
	IF(_idx EQUAL -1)
		_restore("${_line}" _shown)
		STRING(APPEND _new "\n  ${_shown}")
	ENDIF()
ENDFOREACH()
SET(_gone "")
FOREACH(_line IN LISTS _allowed)
	LIST(FIND _found "${_line}" _idx)
	IF(_idx EQUAL -1)
		_restore("${_line}" _shown)
		STRING(APPEND _gone "\n  ${_shown}")
	ENDIF()
ENDFOREACH()

IF(_new)
	STRING(APPEND _problems "\n Doxygen reports what DoxygenWarningsAllowed.txt does not allow:${_new}")
ENDIF()
IF(_gone)
	STRING(APPEND _problems
		"\n DoxygenWarningsAllowed.txt allows what Doxygen no longer reports (remove it):${_gone}")
ENDIF()
IF(_problems)
	MESSAGE(FATAL_ERROR "CheckDoxygenWarnings:${_problems}")
ENDIF()
LIST(LENGTH _allowed _n_allowed)
MESSAGE(STATUS "CheckDoxygenWarnings: no warning beyond the ${_n_allowed} allowed ones")
