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
# GENEVA_STAMP_PROGRAM(<target> [IDENTITY <text>])
#
# Stamps the executable <target> with the identity of the user's program. A server of the networked
# consumers names it in the HELLO it greets each client with, beside the commit of Geneva, and a
# client whose program has another identity ends (Gem::Common::setProgramIdentity(),
# common/GGenevaCommit.hpp). So a server and its clients are known to be built from the same sources.
#
#  - With IDENTITY, <text> is the identity.
#  - Without it, the identity is the commit of the project's sources (CMAKE_SOURCE_DIR), taken at
#    every build by the rule of GenevaCommit.cmake: if the sources are the top of a git work tree,
#    the id of HEAD, with "-dirty" appended when a tracked file differs from it. Sources without git
#    give no identity: CMake says once, at configure time, that the program is not stamped, and it is
#    not checked against a server's program (its log says so once per run).
#
# The function writes one source into the target's build directory -- one static object whose
# constructor calls Gem::Common::setProgramIdentity() before main() -- and adds it to the target,
# which links Geneva (Geneva::geneva or Geneva::common). The source is rewritten only when the
# identity changes. Only executables are stamped: a module loaded at run time is checked by the
# module loader's own gates.
#
# This module is shared by the in-tree build (included from CommonGenevaBuild.cmake) and the
# installed package (included from GenevaConfig.cmake); GenevaCommit.cmake must sit next to it.
#
################################################################################

SET(GENEVA_PROGRAM_STAMP_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/GenevaCommit.cmake)

FUNCTION(GENEVA_STAMP_PROGRAM _target)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 1 _arg "" "IDENTITY" "")
	IF(_arg_UNPARSED_ARGUMENTS)
		MESSAGE(FATAL_ERROR "GENEVA_STAMP_PROGRAM(${_target}): unknown arguments ${_arg_UNPARSED_ARGUMENTS}")
	ENDIF()
	IF(NOT TARGET ${_target})
		MESSAGE(FATAL_ERROR "GENEVA_STAMP_PROGRAM(${_target}): there is no target ${_target}")
	ENDIF()
	GET_TARGET_PROPERTY(_type ${_target} TYPE)
	IF(NOT _type STREQUAL "EXECUTABLE")
		MESSAGE(FATAL_ERROR
			"GENEVA_STAMP_PROGRAM(${_target}): ${_target} is a ${_type}, not an executable. Only a "
			"program is stamped; a module loaded at run time is checked by the module loader.")
	ENDIF()

	GET_TARGET_PROPERTY(_binary_dir ${_target} BINARY_DIR)
	SET(_source ${_binary_dir}/${_target}-GenevaProgramStamp.cpp)
	SET(_command ${CMAKE_COMMAND} -DMODE=program -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_source})
	IF(DEFINED _arg_IDENTITY)
		IF(_arg_IDENTITY STREQUAL "")
			MESSAGE(FATAL_ERROR "GENEVA_STAMP_PROGRAM(${_target}): IDENTITY is empty")
		ENDIF()
		LIST(APPEND _command "-DIDENTITY=${_arg_IDENTITY}")
	ENDIF()

	# Now, so that the source exists and CMake can say what the identity is ...
	EXECUTE_PROCESS(
		COMMAND ${_command} -P ${GENEVA_PROGRAM_STAMP_SCRIPT}
		RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
	IF(NOT _rc EQUAL 0)
		MESSAGE(FATAL_ERROR "GENEVA_STAMP_PROGRAM(${_target}): ${_err}")
	ENDIF()
	SET(_identity "")
	IF(_out MATCHES "Program identity: ([^\n]*)")
		SET(_identity "${CMAKE_MATCH_1}")
	ENDIF()
	IF("${_identity}" STREQUAL "")
		MESSAGE(STATUS
			"${_target} is not stamped: GENEVA_STAMP_PROGRAM was given no IDENTITY, and "
			"${CMAKE_SOURCE_DIR} is not the top of a git work tree (or git is not installed); the "
			"program is not checked against a server's program")
	ELSE()
		MESSAGE(STATUS "${_target} is stamped with the identity ${_identity}")
	ENDIF()

	# ... and, for the commit, at every build, so that a commit made after the configure is seen
	IF(NOT DEFINED _arg_IDENTITY)
		ADD_CUSTOM_TARGET(${_target}-program-stamp
			COMMAND ${_command} -DQUIET=ON -P ${GENEVA_PROGRAM_STAMP_SCRIPT}
			BYPRODUCTS ${_source}
			COMMENT "Taking the identity of ${_target}"
			VERBATIM)
		ADD_DEPENDENCIES(${_target} ${_target}-program-stamp)
	ENDIF()
	TARGET_SOURCES(${_target} PRIVATE ${_source})
ENDFUNCTION()
