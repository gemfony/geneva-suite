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

SET (GEM_DOC_LIB_NAME "geneva")
STRING (CONCAT GEM_DOC_LIB_DESC "Core optimisation: Go2 driver, the flat genome "
		"(GGenome), the optimisation algorithms, individuals, pluggable monitors")
# Upstream: common, hap, dietrich, courtier, each built in libs/<lib>/doc of the build tree; the
# html paths are relative to libs/geneva/doc/html. Joined into one space-separated string (see the
# courtier counterpart for why it is not a CMake list).
SET (_tag_dir "${CMAKE_BINARY_DIR}/libs")
STRING (JOIN " " GEM_DOC_TAGFILES
		"${_tag_dir}/common/doc/common.tag=../../../common/doc/html"
		"${_tag_dir}/hap/doc/hap.tag=../../../hap/doc/html"
		"${_tag_dir}/dietrich/doc/dietrich.tag=../../../dietrich/doc/html"
		"${_tag_dir}/courtier/doc/courtier.tag=../../../courtier/doc/html")

CONFIGURE_FILE (
	${PROJECT_SOURCE_DIR}/CMakeModules/Doxyfile.in
	${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	@ONLY
)

ADD_CUSTOM_TARGET (doc-geneva
	COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating Doxygen documentation for the Geneva library"
	VERBATIM
)

ADD_DEPENDENCIES (doc-geneva doc-common doc-hap doc-dietrich doc-courtier)
