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
# Driver for the test GenevaCommitSources: GenevaCommit.cmake on made-up source trees, each in a
# directory of its own under WORK (removed and made again at the start):
#   - a git work tree gives the id of its HEAD, and an untracked file does not change it;
#   - a changed tracked file appends "-dirty";
#   - a tree without git whose GENEVA_COMMIT holds an id gives that id;
#   - a tree without git whose GENEVA_COMMIT holds the placeholder of the repository fails, and its
#     text names the file.
#
# Input (passed via -D):
#   SCRIPT   CMakeModules/GenevaCommit.cmake
#   GIT      the git executable
#   WORK     a scratch directory in the build tree
#
################################################################################

IF(NOT SCRIPT OR NOT GIT OR NOT WORK)
	MESSAGE(FATAL_ERROR "CheckGenevaCommit.cmake needs -DSCRIPT=<GenevaCommit.cmake> -DGIT=<git> -DWORK=<dir>")
ENDIF()

FILE(REMOVE_RECURSE "${WORK}")
SET(_problems "")

# Runs the script on _tree; sets _rc, _err and _id (the id in the written source, or "")
MACRO(_take_commit _tree)
	FILE(REMOVE "${WORK}/out.cpp")
	EXECUTE_PROCESS(
		COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${_tree} -DOUTPUT=${WORK}/out.cpp -P ${SCRIPT}
		RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
	SET(_id "")
	IF(EXISTS "${WORK}/out.cpp")
		FILE(READ "${WORK}/out.cpp" _out)
		IF(_out MATCHES "return \"([^\"]*)\";")
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

# A git work tree
SET(_tree "${WORK}/git")
FILE(MAKE_DIRECTORY "${_tree}")
FILE(WRITE "${_tree}/GENEVA_COMMIT" "$Format:%H$\n")
FILE(WRITE "${_tree}/tracked.txt" "one\n")
_git("${_tree}" init -q)
_git("${_tree}" add GENEVA_COMMIT tracked.txt)
_git("${_tree}" commit -q -m "a commit")
_git("${_tree}" rev-parse HEAD)
SET(_head "${_git_out}")
FILE(WRITE "${_tree}/untracked.txt" "not in git\n")
_take_commit("${_tree}")
IF(NOT _rc EQUAL 0 OR NOT _id STREQUAL _head)
	STRING(APPEND _problems "\n  git work tree: expected ${_head}, got \"${_id}\" (${_rc}): ${_err}")
ENDIF()

# A changed tracked file
FILE(WRITE "${_tree}/tracked.txt" "two\n")
_take_commit("${_tree}")
IF(NOT _rc EQUAL 0 OR NOT _id STREQUAL "${_head}-dirty")
	STRING(APPEND _problems "\n  changed tracked file: expected ${_head}-dirty, got \"${_id}\" (${_rc}): ${_err}")
ENDIF()

# No git, the file holds an id
SET(_tree "${WORK}/filled")
SET(_given "0123456789abcdef0123456789abcdef01234567")
FILE(WRITE "${_tree}/GENEVA_COMMIT" "${_given}\n")
_take_commit("${_tree}")
IF(NOT _rc EQUAL 0 OR NOT _id STREQUAL _given)
	STRING(APPEND _problems "\n  filled GENEVA_COMMIT: expected ${_given}, got \"${_id}\" (${_rc}): ${_err}")
ENDIF()

# Neither
SET(_tree "${WORK}/neither")
FILE(WRITE "${_tree}/GENEVA_COMMIT" "$Format:%H$\n")
_take_commit("${_tree}")
IF(_rc EQUAL 0 OR EXISTS "${WORK}/out.cpp" OR NOT _err MATCHES "GENEVA_COMMIT")
	STRING(APPEND _problems "\n  neither git nor an id: expected a failure that names GENEVA_COMMIT, got (${_rc}): ${_err}")
ENDIF()

IF(_problems)
	MESSAGE(FATAL_ERROR "GenevaCommit.cmake did not take the commit as it should:${_problems}")
ENDIF()
MESSAGE(STATUS "GenevaCommit.cmake took the commit of all four trees as it should")
