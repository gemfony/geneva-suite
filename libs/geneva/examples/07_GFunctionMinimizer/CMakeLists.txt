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

# This example demonstrates the load-from-disk model: the optimization problem (GFMinIndividual) is a
# runtime-loadable module, and it is optimized by the generic optimizer (example 18's GGenericOptimizer)
# via --individual, rather than by a bespoke main. There is no example-specific executable -- "load a
# problem, run an algorithm, print the best" is exactly what the generic optimizer already does. All
# configuration (Go2, the algorithm, the individual) is auto-created from code defaults on first run.

# The loadable problem module -> libGFMinIndividual.so (problem code + archive tag + manifest glue),
# linking none of the Geneva libraries (their symbols resolve from the generic optimizer at load).
GENEVA_DECLARE_INDIVIDUAL(GFMinIndividual
	MODE    load
	SOURCES GFMinIndividual.cpp
	PLUGIN  GFMinIndividualPlugin.cpp
	CONFIG  ./config/GFMinIndividual.json)

# End-to-end test: the generic optimizer loads this problem module and optimizes it (configs auto-create in
# the build directory). GGenericOptimizer is provided by example 18.
ADD_TEST(
	NAME GFunctionMinimizer
	COMMAND $<TARGET_FILE:GGenericOptimizer> --individual $<TARGET_FILE:GFMinIndividual>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
# Configs auto-create on the first run and the stall criterion halts it in well under a second.
SET_TESTS_PROPERTIES(GFunctionMinimizer PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GFunctionMinimizer S)

INSTALL ( TARGETS GFMinIndividual DESTINATION ${INSTALL_PREFIX_DATA}/examples/GFunctionMinimizer )
