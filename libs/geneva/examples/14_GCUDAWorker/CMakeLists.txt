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

# Example 14 -- the Mona-Lisa problem, evaluated through the unified courtier GPU consumer (folded into
# gemfony-courtier when GEM_BUILD_WITH_GPU_CONSUMER is ON, which gates this whole example -- see
# geneva/examples/CMakeLists.txt). Unlike the old example 14 it does NOT enable the CUDA language: the kernel
# is runtime-compiled by NVRTC at run time, so the example builds with an ordinary C++ toolchain and
# selects CPU / CUDA at run time via config/GGPUConsumer.json. It only needs libpng to load
# the target image and write result pictures; if libpng is missing the example is skipped.

# GIMAGE_USE_FLOAT (float or double parameters and GPU kernel) is an option of the top-level
# CMakeLists.txt.

FIND_PACKAGE ( PNG QUIET )
IF ( NOT PNG_FOUND )
	MESSAGE ( STATUS "Example 14 (GCUDAWorker): libpng not found -- skipping" )
	RETURN ()
ENDIF ()

# Headers, listed for CLion indexing.
SET ( MONALISAINCLUDES
	GImageIndividual.hpp
	GImageHelperFunctions.hpp
	GImagePOM.hpp
	GImageScalar.hpp
	GMonaLisaProblem.hpp
	GMonaLisaGPUMarshaller.hpp
)
ADD_CUSTOM_TARGET ( CLION_EXAMPLE15_HEADERS SOURCES ${MONALISAINCLUDES} )

# The compile-time scalar selection (float default, double when GIMAGE_USE_FLOAT is OFF) must be identical
# for the module and the launcher: the launcher's parity check + GPU-config materialization construct a
# GGPUConsumerT<..., gimage_fp_t> that must match the marshaller the module contributes. Applied to both
# targets below.
IF ( GIMAGE_USE_FLOAT )
	MESSAGE ( STATUS "Example 14 (GCUDAWorker): building with FLOAT parameters + the float GPU kernel (default)" )
ELSE ()
	MESSAGE ( STATUS "Example 14 (GCUDAWorker): building with DOUBLE parameters + the double GPU kernel (GIMAGE_USE_FLOAT=OFF)" )
ENDIF ()

# (1) The loadable GPU-problem module -> libGImageIndividual.so. It carries the Mona-Lisa individual, its GPU
# marshaller, the individual's archive registration (GEM_REGISTER_ARCHIVABLE lives in the individual's
# own hpp/cpp) and a TWO-contribution manifest entry point (GImageIndividualPlugin.cpp) -- the same
# individual-plus-marshaller module shape as the GPU North Star (example 21). It links none of the Geneva
# libraries (their symbols resolve from the host process at load via RTLD_GLOBAL) and needs no libpng: the
# individual's evaluate() scores against the process-wide target and never touches PNG I/O.
GENEVA_ADD_INDIVIDUAL_MODULE ( GImageIndividual GImageIndividual.cpp GImageIndividualPlugin.cpp )
IF ( GIMAGE_USE_FLOAT )
	TARGET_COMPILE_DEFINITIONS ( GImageIndividual PRIVATE GIMAGE_USE_FLOAT )
ENDIF ()

# (2) The thin program -> loads the problem module at runtime. It names only the flat-genome base and the
# loaded factory; the image I/O (target load + PNG output via GImageHelperFunctions.cpp) and the host-side
# render math stay here (a generic optimizer cannot provide the image POM / target). ENABLE_EXPORTS so a
# module loaded RTLD_GLOBAL resolves against one symbol namespace (single archive registry +
# single Geneva singletons, incl. the shared MonaLisa target store).
SET ( EXECUTABLENAME GCUDAWorker )
ADD_EXECUTABLE ( ${EXECUTABLENAME}
	GCUDAWorker.cpp
	GImageHelperFunctions.cpp
)
SET_TARGET_PROPERTIES ( ${EXECUTABLENAME} PROPERTIES ENABLE_EXPORTS TRUE )
TARGET_INCLUDE_DIRECTORIES ( ${EXECUTABLENAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} )
IF ( GIMAGE_USE_FLOAT )
	TARGET_COMPILE_DEFINITIONS ( ${EXECUTABLENAME} PRIVATE GIMAGE_USE_FLOAT )
ENDIF ()

# The float or double build changes what the program sends, which the commit does not see: it is
# part of the program's identity, so that a client of the other build ends instead of failing to
# read every item (GEM_STAMP_PROGRAM, CMakeModules/GemProgramStamp.cmake).
IF ( GIMAGE_USE_FLOAT )
	GEM_STAMP_PROGRAM ( ${EXECUTABLENAME} IDENTITY "GCUDAWorker-float" )
ELSE ()
	GEM_STAMP_PROGRAM ( ${EXECUTABLENAME} IDENTITY "GCUDAWorker-double" )
ENDIF ()

# The Geneva libraries (the GPU consumer + its CUDA backend are folded into gemfony-courtier
# when GEM_BUILD_WITH_GPU_CONSUMER is ON -- which is required for this example to be built at all,
# see geneva/examples/CMakeLists.txt), Boost and libpng.
TARGET_LINK_LIBRARIES ( ${EXECUTABLENAME} PRIVATE
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
	PNG::PNG
)

