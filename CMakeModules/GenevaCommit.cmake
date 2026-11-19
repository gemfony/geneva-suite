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
# The commit of the Geneva sources a build is made from. A server greets each client with it (the
# courtier HELLO), and a client of another commit ends; so a Geneva build always knows its commit,
# from one of three places, tried in this order:
#   1. git: the source directory is the top of a git work tree and git is installed. The id is
#      "git rev-parse HEAD", with "-dirty" appended when a tracked file differs from HEAD (untracked
#      files do not count).
#   2. the file GENEVA_COMMIT at the top of the sources, if it holds a commit id (40 hexadecimal
#      digits, optionally followed by "-dirty"). In the repository it holds a placeholder that
#      "git archive" replaces with the id (.gitattributes: export-subst); a build that copies the
#      sources without .git (the container build) writes the id into it.
#   3. neither: the script fails, and its text names the three ways to give a build its id.
#
# The script writes a C++ source that defines Gem::Common::genevaCommit() (common/GGenevaCommit.hpp)
# and is compiled into the common library. It writes the file only if its content changes, so that
# an unchanged id rebuilds nothing. The top-level CMakeLists.txt runs it at configure time (a tree
# without an id does not configure), and libs/common/src/CMakeLists.txt at every build (a commit
# made after the configure is seen).
#
# Script mode:
#   cmake -DSOURCE_DIR=<the Geneva sources> -DOUTPUT=<the C++ source to write> -P GenevaCommit.cmake
#
# The program mode (-DMODE=program) takes the identity of a user's program for GENEVA_STAMP_PROGRAM
# (GenevaProgramStamp.cmake), by rule 1 with one difference: the program's sources may lie at any
# depth inside the git work tree, whose HEAD is taken (and whose tracked files make it "-dirty"); a
# user's project may well be a subdirectory of its repository. The file GENEVA_COMMIT belongs to the
# Geneva sources and is not read. Without git the identity is empty
# instead of a failure (the program is then not checked). -DIDENTITY=<text> gives the identity
# instead of git. The source it writes holds one static object that calls
# Gem::Common::setProgramIdentity() before main(), again only if its content changes. It says the
# identity as a STATUS line unless -DQUIET=ON.
#
################################################################################

IF(NOT SOURCE_DIR OR NOT OUTPUT)
	MESSAGE(FATAL_ERROR "GenevaCommit.cmake needs -DSOURCE_DIR=<the sources> -DOUTPUT=<file>")
ENDIF()
IF(NOT MODE)
	SET(MODE geneva)
ENDIF()
IF(NOT MODE MATCHES "^(geneva|program)$")
	MESSAGE(FATAL_ERROR "GenevaCommit.cmake: MODE is geneva or program, not ${MODE}")
ENDIF()
FILE(REAL_PATH "${SOURCE_DIR}" _source_dir)

SET(_id "")
IF(MODE STREQUAL "program" AND NOT "${IDENTITY}" STREQUAL "")
	SET(_id "${IDENTITY}")
ENDIF()

