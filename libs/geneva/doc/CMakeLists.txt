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
# Per-library Doxygen target for the Geneva library (the largest; includes
# the dissolved geneva-individuals sources under individuals/).

IF (NOT DOXYGEN_FOUND)
	RETURN ()
ENDIF ()

SET (GENEVA_DOC_LIB_NAME "geneva")
SET (GENEVA_DOC_LIB_DESC "Core optimisation: Go2 driver, GGenome "
		"workflow, seven optimisation algorithms (EA/SA/Swarm/GD/CGD/NM/PS), "
		"per-category GCommonInterfaceT<Root> CRTP roots, individuals, "
		"pluggable monitors")
# Upstream: common, hap, courtier. Single space-separated string (see the
# courtier counterpart for the multi-arg-SET caveat).
SET (GENEVA_DOC_TAGFILES
		"${CMAKE_BINARY_DIR}/common/doc/common.tag=../../../common/doc/html ${CMAKE_BINARY_DIR}/hap/doc/hap.tag=../../../hap/doc/html ${CMAKE_BINARY_DIR}/courtier/doc/courtier.tag=../../../courtier/doc/html")

CONFIGURE_FILE (
	${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
	${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	@ONLY
)

ADD_CUSTOM_TARGET (doc-geneva
	COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating Doxygen documentation for the Geneva library"
	VERBATIM
)

ADD_DEPENDENCIES (doc-geneva doc-common doc-hap doc-courtier)
