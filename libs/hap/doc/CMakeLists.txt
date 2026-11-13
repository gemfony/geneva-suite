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
# Per-library Doxygen target for the Hap library.

IF (NOT DOXYGEN_FOUND)
	RETURN ()
ENDIF ()

SET (GENEVA_DOC_LIB_NAME "hap")
SET (GENEVA_DOC_LIB_DESC "Random-number library: GRandomT, GRandomFactory, "
		"distribution wrappers, xoshiro256++ "
		"engine with optional SIMD bulk-refill")
# Upstream: common -- relative html path from hap/doc/html to common/doc/html.
SET (GENEVA_DOC_TAGFILES
		"${CMAKE_BINARY_DIR}/common/doc/common.tag=../../../common/doc/html")

CONFIGURE_FILE (
	${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
	${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	@ONLY
)

ADD_CUSTOM_TARGET (doc-hap
	COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating Doxygen documentation for the Hap library"
	VERBATIM
)

# Tag-file ordering: common's doc must exist when hap's runs so the upstream
# tag file is available for cross-references.
ADD_DEPENDENCIES (doc-hap doc-common)
