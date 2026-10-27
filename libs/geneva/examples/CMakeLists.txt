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

SET(DEPENDENCIES
		GQuickStart
		GSimpleOptimizer
		GParaboloid2D
		GParameterObjectUsagePatterns
		GMultiCriterionParabola
		GDirectEA
		GDirectSA
		GDirectSwarm
		GFunctionMinimizer
		GPostProcessing
		GNeuralNetwork
		GMetaOptimizer
		GExternalEvaluator
		GPluggableOptimizationMonitors
		GDependentConstraints
		GAlgorithmChaining
		GGenericOptimizer)

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(DEPENDENCIES
			${DEPENDENCIES}
			GMPIConsumer
			GMPISubClients)
ENDIF()

IF(GENEVA_GPU_CONSUMER_ENABLED)
	SET(DEPENDENCIES
		${DEPENDENCIES}
		GCUDAWorker)
ENDIF()

ADD_SUBDIRECTORY( 00_GQuickStart )
ADD_SUBDIRECTORY( 01_GSimpleOptimizer )
# The generic optimizer (example 18) is the shared runtime driver the collapsed, load-from-disk examples
# (02/04/07, ...) run their problem modules on; it is configured first so those examples can depend on it
# (e.g. to materialize their configs from its code defaults at build time).
ADD_SUBDIRECTORY( 18_GLoadableIndividual )
ADD_SUBDIRECTORY( 20_GLoadableOA )
ADD_SUBDIRECTORY( 02_GParaboloid2D )
ADD_SUBDIRECTORY( 03_GParameterObjectUsagePatterns )
ADD_SUBDIRECTORY( 04_GMultiCriterionParabola )
ADD_SUBDIRECTORY( 06_GDirectEA )
ADD_SUBDIRECTORY( 06_GDirectSA )
ADD_SUBDIRECTORY( 06_GDirectSwarm )
ADD_SUBDIRECTORY( 07_GFunctionMinimizer )
ADD_SUBDIRECTORY( 08_GPostProcessing )
ADD_SUBDIRECTORY( 09_GNeuralNetwork )
ADD_SUBDIRECTORY( 10_GMetaOptimizer )
ADD_SUBDIRECTORY( 11_GExternalEvaluator )
ADD_SUBDIRECTORY( 12_GPluggableOptimizationMonitors )
ADD_SUBDIRECTORY( 13_GDependentConstraints )

# Example 14 (Mona-Lisa) drives the courtier GPU consumer, so it is built only when that consumer is
# (GENEVA_BUILD_WITH_GPU_CONSUMER -- the opt-in add-on folded into gemfony-courtier). Its kernel is
# runtime-compiled (NVRTC), so it needs no CUDA language at build time -- it builds with an
# ordinary C++ toolchain and selects CPU/CUDA at run time. It also requires libpng (its own
# CMakeLists skips the example if libpng is absent).
IF(GENEVA_GPU_CONSUMER_ENABLED)
	ADD_SUBDIRECTORY(14_GCUDAWorker)
	# Example 19: a stock GFunctionIndividual evaluated on the GPU purely via "--consumer gpu" -- the
	# store-based GPU path for an ordinary library individual. Needs no libpng (unlike example 14).
	ADD_SUBDIRECTORY(19_GFunctionGPU)
	# Example 21: the GPU North Star -- ONE loadable module carries both the individual and its GPU
	# marshaller; a generic optimizer loads it via --module and runs it on the device with --consumer gpu.
	ADD_SUBDIRECTORY(21_GLoadableGPU)
ENDIF()

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_SUBDIRECTORY( 15_GMPIConsumer )
	ADD_SUBDIRECTORY( 16_GMPISubClients )
ENDIF()

ADD_SUBDIRECTORY( 17_GAlgorithmChaining )

# ---------------------------------------------------------------------------------------------------
# Optional: refresh every shipped example config in place via the Go2 --update-configs switch.
#
# `make geneva_update_configs` runs each eligible example binary with --update-configs, which rewrites
# the configs in that binary's BUILD directory in canonical form (drop keys no registered parameter
# consumes, keep existing values, add newly-registered keys with their defaults). It touches ONLY the
# build tree; copying the refreshed configs back into the source tree is a deliberate, separate step
# (Development Invariant 17 -- a build never writes into the checkout).
#
# Only standard Go2 examples that (a) construct Go2 and reach optimize() and (b) build their individual
# without an external tool/service or a device are eligible. Deliberately NOT included, each for a
# STRUCTURAL reason (verified, not a convenience drop):
#   03_GParameterObjectUsagePatterns -- a parameter-object demo; it never constructs Go2, so the switch
#                                       never engages (its lone config is unaffected either way);
#   06_GDirect{EA,SA,Swarm}          -- drive an optimization algorithm directly, without Go2;
#   11_GExternalEvaluator            -- building its individual EXECUTES an external evaluator program;
#   14_GCUDAWorker / 19_GFunctionGPU -- need a CUDA device;
#   15_GMPIConsumer / 16_GMPISubClients -- need mpirun;
#   18_GLoadableIndividual           -- a plugin loader (needs --individual <plugin.so>).
# Those are refreshed by hand (or via their own run) when needed.
OPTION(GENEVA_UPDATE_CONFIGS
	"Add a 'geneva_update_configs' target that refreshes eligible example configs in place (build tree only) via Go2 --update-configs"
	OFF)

