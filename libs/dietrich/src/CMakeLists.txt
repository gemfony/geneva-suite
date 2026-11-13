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

SET ( DIETRICHSOURCES
	GPlotDesigner.cpp
	GDataLog.cpp
	GGraphPlots.cpp
	GHistogramPlots.cpp
	GRootEmitter.cpp
	GGnuplotEmitter.cpp
	GMatplotlibEmitter.cpp
	GOctaveEmitter.cpp
	GDataEmitter.cpp
)

set_source_files_properties(
    ${DIETRICHSOURCES}
    PROPERTIES
    LANGUAGE CXX
)

ADD_LIBRARY ( ${DIETRICH_LIBNAME} SHARED ${DIETRICHSOURCES} )

SET_TARGET_PROPERTIES(
    ${DIETRICH_LIBNAME}
    PROPERTIES
    VERSION ${GENEVA_VERSION}
	EXPORT_NAME dietrich
)

# Publish the include directory through the target so that both the in-tree
# build and downstream consumers (via the exported Geneva::dietrich target) pick
# up the Dietrich headers. PUBLIC so the requirement propagates to dependents.
TARGET_INCLUDE_DIRECTORIES(
    ${DIETRICH_LIBNAME}
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libs/dietrich/include>
    $<INSTALL_INTERFACE:include>
)

# Dietrich depends ONLY on common (and the Boost components that appear in the
# public plotting headers). It must NOT link hap, courtier or geneva.
TARGET_LINK_LIBRARIES(
    ${DIETRICH_LIBNAME}
    PUBLIC
    ${COMMON_LIBNAME}
)

INSTALL ( TARGETS ${DIETRICH_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )
