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
# Driver for the test GenevaProgramStamp: GENEVA_STAMP_PROGRAM (GenevaProgramStamp.cmake) in
# made-up projects, each in a directory of its own under WORK (removed and made again at the start):
#   - a project without git: a program with IDENTITY gets that text (quotes and backslashes kept),
#     one without it no identity, and CMake says that it is not stamped (at every configure);
#   - a project that is a git work tree: the id of its HEAD, and "-dirty" after a change of a
#     tracked file, taken by the build;
#   - a library target is refused.
#
# Input (passed via -D):
#   MODULE        CMakeModules/GenevaProgramStamp.cmake
#   GIT           the git executable
#   CXX_COMPILER  the C++ compiler of the build (the projects are configured, not compiled)
#   WORK          a scratch directory in the build tree
#
################################################################################

IF(NOT MODULE OR NOT GIT OR NOT CXX_COMPILER OR NOT WORK)
	MESSAGE(FATAL_ERROR
		"CheckGenevaProgramStamp.cmake needs -DMODULE=<GenevaProgramStamp.cmake> -DGIT=<git> "
		"-DCXX_COMPILER=<compiler> -DWORK=<dir>")
ENDIF()

FILE(REMOVE_RECURSE "${WORK}")
SET(_problems "")

# Writes a project into _tree whose CMakeLists.txt ends with ARGN (one line each)
FUNCTION(_project _tree)
	FILE(MAKE_DIRECTORY "${_tree}")
	FILE(WRITE "${_tree}/main.cpp" "int main() { return 0; }\n")
	STRING(JOIN "\n" _lines ${ARGN})
	FILE(WRITE "${_tree}/CMakeLists.txt"
		"cmake_minimum_required(VERSION 3.27)\n"
		"project(StampProbe CXX)\n"
		"include(\"${MODULE}\")\n"
		"${_lines}\n")
ENDFUNCTION()

# Configures the project in _tree into _tree-build; sets _rc and _out (output and errors)
MACRO(_configure _tree)
	EXECUTE_PROCESS(
		COMMAND ${CMAKE_COMMAND} -S ${_tree} -B ${_tree}-build -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
		RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
ENDMACRO()

# Sets _id to the identity the stamp of _target in _tree-build sets ("<none>" without a stamp)
MACRO(_stamp_of _tree _target)
	SET(_id "<none>")
	SET(_file "${_tree}-build/${_target}-GenevaProgramStamp.cpp")
	IF(EXISTS "${_file}")
		FILE(READ "${_file}" _content)
		IF(_content MATCHES "setProgramIdentity\\(\"(([^\"\\\\]|\\\\.)*)\"\\)")
			SET(_id "${CMAKE_MATCH_1}")
		ENDIF()
	ENDIF()
ENDMACRO()

# Runs git in _tree; fails the test if git fails
MACRO(_git _tree)
	EXECUTE_PROCESS(
		COMMAND ${GIT} -C ${_tree} -c user.name=check -c user.email=check@localhost
			-c commit.gpgsign=false ${ARGN}
		RESULT_VARIABLE _git_rc OUTPUT_VARIABLE _git_out ERROR_VARIABLE _git_err
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	IF(NOT _git_rc EQUAL 0)
		MESSAGE(FATAL_ERROR "git ${ARGN} failed in ${_tree}: ${_git_err}")
	ENDIF()
ENDMACRO()

# A project without git: a text of its own, and none
SET(_tree "${WORK}/plain")
_project("${_tree}"
	"add_executable(given main.cpp)"
	"GENEVA_STAMP_PROGRAM(given IDENTITY [=[a \"text\" with a \\ of its own]=])"
	"add_executable(unstamped main.cpp)"
	"GENEVA_STAMP_PROGRAM(unstamped)")
_configure("${_tree}")
IF(NOT _rc EQUAL 0)
	STRING(APPEND _problems "\n  project without git: the configure failed: ${_out}")
ELSE()
	_stamp_of("${_tree}" given)
	IF(NOT _id STREQUAL [=[a \"text\" with a \\ of its own]=])
		STRING(APPEND _problems "\n  IDENTITY: expected the text as a C++ literal, got \"${_id}\"")
	ENDIF()
	_stamp_of("${_tree}" unstamped)
	IF(NOT _id STREQUAL "")
		STRING(APPEND _problems "\n  no git: expected an empty identity, got \"${_id}\"")
	ENDIF()
	IF(NOT _out MATCHES "-- unstamped is not stamped: ")
		STRING(APPEND _problems "\n  no git: CMake did not say that the program is not stamped: ${_out}")
	ENDIF()
	# and says so again at the next configure, after a stamped program
	_configure("${_tree}")
	IF(NOT _out MATCHES "-- unstamped is not stamped: ")
		STRING(APPEND _problems "\n  no git, configured again: CMake did not say that the program is not stamped: ${_out}")
	ENDIF()
ENDIF()

# A project that is a git work tree: its HEAD, and "-dirty" once a tracked file has changed
SET(_tree "${WORK}/git")
_project("${_tree}" "add_executable(probe main.cpp)" "GENEVA_STAMP_PROGRAM(probe)")
_git("${_tree}" init -q)
_git("${_tree}" add CMakeLists.txt main.cpp)
_git("${_tree}" commit -q -m "a commit")
_git("${_tree}" rev-parse HEAD)
SET(_head "${_git_out}")
_configure("${_tree}")
_stamp_of("${_tree}" probe)
IF(NOT _rc EQUAL 0 OR NOT _id STREQUAL _head)
	STRING(APPEND _problems "\n  git work tree: expected ${_head}, got \"${_id}\" (${_rc}): ${_out}")
ENDIF()
FILE(APPEND "${_tree}/main.cpp" "// changed\n")
EXECUTE_PROCESS(
	COMMAND ${CMAKE_COMMAND} --build ${_tree}-build --target probe-program-stamp
	RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
_stamp_of("${_tree}" probe)
IF(NOT _rc EQUAL 0 OR NOT _id STREQUAL "${_head}-dirty")
	STRING(APPEND _problems "\n  changed tracked file: expected ${_head}-dirty, got \"${_id}\" (${_rc}): ${_out}")
ENDIF()

# A library is refused
SET(_tree "${WORK}/library")
_project("${_tree}" "add_library(probe STATIC main.cpp)" "GENEVA_STAMP_PROGRAM(probe)")
_configure("${_tree}")
IF(_rc EQUAL 0 OR NOT _out MATCHES "probe is a STATIC_LIBRARY, not an executable")
	STRING(APPEND _problems "\n  library: expected a refusal, got (${_rc}): ${_out}")
ENDIF()

IF(_problems)
	MESSAGE(FATAL_ERROR "GENEVA_STAMP_PROGRAM did not stamp as it should:${_problems}")
ENDIF()
MESSAGE(STATUS "GENEVA_STAMP_PROGRAM stamped the three programs as it should, and refused the library")