IF(GENEVA_UPDATE_CONFIGS)
	SET(GENEVA_UPDATE_CONFIG_TARGETS
			GQuickStart                    # 00
			GSimpleOptimizer               # 01
			GParaboloid2D                  # 02
			GMultiCriterionParabola        # 04
			GFunctionMinimizer             # 07
			GPostProcessing                # 08
			GNeuralNetwork                 # 09
			GMetaOptimizer                 # 10
			GPluggableOptimizationMonitors # 12
			GDependentConstraints          # 13
			GAlgorithmChaining)            # 17
	ADD_CUSTOM_TARGET(geneva_update_configs
			COMMENT "Refreshing eligible example configs in place via --update-configs (build tree only)")
	FOREACH(_gpb_target IN LISTS GENEVA_UPDATE_CONFIG_TARGETS)
		IF(TARGET ${_gpb_target})
			# Run the built binary in its own build directory (where its config/ was copied), so it
			# rewrites ./config/*.json there.
			ADD_CUSTOM_COMMAND(TARGET geneva_update_configs POST_BUILD
					COMMAND ${_gpb_target} --update-configs
					WORKING_DIRECTORY $<TARGET_FILE_DIR:${_gpb_target}>
					COMMENT "  ${_gpb_target} --update-configs"
					VERBATIM)
			ADD_DEPENDENCIES(geneva_update_configs ${_gpb_target})
		ENDIF()
	ENDFOREACH()
ENDIF()

# ---------------------------------------------------------------------------------------------------
# config-reference: (re)generate the single, version-controlled configuration REFERENCE tree in
# docs/config-reference/ from the code's registered parameters + comments. This is the one place a
# source-browser sees every standard configuration and what each parameter means, without building.
#
# The reference is generated (never hand-edited) by running a representative Go2 example with
# --update-configs, which materializes Go2.json, every registered optimization-algorithm config and the
# standard individual config. The GENEVA_CONFIG_REFERENCE environment variable suppresses the header's
# creation timestamp, so the output is byte-stable and regenerating it produces no spurious diff.
#
# Regenerating writes into the source tree -- the ONE deliberate exception to "a build never writes into
# the checkout" (Development Invariant 17), because a *documentation* artifact is meant to be versioned.
# It is therefore an explicit, opt-in target, NOT part of the normal build:
#   make config-reference         (re)generates docs/config-reference/ in place
#   make config-reference-check   regenerates to a temp dir and fails if it differs from the checked-in
#                                 tree (CI / pre-commit guard); never writes into the source tree
IF(TARGET GSimpleOptimizer)
	# The generated configs live in docs/config-reference/config/; the explanatory README.md sits beside it.
	# A cmake -P helper does the filesystem sequencing (create scratch dir, run the generator in it, then
	# copy-or-compare), which keeps the working-directory handling and the directory comparison portable.
	SET(_config_reference_dir ${CMAKE_SOURCE_DIR}/docs/config-reference/config)
	SET(_config_reference_drv ${CMAKE_CURRENT_SOURCE_DIR}/config_reference.cmake)

	ADD_CUSTOM_TARGET(config-reference
			COMMAND ${CMAKE_COMMAND}
					-DBIN=$<TARGET_FILE:GSimpleOptimizer> -DGEN=${CMAKE_CURRENT_BINARY_DIR}/config-reference-gen
					-DDEST=${_config_reference_dir} -DMODE=generate -P ${_config_reference_drv}
			COMMENT "Regenerating the configuration reference tree in docs/config-reference/config/"
			VERBATIM)
	ADD_DEPENDENCIES(config-reference GSimpleOptimizer)

	ADD_CUSTOM_TARGET(config-reference-check
			COMMAND ${CMAKE_COMMAND}
					-DBIN=$<TARGET_FILE:GSimpleOptimizer> -DGEN=${CMAKE_CURRENT_BINARY_DIR}/config-reference-check-gen
					-DDEST=${_config_reference_dir} -DMODE=check -P ${_config_reference_drv}
			COMMENT "Checking that docs/config-reference/config/ is up to date with the code"
			VERBATIM)
	ADD_DEPENDENCIES(config-reference-check GSimpleOptimizer)

	# These are opt-in maintenance targets (config-reference regenerates a version-controlled tree); keep
	# them out of the aggregate build target so a normal build never runs them.
	SET_TARGET_PROPERTIES(config-reference config-reference-check
			PROPERTIES GENEVA_EXCLUDE_FROM_AGGREGATE TRUE)
ENDIF()

GENEVA_AGGREGATE_TARGET( "examples-geneva" )