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
# Driver for the shard-completeness test.
#
# A Catch2 test binary that is registered with CTest as several test-spec subsets
# (shards) is only equivalent to the single whole-binary entry it replaced if those
# specs PARTITION its test cases: every case selected by exactly one shard. Nothing
# in Catch2 or CTest enforces that -- a newly written TEST_CASE whose tags fall
# outside every spec would simply stop being run, silently, which is precisely the
# hidden red that Invariant 9 exists to prevent.
#
# This driver asserts the partition instead of assuming it. It asks the binary for
# its full test-case list, asks it again once per shard spec, and compares:
#
#   * no case is unmatched     (a case in the full list that no shard selects)
#   * no case is claimed twice (the same case selected by two shards)
#
# The comparison runs against the binary that was actually built, so it tracks the
# source: adding a TEST_CASE re-runs it, and only a case that really escapes every
# shard fails it. Listing is all it does -- no test case is executed.
#
# Names are the keys, so the check has a second, useful effect: Catch2 tolerates two
# TEST_CASEs sharing a name as long as their tags differ, and this driver does not --
# it reports the second one as claimed twice. That is the right verdict. A duplicated
# name cannot be selected individually, and it makes every name-based report and tool
# ambiguous. Writing this guard found one such pair already.
#
# Inputs (passed via -D):
#   TEST_EXE   the Catch2 test executable
#   SPECS      a CMake list of Catch2 test specs, one per shard
#
# Implementation note, learned the hard way: test-case names must NOT be put into
# CMake lists. Real names in this suite contain semicolons ("clone copies it;
# compare ignores it") and unbalanced brackets ("stays in [-0.5, 0.5)"), and both
# corrupt CMake's list parsing -- an unbalanced "[" silently swallows every element
# after it, which reads as "these cases are in no shard" and would have made this
# very guard produce a confident, wrong answer. Names are therefore carried in a
# newline-delimited string and compared with STRING(FIND). Catch2 writes one <Name>
# element per line, so a newline cannot occur inside a name.
#
################################################################################

# Ask the binary for a test-case list as XML and return the names in <out_text> as a
# newline-delimited, newline-terminated string (so "\n<name>\n" is an exact-match
# probe), and their count in <out_count>. Any further arguments are passed to the
# binary ahead of the listing options -- that is where a shard's test spec goes.
#
# The XML reporter is used rather than the console listing because the latter wraps
# long names over several lines, and because it escapes any markup character.
FUNCTION(gpd_list_cases out_text out_count)
	SET(_cmd "${TEST_EXE}" ${ARGN} --list-tests --reporter xml)
	EXECUTE_PROCESS(
		COMMAND ${_cmd}
		OUTPUT_VARIABLE _xml
		ERROR_VARIABLE _err
		RESULT_VARIABLE _rc
	)
	IF(NOT _rc EQUAL 0)
		MESSAGE(FATAL_ERROR "listing test cases failed (rc=${_rc}) for: ${_cmd}\n${_err}")
	ENDIF()

	SET(_names "\n")
	SET(_count 0)
	SET(_rest "${_xml}")
	WHILE(TRUE)
		STRING(FIND "${_rest}" "<Name>" _open)
		IF(_open LESS 0)
			BREAK()
		ENDIF()
		MATH(EXPR _open "${_open} + 6")
		STRING(SUBSTRING "${_rest}" ${_open} -1 _rest)
		STRING(FIND "${_rest}" "</Name>" _close)
		IF(_close LESS 0)
			MESSAGE(FATAL_ERROR "malformed listing output: an unterminated <Name> element")
		ENDIF()
		STRING(SUBSTRING "${_rest}" 0 ${_close} _name)
		STRING(APPEND _names "${_name}\n")
		MATH(EXPR _count "${_count} + 1")
		MATH(EXPR _close "${_close} + 7")
		STRING(SUBSTRING "${_rest}" ${_close} -1 _rest)
	ENDWHILE()

	SET(${out_text} "${_names}" PARENT_SCOPE)
	SET(${out_count} ${_count} PARENT_SCOPE)
ENDFUNCTION()

IF(NOT TEST_EXE OR NOT SPECS)
	MESSAGE(FATAL_ERROR "CheckShardCompleteness.cmake needs -DTEST_EXE=<binary> and -DSPECS=<spec list>")
ENDIF()

gpd_list_cases(_all_names _n_all)
IF(_n_all EQUAL 0)
	MESSAGE(FATAL_ERROR "${TEST_EXE} reports no test cases at all -- the listing mechanism is broken.")
ENDIF()

# Collect what the shards select, naming the second claimant when a case is claimed twice.
SET(_claimed "\n")
SET(_dupes "")
SET(_n_claimed 0)
FOREACH(_spec IN LISTS SPECS)
	gpd_list_cases(_shard_names _n_shard "${_spec}")
	IF(_n_shard EQUAL 0)
		MESSAGE(FATAL_ERROR
			"shard spec '${_spec}' selects no test case at all.\n"
			"Either the case it was written for is gone, or its tag was renamed.")
	ENDIF()
	MESSAGE(STATUS "shard '${_spec}': ${_n_shard} case(s)")

	SET(_rest "${_shard_names}")
	STRING(SUBSTRING "${_rest}" 1 -1 _rest) # drop the leading delimiter
	WHILE(NOT "${_rest}" STREQUAL "")
		STRING(FIND "${_rest}" "\n" _eol)
		STRING(SUBSTRING "${_rest}" 0 ${_eol} _case)
		MATH(EXPR _eol "${_eol} + 1")
		STRING(SUBSTRING "${_rest}" ${_eol} -1 _rest)

		STRING(FIND "${_claimed}" "\n${_case}\n" _seen)
		IF(_seen GREATER_EQUAL 0)
			STRING(APPEND _dupes "    ${_case}  (claimed again by '${_spec}')\n")
		ELSE()
			STRING(APPEND _claimed "${_case}\n")
			MATH(EXPR _n_claimed "${_n_claimed} + 1")
		ENDIF()
	ENDWHILE()
ENDFOREACH()

# Unmatched: in the full list, claimed by nobody.
SET(_unmatched "")
SET(_rest "${_all_names}")
STRING(SUBSTRING "${_rest}" 1 -1 _rest)
WHILE(NOT "${_rest}" STREQUAL "")
	STRING(FIND "${_rest}" "\n" _eol)
	STRING(SUBSTRING "${_rest}" 0 ${_eol} _case)
	MATH(EXPR _eol "${_eol} + 1")
	STRING(SUBSTRING "${_rest}" ${_eol} -1 _rest)

	STRING(FIND "${_claimed}" "\n${_case}\n" _seen)
	IF(_seen LESS 0)
		STRING(APPEND _unmatched "    ${_case}\n")
	ENDIF()
ENDWHILE()

IF(NOT "${_unmatched}" STREQUAL "")
	MESSAGE(FATAL_ERROR
		"these test cases are selected by NO shard and would never run:\n${_unmatched}"
		"Give the case a tag one of the shards selects, or widen the catch-all shard's "
		"spec (see the shard table in this directory's CMakeLists.txt).")
ENDIF()

IF(NOT "${_dupes}" STREQUAL "")
	MESSAGE(FATAL_ERROR
		"these test cases are selected by MORE THAN ONE shard and would run twice:\n${_dupes}"
		"The shard specs must partition the suite, not merely cover it.")
ENDIF()

MESSAGE(STATUS
	"shard completeness: the shard specs partition all ${_n_all} test case(s) of ${TEST_EXE} "
	"(${_n_claimed} selected) -- none unmatched, none claimed twice.")
