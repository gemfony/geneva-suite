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

# This example has TWO artifacts: a runtime-loadable individual (the problem, built as a shared object)
# and a GENERIC optimizer that loads it via --individual and has no problem compiled in.

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK ${LIBRARIES_TO_LINK} ${MPI_LIBRARIES})
ENDIF()

# (1) The loadable individual plugin -> libGLoadableParaboloid.so. It carries the problem code + its
# archive-tag registration + the plugin entry points. GENEVA_ADD_INDIVIDUAL_MODULE builds it as a shared MODULE
# that links none of the Geneva libraries (it resolves their symbols from the HOST PROCESS at load time via
# RTLD_GLOBAL, needing only their headers) -- the reusable recipe every modularised individual now shares.
GENEVA_ADD_INDIVIDUAL_MODULE(GLoadableParaboloid GLoadableParaboloidPlugin.cpp)

# (2) The generic optimizer -> loads a problem at runtime. ENABLE_EXPORTS so a plugin loaded RTLD_GLOBAL
# resolves against one symbol namespace (single archive registry + single Geneva singletons).
ADD_EXECUTABLE(GGenericOptimizer GGenericOptimizer.cpp)
SET_TARGET_PROPERTIES(GGenericOptimizer PROPERTIES ENABLE_EXPORTS TRUE)
TARGET_LINK_LIBRARIES(GGenericOptimizer ${LIBRARIES_TO_LINK})

# Materialize the configuration from code defaults at build time. This plugin loader only knows its full
# configuration once an individual is loaded, so materialize with the just-built plugin (and depend on it so
# it exists first).
ADD_DEPENDENCIES(GGenericOptimizer GLoadableParaboloid)
GENEVA_MATERIALIZE_CONFIGS(GGenericOptimizer
	${INSTALL_PREFIX_DATA}/examples/GLoadableIndividual/config
	--individual $<TARGET_FILE:GLoadableParaboloid>)

# Module-ABI smoke test: the generic optimizer dlopen()s the just-built individual plugin through the real
# loader path and optimizes it. This is also the empirical check that the plugin and process share ONE set of
# singletons / one serialization registry.
#
# This trio (examples 18, 20 and 21) is the ONLY automated coverage of the loadable-module ABI, and every
# change to that ABI must be gated on it. The run uses the materialized default configuration and finishes
# in well under a second (the stall criterion halts it after a few dozen iterations), so it costs the suite
# nothing (Inv 22).
ADD_TEST(
	NAME GLoadableIndividual
	COMMAND GGenericOptimizer --individual $<TARGET_FILE:GLoadableParaboloid>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
# A dlopen smoke test that hangs (rather than fails) would otherwise stall the gate for CTest's 25-minute
# default; a sub-second run gets a generous but finite bound.
SET_TESTS_PROPERTIES(GLoadableIndividual PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GLoadableIndividual S)

# Regression guard (Inv 15) for the GCudaRNG teardown race fixed in hap/src/GCUDARng.cu. At process exit the
# CUDA runtime unloads while the GRandomFactory's producer threads may still be refilling on the GPU; cuRAND
# reports that as CURAND_STATUS_LAUNCH_FAILURE, which used to leak "cuRAND call failed" to stderr because the
# cuRAND error path could not recognise shutdown on its own. This module optimizer keeps those producer
# threads saturated until it exits -- the reproducer that reliably provokes the race -- so we run it many
# times and fail if the noise reappears in ANY run. Registered only when the CUDA RNG backend is built -- the
# defect cannot occur otherwise -- which is a real, configure-time-detected condition.
IF(GENEVA_USE_CUDA_RNG_ENABLED)
	ADD_TEST(
		NAME GCudaRngShutdownNoise
		COMMAND ${CMAKE_COMMAND}
			-DBIN=$<TARGET_FILE:GGenericOptimizer>
			-DPLUGIN=$<TARGET_FILE:GLoadableParaboloid>
			-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
			-DRUNS=20
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoCudaShutdownNoise.cmake
	)
	SET_TESTS_PROPERTIES(GCudaRngShutdownNoise PROPERTIES LABELS "geneva;example")
	GENEVA_TEST_SIZE(GCudaRngShutdownNoise M)
ENDIF()

INSTALL(TARGETS GGenericOptimizer GLoadableParaboloid
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/GLoadableIndividual)
