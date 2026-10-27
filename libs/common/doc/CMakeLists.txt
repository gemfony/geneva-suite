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
# Per-library Doxygen target for the Common library.
#
# Configures Doxyfile.in from this directory with per-library values and
# registers `doc-common` as a top-level target. Output (HTML + XML + tag
# file) lands in this lib's build dir, NOT in the source tree.

IF (NOT DOXYGEN_FOUND)
	RETURN ()
ENDIF ()

SET (GENEVA_DOC_LIB_NAME "common")
SET (GENEVA_DOC_LIB_DESC "Utilities: logging, thread pool, formula parser, "
		"plot designer, serialization helpers, bounded buffers, exception "
		"types")
# No upstream libraries -- common is the foundation.
SET (GENEVA_DOC_TAGFILES "")

CONFIGURE_FILE (
	${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
	${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	@ONLY
)

ADD_CUSTOM_TARGET (doc-common
	COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating Doxygen documentation for the Common library"
	VERBATIM
)
