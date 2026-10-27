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

# Example 19 -- a stock GFunctionIndividual (the standard benchmark-function individual) evaluated on the
# GPU purely by selecting "--consumer gpu". It demonstrates the store-based GPU path for an ORDINARY
# library individual: the problem contributes only a marshaller (deriving Gem::Geneva::GBaseGPUMarshallerT)
# and Go2 builds the generic GPU consumer around it. Like example 14 it drives the courtier GPU consumer,
# so it is built only when that consumer is (GENEVA_BUILD_WITH_GPU_CONSUMER -- see
# geneva/examples/CMakeLists.txt). The kernel is runtime-compiled by NVRTC, so no CUDA language is enabled
# at build time -- it builds with an ordinary C++ toolchain and selects CPU / CUDA at run time via
# config/GGPUConsumer.json.

# Headers, listed for CLion indexing.
SET ( GFUNCTIONGPUINCLUDES
	GFunctionGPUMarshaller.hpp
)
ADD_CUSTOM_TARGET ( CLION_EXAMPLE20_HEADERS SOURCES ${GFUNCTIONGPUINCLUDES} )

SET ( EXECUTABLENAME GFunctionGPU )

ADD_EXECUTABLE ( ${EXECUTABLENAME}
	GFunctionGPUMain.cpp
)

TARGET_INCLUDE_DIRECTORIES ( ${EXECUTABLENAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} )

# The Geneva libraries (the GPU consumer + its CUDA backend are folded into gemfony-courtier when
# GENEVA_BUILD_WITH_GPU_CONSUMER is ON -- required for this example to be built at all) and Boost.
# GFunctionIndividual is no longer in the geneva library; it is compiled in via its object library (before
# the Geneva/Boost libraries, the in-tree convention for an object library carrying archive registrations).
TARGET_LINK_LIBRARIES ( ${EXECUTABLENAME} PRIVATE
	GFunctionIndividual-obj
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
)

# Runtime assets. The kernel is runtime-loaded source, so copy it at BUILD time (a POST_BUILD command) --
# editing the kernel + rebuilding then re-stages it, instead of the stale copy a configure-time FILE(COPY)
# would leave.
ADD_CUSTOM_COMMAND ( TARGET ${EXECUTABLENAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/kernels ${CMAKE_CURRENT_BINARY_DIR}/kernels )

# Materialize the configuration from code defaults at build time and overlay this example's overrides
# (config/config-overrides.json: consumer=gpu and the GPU-consumer kernel path), then install it.
GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/19_GFunctionGPU/config )

INSTALL ( TARGETS ${EXECUTABLENAME}
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/19_GFunctionGPU )
INSTALL ( DIRECTORY kernels
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/19_GFunctionGPU )
