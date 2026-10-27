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

# Multi-algorithm GPU optimisation benchmark. It is evaluated through the SAME unified courtier GPU
# consumer (folded into gemfony-courtier when GENEVA_BUILD_WITH_GPU_CONSUMER is ON) that example 14 uses
# -- a whole population scored in one bulk,
# runtime-compiled kernel launch. The kernel (kernels/benchmark_eval.cu via NVRTC)
# is loaded at run time, so this target no longer enables the CUDA language at build time: it builds
# with an ordinary C++ toolchain and the backend (cpu/cuda) is chosen in config/GGPUConsumer.json.

SET ( EXECUTABLENAME GCUDAOptBenchmark )

ADD_EXECUTABLE ( ${EXECUTABLENAME}
	GCUDAOptBenchmarkMain.cpp
	GAlgorithmBenchmarkRunner.cpp
	GBenchmarkResultWriter.cpp
)

TARGET_INCLUDE_DIRECTORIES ( ${EXECUTABLENAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} )

# The Geneva libraries (the GPU consumer + its CUDA backend + the libstdc++ ABI fix are folded
# into gemfony-courtier when GENEVA_BUILD_WITH_GPU_CONSUMER is ON -- required for this benchmark to be
# built at all, see geneva/benchmarks/CMakeLists.txt) and Boost.
SET ( LIBRARIES_TO_LINK
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
	# GFunctionIndividual is no longer in the geneva library; compiled in via its object library.
	GFunctionIndividual-obj
)
IF ( GENEVA_BUILD_WITH_MPI_CONSUMER )
	SET ( LIBRARIES_TO_LINK ${LIBRARIES_TO_LINK} ${MPI_LIBRARIES} )
ENDIF ()

TARGET_LINK_LIBRARIES ( ${EXECUTABLENAME} PRIVATE ${LIBRARIES_TO_LINK} )

# CLion workaround: expose headers as a custom target so the IDE indexes them.
SET ( GBENCHMARK_HEADERS
	GBenchmarkGPUMarshaller.hpp
	GBenchmarkRunResult.hpp
	GAlgorithmBenchmarkRunner.hpp
	GBenchmarkResultWriter.hpp
)
ADD_CUSTOM_TARGET ( CLION_GBENCHMARKCUDA_HEADERS SOURCES ${GBENCHMARK_HEADERS} )

# Runtime assets. The kernels are runtime-loaded source, so copy them at BUILD time (POST_BUILD) --
# editing a kernel + rebuilding then re-stages it, instead of the stale copy a configure-time
# FILE(COPY) would leave. The config rarely changes, so a configure-time copy is fine for it.
ADD_CUSTOM_COMMAND ( TARGET ${EXECUTABLENAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/kernels ${CMAKE_CURRENT_BINARY_DIR}/kernels )

# Materialize the configuration from code defaults at build time and overlay this benchmark's overrides
# (config/config-overrides.json: the GPU-consumer kernel path), then install it.
GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/benchmarks/${EXECUTABLENAME}/config )

# A throughput-scale run: it repeats the same optimization on each available backend and compares them,
# so it takes minutes, not seconds -- 1372 s measured on the development host. Registered like every
# other test (this whole directory already exists only in a GPU-consumer build, which is the real,
# configure-time-detected dependency); what keeps it out of the development loop is its measured size
# (XL), not a flag.
#
# The TIMEOUT is ~3x that measurement. The former 2500 s was only 1.8x, which is not enough margin for
# a wall-clock comparison of BACKENDS: a slower or busier machine, or one whose GPU is shared, would
# have tripped the clock and reported a defect that is not there.
ADD_TEST ( ${EXECUTABLENAME} ${EXECUTABLENAME} )
SET_TESTS_PROPERTIES ( ${EXECUTABLENAME} PROPERTIES LABELS "benchmark" TIMEOUT 4500 )
GENEVA_TEST_SIZE(${EXECUTABLENAME} XL)

INSTALL ( TARGETS ${EXECUTABLENAME}
	DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/${EXECUTABLENAME} )
INSTALL ( DIRECTORY kernels
	DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/${EXECUTABLENAME} )
