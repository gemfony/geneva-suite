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
# Driver for the test LibraryLayersRespected.
#
# The libraries of the collection are layered: weft at the bottom, common on weft, hap, dietrich and
# courtier each on common only (three peers that know nothing of each other), geneva on all of them.
# A file that includes a header of a library outside its layer makes its library, or its program,
# depend on that library. Every library's include directory is on the global include path of the
# build, and the shared libraries are linked without -z defs, so such an include shows neither when
# the file is compiled nor when its library is linked. This driver reads the #include lines of
# every C++ file under libs/ and fails, naming file and line, if one names a library its file may
# not use:
#
#   library    its code (include/, src/) may      its programs (tests/, examples/, benchmarks/,
#              include                            docprograms/, quickstart/, tools/) may include
#   weft       weft                               weft
#   common     common, weft                       common, weft
#   hap        hap, common, weft                  hap, common, weft, dietrich
#   dietrich   dietrich, common, weft             dietrich, common, weft
#   courtier   courtier, common, weft             courtier, common, weft
#   geneva     everything                         everything
#
# (hap's programs may include dietrich: two of its test programs plot what they draw; dietrich is a
# peer, used there by test code only.) A C++ file in any other directory of a library is reported
# too, so that a new directory is placed in one of the two groups rather than escaping the check.
#
# Input (passed via -D):
#   SOURCE_DIR   the libs/ directory
#
################################################################################

IF(NOT SOURCE_DIR)
	MESSAGE(FATAL_ERROR "CheckLibraryLayers.cmake needs -DSOURCE_DIR=<the libs directory>")
ENDIF()

SET(_libraries weft common hap dietrich courtier geneva)

# What each library's code and programs may include ("*": every library)
SET(_code_weft weft)
SET(_code_common common weft)
SET(_code_hap hap common weft)
SET(_code_dietrich dietrich common weft)
SET(_code_courtier courtier common weft)
SET(_code_geneva "*")
SET(_programs_weft weft)
SET(_programs_common common weft)
SET(_programs_hap hap common weft dietrich)
SET(_programs_dietrich dietrich common weft)
SET(_programs_courtier courtier common weft)
SET(_programs_geneva "*")

SET(_code_dirs include src)
SET(_program_dirs tests examples benchmarks docprograms quickstart tools)

SET(_problems "")
SET(_n_files 0)
FOREACH(_lib IN LISTS _libraries)
	IF(NOT IS_DIRECTORY "${SOURCE_DIR}/${_lib}")
		MESSAGE(FATAL_ERROR "No directory ${SOURCE_DIR}/${_lib}: the search is broken.")
	ENDIF()
	FILE(GLOB_RECURSE _files RELATIVE "${SOURCE_DIR}/${_lib}"
		"${SOURCE_DIR}/${_lib}/*.hpp" "${SOURCE_DIR}/${_lib}/*.cpp" "${SOURCE_DIR}/${_lib}/*.h"
		"${SOURCE_DIR}/${_lib}/*.cu" "${SOURCE_DIR}/${_lib}/*.cuh")
	FOREACH(_file IN LISTS _files)
		MATH(EXPR _n_files "${_n_files} + 1")
		STRING(REGEX REPLACE "/.*$" "" _top "${_file}")
		IF(_top IN_LIST _code_dirs)
			SET(_allowed ${_code_${_lib}})
			SET(_group "code")
		ELSEIF(_top IN_LIST _program_dirs)
			SET(_allowed ${_programs_${_lib}})
			SET(_group "programs")
		ELSE()
			STRING(APPEND _problems "  ${_lib}/${_file}: in a directory that is neither code "
				"(${_code_dirs}) nor programs (${_program_dirs})\n")
			CONTINUE()
		ENDIF()
		IF("*" IN_LIST _allowed)
			CONTINUE()
		ENDIF()

		# The #include lines that name a directory; a leading newline lets the first line match too
		FILE(READ "${SOURCE_DIR}/${_lib}/${_file}" _text)
		STRING(PREPEND _text "\n")
		STRING(REGEX MATCHALL "\n[ \t]*#[ \t]*include[ \t]*[<\"][A-Za-z0-9_]+/" _includes "${_text}")
		# The matches come in the order of the file: each is looked for after the one before, so that
		# two equal include lines get their own line numbers. _nl counts the newlines before _offset;
		# a match starts with the newline that ends the line before it, so its line is the count of
		# the newlines before it plus one (the prepended newline makes up for the first line).
		SET(_offset 0)
		SET(_nl 0)
		FOREACH(_include IN LISTS _includes)
			STRING(SUBSTRING "${_text}" ${_offset} -1 _rest)
			STRING(FIND "${_rest}" "${_include}" _pos)
			STRING(SUBSTRING "${_rest}" 0 ${_pos} _before)
			STRING(REGEX MATCHALL "\n" _newlines "${_before}")
			LIST(LENGTH _newlines _n)
			MATH(EXPR _line "${_nl} + ${_n} + 1")
			MATH(EXPR _offset "${_offset} + ${_pos} + 1")
			MATH(EXPR _nl "${_nl} + ${_n} + 1")

			STRING(REGEX REPLACE "^.*[<\"]([A-Za-z0-9_]+)/$" "\\1" _used "${_include}")
			IF(NOT _used IN_LIST _libraries OR _used IN_LIST _allowed)
				CONTINUE()
			ENDIF()
			STRING(REPLACE ";" ", " _allowed_text "${_allowed}")
			STRING(APPEND _problems "  ${_lib}/${_file}:${_line}: includes ${_used}/ -- the ${_group} "
				"of ${_lib} may include ${_allowed_text}\n")
		ENDFOREACH()
	ENDFOREACH()
ENDFOREACH()

IF(NOT _problems STREQUAL "")
	MESSAGE(FATAL_ERROR "A library or program includes a library outside its layer:\n${_problems}")
ENDIF()
STRING(REPLACE ";" ", " _libraries_text "${_libraries}")
MESSAGE(STATUS "${_n_files} files of ${_libraries_text}: every include within its library's layer")
