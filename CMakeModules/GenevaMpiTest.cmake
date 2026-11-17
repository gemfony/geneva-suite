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
# through a driver -- with a bound on its command. Every such entry is registered with it: the entry runs
# under GenevaRunMpiTest.sh, which gives it a directory of its own for the files of Open MPI's
# shared-memory transport (OMPI_MCA_btl_sm_backing_directory) and removes it afterwards, also
# when the entry fails or reaches its bound. Two entries that run at the same time could otherwise
# get the same names for these files (the script says why) and crash or hang.
#
# TIMEOUT is the bound of <command>: the script ends it then (SIGTERM, SIGKILL ten seconds later).
# The entry's ctest TIMEOUT is the bound plus GENEVA_MPI_TEST_MARGIN, the time the script needs to
# end <command> and remove the directory before ctest would end the script without a chance to.
# The margin is stated here only; GenevaMpiTestsRegistered is given it. The script's signals reach
# <command>'s process group: a driver that starts MPI jobs in a session or process group of its own
# (the speed-up checks do) ends them by its own limits, before the bound. Labels, PROCESSORS and
# the size (GENEVA_TEST_SIZE) are set by the caller, as for any entry. The tests GenevaMpiTestDirectories and GenevaMpiTestsRegistered
# (top-level CMakeLists.txt) check the script and that every entry that names the launcher is
# registered here.
#
################################################################################

SET(GENEVA_MPI_TEST_RUNNER ${CMAKE_CURRENT_LIST_DIR}/GenevaRunMpiTest.sh)
# The seconds between the bound of an entry's command and the entry's ctest TIMEOUT
SET(GENEVA_MPI_TEST_MARGIN 15)

FUNCTION(GENEVA_ADD_MPI_TEST)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 0 arg "" "NAME;TIMEOUT;WORKING_DIRECTORY" "COMMAND")
	IF(NOT arg_NAME OR NOT arg_COMMAND)
		MESSAGE(FATAL_ERROR "GENEVA_ADD_MPI_TEST needs NAME and COMMAND")
	ENDIF()
	IF(NOT "${arg_TIMEOUT}" MATCHES "^[1-9][0-9]*$")
		MESSAGE(FATAL_ERROR "GENEVA_ADD_MPI_TEST(${arg_NAME}): TIMEOUT, the bound of the command, "
			"must be a number of seconds above 0")
	ENDIF()
	IF(arg_UNPARSED_ARGUMENTS)
		MESSAGE(FATAL_ERROR
			"GENEVA_ADD_MPI_TEST(${arg_NAME}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
	ENDIF()
	MATH(EXPR _timeout "${arg_TIMEOUT} + ${GENEVA_MPI_TEST_MARGIN}")
	IF(arg_WORKING_DIRECTORY)
		SET(_where WORKING_DIRECTORY ${arg_WORKING_DIRECTORY})
	ELSE()
		SET(_where "")
	ENDIF()
	ADD_TEST(NAME ${arg_NAME}
		COMMAND ${GENEVA_MPI_TEST_RUNNER} ${arg_TIMEOUT} ${arg_COMMAND}
		${_where})
	SET_TESTS_PROPERTIES(${arg_NAME} PROPERTIES TIMEOUT ${_timeout})
ENDFUNCTION()
