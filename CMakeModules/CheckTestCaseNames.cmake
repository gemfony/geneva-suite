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
# Driver for the test TestCaseNamesSelectable: every Catch2 test case of the tree
# can be selected by its full name.
#
# A Catch2 test spec is not a plain name. A comma separates alternatives, square
# brackets enclose a tag, a double quote begins or ends a quoted name and a
# backslash escapes the next character. A test case whose name contains one of
# them cannot be run by passing its name: "a, b" selects the cases "a" and "b",
# and "within [min,max]" asks for the tag [min,max]. The name then has to be
# escaped by hand, and a CTest entry or a script that names the case runs
# nothing at all, or something else.
#
# The driver reads the C++ sources under SOURCE_DIR (headers too), finds every
# TEST_CASE, TEST_CASE_METHOD, SCENARIO and TEMPLATE_*TEST_CASE, joins the string
# literals of its name, and fails with the list of names that contain one of
# , [ ] " or \ (an escaped quote in the source is a backslash in the literal).
#
# Input (passed via -D):
#   SOURCE_DIR   the directory to search (the libs/ tree)
#
################################################################################

IF(NOT SOURCE_DIR)
	MESSAGE(FATAL_ERROR "CheckTestCaseNames.cmake needs -DSOURCE_DIR=<directory>")
ENDIF()

FILE(GLOB_RECURSE _sources
	"${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.hpp" "${SOURCE_DIR}/*.cu" "${SOURCE_DIR}/*.cuh")

# A string literal, and the name argument of a test-case macro: one or more adjacent literals, after
# the fixture or the template-type argument for the macros that take one first.
SET(_lit "\"([^\"\\\\]|\\\\.)*\"")
STRING(CONCAT _macro
	"(TEST_CASE|TEST_CASE_METHOD|SCENARIO|"
	"TEMPLATE_TEST_CASE|TEMPLATE_LIST_TEST_CASE|TEMPLATE_PRODUCT_TEST_CASE)")
SET(_ws "[ \t\r\n]*")
SET(_call "[^A-Za-z0-9_]${_macro}${_ws}\\(${_ws}([A-Za-z0-9_:]+${_ws},${_ws})?(${_lit}${_ws})+")

# Names are never put into CMake lists: a semicolon in a name splits a list element, and an
# unbalanced "[" swallows every element after it (see libs/geneva/tests/UnitTests/
# CheckShardCompleteness.cmake). The text is walked match by match instead.
SET(_n_cases 0)
SET(_offenders "")
FOREACH(_file IN LISTS _sources)
	FILE(READ "${_file}" _text)
	STRING(FIND "${_text}" "TEST_CASE" _has_case)
	STRING(FIND "${_text}" "SCENARIO" _has_scenario)
	IF(_has_case LESS 0 AND _has_scenario LESS 0)
		CONTINUE()
	ENDIF()
	# A leading newline lets the macro match at the very start of a file as well.
	SET(_rest "\n${_text}")
	WHILE(TRUE)
		STRING(REGEX MATCH "${_call}" _c "${_rest}")
		IF("${_c}" STREQUAL "")
			BREAK()
		ENDIF()
		STRING(FIND "${_rest}" "${_c}" _at)
		STRING(LENGTH "${_c}" _len_c)
		MATH(EXPR _after "${_at} + ${_len_c}")
		STRING(SUBSTRING "${_rest}" ${_after} -1 _rest)

		# Join the contents of the name's literals.
		SET(_name "")
		SET(_lits "${_c}")
		WHILE(TRUE)
			STRING(REGEX MATCH "${_lit}" _l "${_lits}")
			IF("${_l}" STREQUAL "")
				BREAK()
			ENDIF()
			STRING(FIND "${_lits}" "${_l}" _at_l)
			STRING(LENGTH "${_l}" _len_l)
			MATH(EXPR _after_l "${_at_l} + ${_len_l}")
			STRING(SUBSTRING "${_lits}" ${_after_l} -1 _lits)
			MATH(EXPR _len_inner "${_len_l} - 2")
			STRING(SUBSTRING "${_l}" 1 ${_len_inner} _inner)
			STRING(APPEND _name "${_inner}")
		ENDWHILE()

		MATH(EXPR _n_cases "${_n_cases} + 1")
		IF("${_name}" MATCHES "[],[\"\\\\]")
			FILE(RELATIVE_PATH _rel "${SOURCE_DIR}" "${_file}")
			STRING(APPEND _offenders "  ${_rel}: ${_name}\n")
		ENDIF()
	ENDWHILE()
ENDFOREACH()

IF(_n_cases EQUAL 0)
	MESSAGE(FATAL_ERROR "No test case found under ${SOURCE_DIR}: the search is broken.")
ENDIF()
IF(NOT _offenders STREQUAL "")
	MESSAGE(FATAL_ERROR
		"Of ${_n_cases} test cases, these cannot be selected by their full name: they contain\n"
		"one of the characters , [ ] \" \\ which a Catch2 test spec reads as a separator, a\n"
		"tag, a quote or an escape. Rename them:\n${_offenders}")
ENDIF()
MESSAGE(STATUS "${_n_cases} test cases, every one selectable by its full name")
