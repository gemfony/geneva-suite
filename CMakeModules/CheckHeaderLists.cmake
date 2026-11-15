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
# Driver for the test HeaderListsSortedAndComplete.
#
# Each library's libs/<library>/include/<library>/CMakeLists.txt names its headers
# in SET lists for the IDE's header target (the headers are installed by
# directory, so the lists do not decide what is shipped). A list that is not in
# order is hard to check by eye, and a header that is in no list is missing from
# the IDE target without anyone noticing. This driver reads every such file and
# fails if
#
#   * the header names of one SET command are not in ascending (ASCII) order, or
#   * a header (.hpp, .cuh) of the directory is named in none of the file's SET
#     commands -- also those that only apply to an optional part of the build, or
#   * a SET command names a header that does not exist.
#
# Input (passed via -D):
#   SOURCE_DIR   the libs/ directory
#
################################################################################

IF(NOT SOURCE_DIR)
	MESSAGE(FATAL_ERROR "CheckHeaderLists.cmake needs -DSOURCE_DIR=<the libs directory>")
ENDIF()

FILE(GLOB _lists "${SOURCE_DIR}/*/include/*/CMakeLists.txt")
IF(NOT _lists)
	MESSAGE(FATAL_ERROR "No header CMakeLists.txt found under ${SOURCE_DIR}: the search is broken.")
ENDIF()

SET(_problems "")
FOREACH(_list IN LISTS _lists)
	GET_FILENAME_COMPONENT(_dir "${_list}" DIRECTORY)
	FILE(RELATIVE_PATH _rel_list "${SOURCE_DIR}" "${_list}")
	FILE(READ "${_list}" _text)

	# Every SET command whose arguments name headers (the header names contain no ")").
	STRING(REGEX MATCHALL "[Ss][Ee][Tt][ \t]*\\([^)]*\\)" _sets "${_text}")
	SET(_named "")
	FOREACH(_set IN LISTS _sets)
		STRING(REGEX MATCHALL "[A-Za-z0-9_./-]+\\.(hpp|cuh)" _headers "${_set}")
		IF(NOT _headers)
			CONTINUE()
		ENDIF()
		SET(_sorted ${_headers})
		LIST(SORT _sorted)
		IF(NOT "${_sorted}" STREQUAL "${_headers}")
			STRING(REGEX REPLACE "^[^(]*\\([ \t]*([A-Za-z0-9_]+).*$" "\\1" _set_name "${_set}")
			STRING(APPEND _problems "  ${_rel_list}: the list ${_set_name} is not in order\n")
		ENDIF()
		LIST(APPEND _named ${_headers})
	ENDFOREACH()

	FILE(GLOB_RECURSE _present RELATIVE "${_dir}" "${_dir}/*.hpp" "${_dir}/*.cuh")
	FOREACH(_header IN LISTS _present)
		IF(NOT _header IN_LIST _named)
			STRING(APPEND _problems "  ${_rel_list}: ${_header} is in no list\n")
		ENDIF()
	ENDFOREACH()
	FOREACH(_header IN LISTS _named)
		IF(NOT EXISTS "${_dir}/${_header}")
			STRING(APPEND _problems "  ${_rel_list}: ${_header} does not exist\n")
		ENDIF()
	ENDFOREACH()
ENDFOREACH()

IF(NOT _problems STREQUAL "")
	MESSAGE(FATAL_ERROR "The header lists are not sorted and complete:\n${_problems}")
ENDIF()
LIST(LENGTH _lists _n_lists)
MESSAGE(STATUS "${_n_lists} header CMakeLists.txt: every list sorted, every header listed")
