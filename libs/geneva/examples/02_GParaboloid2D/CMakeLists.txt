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

# This introductory example ships the SAME problem (GParaboloidIndividual2D) packaged BOTH ways, side by
# side, so the two models can be compared directly:
#
#   GParaboloid2D-fixed  -- an executable with the individual compiled straight in: the classic, minimal
#     model the Geneva manual introduces (subclass GGenome, override evaluate(), compile it in).
#
#   GParaboloid2D-module -- the individual built as a runtime-loadable module (libGParaboloid2D-module.so),
#     optimized by the generic optimizer (example 18's GGenericOptimizer) via --individual, with no bespoke
#     driver. Configuration is auto-created from code defaults on first run.
#
# One source serves both: the individual supplies the Tier-2 factory hooks (Config / describeConfig /
# buildGenome / buildAdaptionConfig) the module path needs while keeping its constructor and one-argument
# buildAdaptionConfig for the compiled-in driver. The two artifacts are separate binaries (one process each),
# so the single archive registration is never duplicated.

# (1) The compiled-in variant -> executable GParaboloid2D-fixed.
ADD_EXECUTABLE(GParaboloid2D-fixed
	GParaboloid2D.cpp
	GParaboloidIndividual2D.cpp
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (GParaboloid2D-fixed
		${LIBRARIES_TO_LINK}
		)

GENEVA_MATERIALIZE_CONFIGS ( GParaboloid2D-fixed ${INSTALL_PREFIX_DATA}/examples/GParaboloid2D/config )

# (2) The loadable variant -> module libGParaboloid2D-module.so, run by the generic optimizer.
GENEVA_DECLARE_INDIVIDUAL(GParaboloid2D-module
	MODE    load
	SOURCES GParaboloidIndividual2D.cpp
	PLUGIN  GParaboloidIndividual2DPlugin.cpp
	CONFIG  ./config/GParaboloidIndividual2D.json)

# Both packagings of the same problem are tested, which is the point of this example: they must reach
# the same answer by two different routes. Each halts on the stall criterion in well under a second.
ADD_TEST(NAME GParaboloid2D-fixed COMMAND GParaboloid2D-fixed
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
SET_TESTS_PROPERTIES(GParaboloid2D-fixed PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GParaboloid2D-fixed S)

ADD_TEST(
	NAME GParaboloid2D-module
	COMMAND $<TARGET_FILE:GGenericOptimizer> --individual $<TARGET_FILE:GParaboloid2D-module>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
SET_TESTS_PROPERTIES(GParaboloid2D-module PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GParaboloid2D-module S)

INSTALL ( TARGETS GParaboloid2D-fixed GParaboloid2D-module
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/GParaboloid2D )
