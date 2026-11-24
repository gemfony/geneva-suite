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
#
# Driver for the test PrepareBuildFailsWhenCMakeFails.
#
# scripts/prepareBuild.sh configures a build directory with the cmake named in its .gcfg file. A
# gate that runs the script decides by its exit status, so a configuration that fails must end the
# script with a status other than 0. This driver runs the script in a directory of its own with a
# .gcfg whose "cmake" always fails (/bin/false) and fails if the script reports success.
#
# Input (passed via -D):
#   SCRIPT     the path of scripts/prepareBuild.sh
#   WORK_DIR   a directory in the build tree this driver may remove and create
#
################################################################################

IF(NOT SCRIPT OR NOT WORK_DIR)
	MESSAGE(FATAL_ERROR "CheckPrepareBuildStatus.cmake needs -DSCRIPT=<prepareBuild.sh> and -DWORK_DIR=<directory>")
ENDIF()
IF(NOT EXISTS "/bin/false")
	MESSAGE(FATAL_ERROR "No /bin/false on this system: the check cannot be made.")
ENDIF()

FILE(REMOVE_RECURSE "${WORK_DIR}")
FILE(MAKE_DIRECTORY "${WORK_DIR}")
FILE(WRITE "${WORK_DIR}/failing.gcfg" "CMAKE=\"/bin/false\"\n")

EXECUTE_PROCESS(
	COMMAND "${SCRIPT}" failing.gcfg
	WORKING_DIRECTORY "${WORK_DIR}"
	RESULT_VARIABLE _status
	OUTPUT_VARIABLE _output
	ERROR_VARIABLE _output
)

# The script must have come as far as the configuration, or the check proves nothing
IF(NOT _output MATCHES "Configuring with command")
	MESSAGE(FATAL_ERROR "prepareBuild.sh ended before it configured:\n${_output}")
ENDIF()
IF(_status EQUAL 0)
	MESSAGE(FATAL_ERROR "prepareBuild.sh reported success (status 0) although its cmake failed:\n${_output}")
ENDIF()
MESSAGE(STATUS "prepareBuild.sh ended with status ${_status} after a failed configuration")
