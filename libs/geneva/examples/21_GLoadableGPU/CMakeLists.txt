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

# Example 21 -- the GPU North Star of the loadable-module scheme. ONE loadable module carries BOTH the
# optimization individual AND its GPU marshaller; a generic optimizer loads it via --module and runs it
# entirely on the device with `--consumer gpu`. It is built only when the GPU consumer is (this directory is
# added under GENEVA_GPU_CONSUMER_ENABLED in geneva/examples/CMakeLists.txt). The kernel is runtime-compiled
# by NVRTC, so no CUDA language is enabled at build time -- an ordinary C++ toolchain builds it and the
# backend/kernel are selected at run time via config/GGPUConsumer.json.

# Headers, listed for CLion indexing.
SET(GLOADABLEGPU_INCLUDES
	GGPUParaboloid.hpp
	GGPUParaboloidMarshaller.hpp
)
ADD_CUSTOM_TARGET(CLION_EXAMPLE22_HEADERS SOURCES ${GLOADABLEGPU_INCLUDES})

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK ${LIBRARIES_TO_LINK} ${MPI_LIBRARIES})
ENDIF()

# (1) The loadable GPU-problem module -> libGGPUParaboloid.so. It carries the individual, its GPU marshaller,
# the individual's archive tag and a TWO-contribution manifest entry point. GENEVA_ADD_INDIVIDUAL_MODULE
# is kind-agnostic -- it builds a shared MODULE that links none of the Geneva libraries (resolving their
# symbols from the HOST PROCESS at load time via RTLD_GLOBAL, needing only their headers, with pinned ABI
# flags) -- so it serves an individual-plus-marshaller module exactly as it serves any other.
GENEVA_ADD_INDIVIDUAL_MODULE(GGPUParaboloid GGPUParaboloidPlugin.cpp)

# (2) The generic optimizer -> loads the GPU problem at runtime. ENABLE_EXPORTS so a module loaded RTLD_GLOBAL
# resolves against one symbol namespace (single archive registry + single Geneva singletons).
ADD_EXECUTABLE(GGPUParaboloidOptimizer GGPUParaboloidOptimizer.cpp)
SET_TARGET_PROPERTIES(GGPUParaboloidOptimizer PROPERTIES ENABLE_EXPORTS TRUE)
TARGET_LINK_LIBRARIES(GGPUParaboloidOptimizer ${LIBRARIES_TO_LINK})

# Runtime asset: the kernel is runtime-loaded source, so copy it at BUILD time (editing + rebuilding then
# re-stages it, unlike a stale configure-time copy).
ADD_CUSTOM_COMMAND(TARGET GGPUParaboloidOptimizer POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/kernels ${CMAKE_CURRENT_BINARY_DIR}/kernels)

# Materialize the configuration from code defaults at build time and overlay this example's overrides
# (config/config-overrides.json: consumer=gpu and the GPU-consumer kernel path). The individual's and GPU
# consumer's configs only exist once the module is loaded, so materialize with the just-built module (and
# depend on it so it exists first).
ADD_DEPENDENCIES(GGPUParaboloidOptimizer GGPUParaboloid)
GENEVA_MATERIALIZE_CONFIGS(GGPUParaboloidOptimizer
	${INSTALL_PREFIX_DATA}/examples/21_GLoadableGPU/config
	--module $<TARGET_FILE:GGPUParaboloid>)

# Device-free module-ABI smoke test: the generic optimizer dlopen()s the module through the real loader path
# and optimizes the loaded individual on the CPU (--consumer stc uses the individual's own evaluate()). This
# exercises the module load (BOTH contributions -- individual + marshaller -- and the single shared
# serialization registry) without requiring a GPU; the on-device GPU run (--consumer gpu) needs a CUDA device
# and is verified separately.
#
# Registered unconditionally for the reason given in example 18's CMakeLists.txt. The GPU conditionality is
# handled entirely at CONFIGURE time and one level up: this whole directory is only added under
# GENEVA_GPU_CONSUMER_ENABLED (geneva/examples/CMakeLists.txt), so where the GPU consumer is not built the
# test simply does not exist. Where it IS built, the test needs no device -- deliberately, so that a machine
# with the GPU consumer compiled in but no usable device (a sandbox, a CI container) still gates the module
# ABI instead of reporting a device problem as an ABI failure.
ADD_TEST(
	NAME GLoadableGPU_CPU
	COMMAND GGPUParaboloidOptimizer --module $<TARGET_FILE:GGPUParaboloid> --consumer stc
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
SET_TESTS_PROPERTIES(GLoadableGPU_CPU PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GLoadableGPU_CPU S)

INSTALL(TARGETS GGPUParaboloidOptimizer GGPUParaboloid
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/21_GLoadableGPU)
INSTALL(DIRECTORY kernels
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/21_GLoadableGPU)