# Runtime assets. The kernel is runtime-loaded source, so copy it at BUILD time (a POST_BUILD command)
# -- editing the kernel + rebuilding then re-stages it, instead of the stale copy a configure-time
# FILE(COPY) would leave. The target images and config rarely change, so a configure-time copy is fine.
ADD_CUSTOM_COMMAND ( TARGET ${EXECUTABLENAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/kernels ${CMAKE_CURRENT_BINARY_DIR}/kernels )
FILE ( COPY ${CMAKE_CURRENT_SOURCE_DIR}/pictures DESTINATION ${CMAKE_CURRENT_BINARY_DIR} )

# Materialize the configuration from code defaults at build time and overlay this example's overrides
# (config/config-overrides.json: consumer=gpu and the GPU-consumer kernel paths). The individual's and GPU
# consumer's configs only exist once the module is loaded, so materialize with the just-built module (and
# depend on it so it exists first). This build-time run also loads the module under RTLD_NOW, so it doubles
# as the smoke check that both contributions (individual + marshaller) resolve.
ADD_DEPENDENCIES ( ${EXECUTABLENAME} GImageIndividual )
GEM_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/14_GCUDAWorker/config
	--module $<TARGET_FILE:GImageIndividual> )

# Device-free end-to-end test: the launcher loads the module and optimizes the loaded individual on the CPU
# (--consumer stc uses the individual's own evaluate()). This exercises the module load (individual +
# marshaller contributions, single shared Boost registry) without requiring a GPU; the on-device GPU run
# (--consumer gpu) needs a CUDA device and is verified separately.
#
# --eaMaxIterations bounds the run to two generations: the shipped configuration deliberately has no
# iteration cap (the point of the example is to keep refining the picture until it stalls), and a
# single Mona-Lisa evaluation is expensive enough that the default run takes minutes. Two generations
# still cover the whole path -- module load, adapt, evaluate, select -- which is what this test is for
# (Inv 22). The budget lives on the command line rather than in config-overrides.json so the SHIPPED
# example keeps producing a picture.
ADD_TEST (
	NAME GCUDAWorker_CPU
	COMMAND ${EXECUTABLENAME} --module $<TARGET_FILE:GImageIndividual> --consumer stc --eaMaxIterations 2
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
SET_TESTS_PROPERTIES ( GCUDAWorker_CPU PROPERTIES LABELS "geneva;example" TIMEOUT 300 )
GEM_TEST_SIZE(GCUDAWorker_CPU L)

# The individual on its own, without a target image or a device: what its configuration makes of a
# produced genome (start_size).
IF ( GEM_BUILD_TESTS )
	ADD_EXECUTABLE ( GImageIndividualTests GImageIndividualTests.cpp GImageIndividual.cpp )
	TARGET_INCLUDE_DIRECTORIES ( GImageIndividualTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} )
	IF ( GIMAGE_USE_FLOAT )
		TARGET_COMPILE_DEFINITIONS ( GImageIndividualTests PRIVATE GIMAGE_USE_FLOAT )
	ENDIF ()
	TARGET_LINK_LIBRARIES ( GImageIndividualTests PRIVATE
		${GENEVA_LIBRARIES} ${Boost_LIBRARIES} Catch2::Catch2WithMain )
	ADD_TEST ( NAME GImageIndividualTests COMMAND GImageIndividualTests
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
	SET_TESTS_PROPERTIES ( GImageIndividualTests PROPERTIES LABELS "geneva;example" )
	GEM_TEST_SIZE ( GImageIndividualTests S )
ENDIF ()

# A networked run: a server and three clients, all of them ranks of the MPI consumer (the launcher
# that starts the processes is the only infrastructure this needs, so the test exists only where the
# MPI consumer is built). Each client evaluates against the target image it loaded itself; an
# evaluation without a target throws, and a best fitness of exactly 0 -- what an empty target
# scores -- fails the test as well. One generation covers the path (Inv 22); three clients rather
# than one keep a Debug run of it below a minute.
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	GEM_ADD_MPI_TEST (
		NAME GCUDAWorker_NetworkedClient
		TIMEOUT 300
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4
			${MPIEXEC_PREFLAGS} $<TARGET_FILE:${EXECUTABLENAME}> ${MPIEXEC_POSTFLAGS}
			--module $<TARGET_FILE:GImageIndividual> --consumer mpi --eaMaxIterations 1
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	)
	SET_TESTS_PROPERTIES ( GCUDAWorker_NetworkedClient PROPERTIES
		LABELS "geneva;example" PROCESSORS 4
		FAIL_REGULAR_EXPRESSION "no target image is loaded;Best fitness: 0[\r\n]" )
	GEM_TEST_SIZE(GCUDAWorker_NetworkedClient L)
ENDIF ()

INSTALL ( TARGETS ${EXECUTABLENAME} GImageIndividual
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/14_GCUDAWorker )
INSTALL ( DIRECTORY kernels pictures
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/14_GCUDAWorker )
