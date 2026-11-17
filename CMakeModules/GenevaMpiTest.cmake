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
# GENEVA_ADD_MPI_TEST(NAME <name> TIMEOUT <seconds> COMMAND <command> [<argument>...]
#                     [WORKING_DIRECTORY <dir>])
#
# Registers a CTest entry that starts MPI jobs -- one that runs the MPI launcher, directly or
# through a driver -- and sets its TIMEOUT. Every such entry is registered with it: the entry runs
# under GenevaRunMpiTest.sh, which gives it a directory of its own for the files of Open MPI's
# shared-memory transport (OMPI_MCA_btl_sm_backing_directory) and removes it afterwards, also
# when the entry fails or reaches its bound. Two entries that run at the same time could otherwise
# get the same names for these files (the script says why) and crash or hang.
#
# The script ends <command> 15 s before TIMEOUT, so that it can still remove the directory: ctest
# would end it without a chance to. Labels, PROCESSORS and the size (GENEVA_TEST_SIZE) are set by
# the caller, as for any entry. The tests GenevaMpiTestDirectories and GenevaMpiTestsRegistered
# (top-level CMakeLists.txt) check the script and that every entry that names the launcher is
# registered here.
#
################################################################################

SET(GENEVA_MPI_TEST_RUNNER ${CMAKE_CURRENT_LIST_DIR}/GenevaRunMpiTest.sh)

FUNCTION(GENEVA_ADD_MPI_TEST)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 0 arg "" "NAME;TIMEOUT;WORKING_DIRECTORY" "COMMAND")
	IF(NOT arg_NAME OR NOT arg_COMMAND)
		MESSAGE(FATAL_ERROR "GENEVA_ADD_MPI_TEST needs NAME and COMMAND")
	ENDIF()
	IF(NOT "${arg_TIMEOUT}" MATCHES "^[0-9]+$" OR arg_TIMEOUT LESS 30)
		MESSAGE(FATAL_ERROR "GENEVA_ADD_MPI_TEST(${arg_NAME}): TIMEOUT must be a number of seconds, "
			"at least 30 (the entry's command is ended 15 s before it)")
	ENDIF()
	IF(arg_UNPARSED_ARGUMENTS)
		MESSAGE(FATAL_ERROR
			"GENEVA_ADD_MPI_TEST(${arg_NAME}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
	ENDIF()
	MATH(EXPR _bound "${arg_TIMEOUT} - 15")
	IF(arg_WORKING_DIRECTORY)
		SET(_where WORKING_DIRECTORY ${arg_WORKING_DIRECTORY})
	ELSE()
		SET(_where "")
	ENDIF()
	ADD_TEST(NAME ${arg_NAME}
		COMMAND ${GENEVA_MPI_TEST_RUNNER} ${_bound} ${arg_COMMAND}
		${_where})
	SET_TESTS_PROPERTIES(${arg_NAME} PROPERTIES TIMEOUT ${arg_TIMEOUT})
ENDFUNCTION()
