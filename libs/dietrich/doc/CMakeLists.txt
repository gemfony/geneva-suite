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
# Per-library Doxygen target for the Dietrich plotting library.
#
# Configures the shared CMakeModules/Doxyfile.in with per-library values and
# registers `doc-dietrich` as a top-level target. Output (HTML + XML + tag
# file) lands in this lib's build dir, NOT in the source tree.

IF (NOT DOXYGEN_FOUND)
	RETURN ()
ENDIF ()

SET (GEM_DOC_LIB_NAME "dietrich")
STRING (CONCAT GEM_DOC_LIB_DESC "Plotting library: ROOT macros, gnuplot / matplotlib "
		"scripts, CSV / .npz data exports and a self-describing manifest for an "
		"external renderer (named for the painter C. W. E. Dietrich)")
# Dietrich depends on common; reference its tag file for cross-links (built in libs/common/doc;
# the html path is relative to libs/dietrich/doc/html).
SET (GEM_DOC_TAGFILES "${CMAKE_BINARY_DIR}/libs/common/doc/common.tag=../../../common/doc/html")

CONFIGURE_FILE (
	${PROJECT_SOURCE_DIR}/CMakeModules/Doxyfile.in
	${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	@ONLY
)

ADD_CUSTOM_TARGET (doc-dietrich
	COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating Doxygen documentation for the Dietrich library"
	VERBATIM
)

# Tag-file ordering: common's doc must exist when dietrich's runs.
ADD_DEPENDENCIES (doc-dietrich doc-common)