# 1. git: for the Geneva sources only if the source directory is the top of the work tree -- sources
#    unpacked into another project's repository must not take that repository's commit; for a
#    user's program if the source directory is inside a work tree at all. No optional locks: a build
#    must not get in the way of a git command the user runs at the same time.
FIND_PROGRAM(_git git)
IF(_git AND _id STREQUAL "")
	EXECUTE_PROCESS(
		COMMAND ${_git} -C ${_source_dir} rev-parse --show-toplevel
		RESULT_VARIABLE _rc OUTPUT_VARIABLE _top ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
	IF(_rc EQUAL 0 AND _top)
		FILE(REAL_PATH "${_top}" _top)
	ENDIF()
	IF(_rc EQUAL 0 AND _top AND (MODE STREQUAL "program" OR _top STREQUAL _source_dir))
		EXECUTE_PROCESS(
			COMMAND ${_git} -C ${_source_dir} rev-parse --verify HEAD
			RESULT_VARIABLE _rc OUTPUT_VARIABLE _head ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
		IF(_rc EQUAL 0)
			EXECUTE_PROCESS(
				COMMAND ${_git} --no-optional-locks -C ${_source_dir}
					status --porcelain --untracked-files=no
				RESULT_VARIABLE _rc OUTPUT_VARIABLE _changes ERROR_QUIET)
			IF(_rc EQUAL 0)
				SET(_id "${_head}")
				IF(NOT _changes STREQUAL "")
					STRING(APPEND _id "-dirty")
				ENDIF()
			ENDIF()
		ENDIF()
	ENDIF()
ENDIF()

# 2. the file GENEVA_COMMIT (of the Geneva sources only)
IF(_id STREQUAL "" AND MODE STREQUAL "geneva" AND EXISTS "${_source_dir}/GENEVA_COMMIT")
	FILE(READ "${_source_dir}/GENEVA_COMMIT" _content)
	STRING(STRIP "${_content}" _content)
	IF(_content MATCHES "^[0-9a-f]+(-dirty)?$")
		STRING(REGEX REPLACE "-dirty$" "" _hex "${_content}")
		STRING(LENGTH "${_hex}" _length)
		IF(_length EQUAL 40)
			SET(_id "${_content}")
		ENDIF()
	ENDIF()
ENDIF()

# 3. neither: a Geneva build fails; a program is not stamped
IF(_id STREQUAL "" AND MODE STREQUAL "geneva")
	MESSAGE(FATAL_ERROR
		"The Geneva sources in ${_source_dir} do not say which commit they are. A Geneva build must "
		"know its commit (a server and its clients must be built from the same one). Give it in one "
		"of three ways:\n"
		"  - build from a git checkout, with git installed;\n"
		"  - build from an archive made by \"git archive\", which writes the id into the file "
		"GENEVA_COMMIT;\n"
		"  - write the commit id (40 hexadecimal digits, optionally followed by \"-dirty\") into the "
		"file GENEVA_COMMIT at the top of the sources -- as a build that copies the sources without "
		".git must (the container build takes it as the build argument GENEVA_COMMIT).")
ENDIF()

SET(_source [=[
// Written by CMakeModules/GenevaCommit.cmake from the commit of the Geneva sources; do not edit.
#include "common/GGenevaCommit.hpp"

std::string_view Gem::Common::genevaCommit() noexcept {
    return "@ID@";
}
]=])
STRING(REPLACE "@ID@" "${_id}" _source "${_source}")
IF(MODE STREQUAL "program")
	# The identity as the text of a C++ string literal
	STRING(REPLACE "\\" "\\\\" _literal "${_id}")
	STRING(REPLACE "\"" "\\\"" _literal "${_literal}")
	SET(_source [=[
// Written by GenevaCommit.cmake for GENEVA_STAMP_PROGRAM (GenevaProgramStamp.cmake); do not edit.
#include "common/GGenevaCommit.hpp"

namespace {
/// Sets the identity of this program before main(); an empty one leaves the program unchecked
const struct GenevaProgramStamp {
    GenevaProgramStamp() { Gem::Common::setProgramIdentity("@ID@"); }
} genevaProgramStamp;
} // namespace
]=])
	STRING(REPLACE "@ID@" "${_literal}" _source "${_source}")
ENDIF()
SET(_old "")
IF(EXISTS "${OUTPUT}")
	FILE(READ "${OUTPUT}" _old)
ENDIF()
IF(NOT _old STREQUAL _source)
	FILE(WRITE "${OUTPUT}" "${_source}")
ENDIF()
IF(MODE STREQUAL "geneva")
	MESSAGE(STATUS "Geneva commit: ${_id}")
ELSEIF(NOT QUIET)
	MESSAGE(STATUS "Program identity: ${_id}")
ENDIF()
