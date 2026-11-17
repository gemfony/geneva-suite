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
# Driver for the test SpecialMemberNolintEffective: a suppression of
# cppcoreguidelines-special-member-functions sits on the line that names the
# class.
#
# clang-tidy reports that check at the class's name, and a trailing NOLINT
# suppresses only what is reported on its own line. Written after the base
# list ("  , public Base { // NOLINT(...)") it suppresses nothing, and the
# warning it was meant to silence is reported all the same. The driver reads
# the C++ sources under SOURCE_DIR and fails with every NOLINT of that check
# on a line that does not begin with "class" or "struct" (NOLINTBEGIN and
# NOLINTNEXTLINE are not affected).
#
# Input (passed via -D):
#   SOURCE_DIR   the directory to search (the libs/ tree)
#
################################################################################

IF(NOT SOURCE_DIR)
	MESSAGE(FATAL_ERROR "CheckSpecialMemberNolint.cmake needs -DSOURCE_DIR=<directory>")
ENDIF()

FILE(GLOB_RECURSE _sources
	"${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.hpp" "${SOURCE_DIR}/*.cu" "${SOURCE_DIR}/*.cuh")

SET(_n_found 0)
SET(_offenders "")
FOREACH(_file IN LISTS _sources)
	FILE(STRINGS "${_file}" _lines REGEX "NOLINT\\(cppcoreguidelines-special-member-functions\\)")
	FOREACH(_line IN LISTS _lines)
		MATH(EXPR _n_found "${_n_found} + 1")
		IF(NOT _line MATCHES "^[ \t]*(class|struct)[ \t]")
			FILE(RELATIVE_PATH _rel "${SOURCE_DIR}" "${_file}")
			STRING(STRIP "${_line}" _stripped)
			STRING(APPEND _offenders "\n  ${_rel}: ${_stripped}")
		ENDIF()
	ENDFOREACH()
ENDFOREACH()

IF(NOT "${_offenders}" STREQUAL "")
	MESSAGE(FATAL_ERROR "CheckSpecialMemberNolint: a NOLINT that suppresses nothing (it belongs on "
		"the line that names the class):${_offenders}")
ENDIF()
MESSAGE(STATUS "CheckSpecialMemberNolint: ${_n_found} suppression(s), each on its class's line")
