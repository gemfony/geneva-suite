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

SET ( OPTIMIZATIONSOURCES
	Interface/GRateableI.cpp
	GenevaHelperFunctions.cpp
	GConsumerSetup.cpp
	GFaultInjector.cpp
	GenevaInitializer.cpp
	Go2.cpp
    oa/GBasePluggableOM.cpp
    oa/GPluggableOptimizationMonitors.cpp
    oa/GPostProcessorT.cpp
    oa/GOptimizationAlgorithmBase.cpp
    oa/GBuiltinAlgorithms.cpp
    oa/GLineSearch.cpp
    oa/GHesseError.cpp
    oa/GEvolutionaryAlgorithm.cpp
    oa/GEvolutionaryAlgorithm_PersonalityTraits.cpp
    oa/GMetaEvolutionaryAlgorithm.cpp
    oa/GParChild.cpp
    oa/GBaseParChildPersonalityTraits.cpp
    oa/GPositionPersonalityTraits.cpp
    oa/GSepCmaEvolutionStrategy.cpp
    oa/GSepCmaEvolutionStrategy_PersonalityTraits.cpp
    oa/GStandardPSO2011.cpp
    oa/GStandardPSO2011_PersonalityTraits.cpp
    oa/GAntColonyOptimization.cpp
    oa/GAntColonyOptimization_PersonalityTraits.cpp
    oa/GGeneralizedSimulatedAnnealing.cpp
    oa/GGeneralizedSimulatedAnnealing_PersonalityTraits.cpp
	GModuleLoader.cpp
	genome/GParameterPropertyParser.cpp
	genome/GIndividualProcessingResult.cpp
	genome/GProblemPolicy.cpp
	genome/GGenome.cpp
	genome/GGenomeFixedSizePriorityQueue.cpp
	genome/GGenomeMultiConstraint.cpp
	genome/GIndividualMultiConstraint.cpp
	GPersonalityTraits.cpp
)

# GTestIndividual1 is a LIBRARY-INTERNAL test fixture: GEvolutionaryAlgorithm's GEM_TESTING self-test
# (fillWithObjects) instantiates it. It is therefore compiled into the library, but ONLY under
# GENEVA_BUILD_TESTS (-DGEM_TESTING) -- a production build omits it, so libgemfony-geneva still ships zero
# concrete individuals. Its GEM_TESTING-only test consumers (GenevaStandardTests) link the library and get it
# from there. GTestIndividual2 and GTestIndividual3 are NOT used by the library; they are packaged as object
# libraries below.
IF(GENEVA_BUILD_TESTS)
	SET(OPTIMIZATIONSOURCES ${OPTIMIZATIONSOURCES} individuals/GTestIndividual1.cpp)
ENDIF()

# The reusable sample individuals are packaged, NOT compiled into the geneva library (DA1: libgemfony-geneva
# ships zero concrete GGenome subclasses). Each MODE-both declaration builds an OBJECT library
# <name>-obj (linked by the individual's compile-in consumers -- examples / tests / benchmarks) and a
# runtime-loadable module lib<name>.so (loaded by the generic optimizer via --individual). One
# GEM_REGISTER_ARCHIVABLE (in the individual's own .cpp) serves both; each module-only glue TU carries the
# extern "C" manifest entry point.
#! [modules-cmake-declare]
GENEVA_DECLARE_INDIVIDUAL(GLineFitIndividual
	MODE    both
	SOURCES individuals/GLineFitIndividual.cpp
	PLUGIN  individuals/GLineFitIndividualPlugin.cpp
	CONFIG  ./config/GLineFitIndividual.json)
#! [modules-cmake-declare]

GENEVA_DECLARE_INDIVIDUAL(GNeuralNetworkIndividual
	MODE    both
	SOURCES individuals/GNeuralNetworkIndividual.cpp
	PLUGIN  individuals/GNeuralNetworkIndividualPlugin.cpp
	CONFIG  ./config/GNeuralNetworkIndividual.json)

GENEVA_DECLARE_INDIVIDUAL(GExternalEvaluatorIndividual
	MODE    both
	SOURCES individuals/GExternalEvaluatorIndividual.cpp
	PLUGIN  individuals/GExternalEvaluatorIndividualPlugin.cpp
	CONFIG  ./config/GExternalEvaluatorIndividual.json)

# GDelayIndividual is a pure benchmarking / testing individual (it performs no real optimisation -- its
# fitness just sleeps, its genome is transport ballast) and deliberately owns its own config parsing and
# construction (readConfig / create) instead of going through GIndividualFactory. There is nothing to
# load into the generic optimizer, so it is MODE compile: an object library its benchmark / test consumers
# link, but no runtime-loadable module. (Maintainer-confirmed 2026-07-06: compile-in is fine here.)
GENEVA_DECLARE_INDIVIDUAL(GDelayIndividual
	MODE    compile
	SOURCES individuals/GDelayIndividual.cpp)

GENEVA_DECLARE_INDIVIDUAL(GFunctionIndividual
	MODE    both
	SOURCES individuals/GFunctionIndividual.cpp
	PLUGIN  individuals/GFunctionIndividualPlugin.cpp
	CONFIG  ./config/GFunctionIndividual.json)

# GMetaOptimizerIndividualT is the meta-optimization umbrella individual (example 10). It is a class template
# whose default and only exported instantiation is GMetaOptimizerIndividualT<GFunctionIndividual>, so it needs
# GFunctionIndividual at compile and link time and must never be loaded (loading a meta-umbrella individual is
# meaningless). Hence MODE compile: an object library its consumers link, no module. A consumer that links this
# object library must ALSO link GFunctionIndividual-obj (its instantiation's symbols); the two meta consumers
# (example 10 and the unit tests) do so explicitly.
GENEVA_DECLARE_INDIVIDUAL(GMetaOptimizerIndividualT
	MODE    compile
	SOURCES individuals/GMetaOptimizerIndividualT.cpp)

# GTestIndividual2 / GTestIndividual3 are serialization / auxiliary-store test fixtures used ONLY by external
# test and benchmark targets (never by the library). They are packaged MODE compile -- an object library each
# consumer links -- and built only when something can consume them: GTestIndividual2 by the unit tests and the
# GSerializationOverhead / GRandomWalk benchmarks-and-manual-tests; GTestIndividual3 by the unit tests only.
IF(GENEVA_BUILD_TESTS OR GENEVA_BUILD_BENCHMARKS)
	GENEVA_DECLARE_INDIVIDUAL(GTestIndividual2
		MODE    compile
		SOURCES individuals/GTestIndividual2.cpp)
ENDIF()
IF(GENEVA_BUILD_TESTS)
	GENEVA_DECLARE_INDIVIDUAL(GTestIndividual3
		MODE    compile
		SOURCES individuals/GTestIndividual3.cpp)
ENDIF()

# Add MPI specific files
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			GMPISubClientIndividual.cpp)
ENDIF()

# Add MPI specific files
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			GMPISubClientOptimizer.cpp)
ENDIF()

################################################################################
#
# Build-time algorithm selection ("slim core").
#
# Each option below gates the three source files of one optimization algorithm.
# All default to ON, so the stock build is byte-for-byte identical to before:
# every algorithm is compiled in and its factory is registered with the global
# optimization-algorithm store. Setting an option to OFF omits that algorithm's
# sources AND its GENEVA_ALGO_* compile definition, so the single registration
# translation unit (oa/GBuiltinAlgorithms.cpp) leaves it out too and its
# mnemonic simply disappears from the runtime selection (the '-a' option).
#
# Evolutionary algorithms (EA) are intentionally NOT optional and remain in the
# core source list above: the EA factory instantiation is referenced directly
# by the post-processor (GPostProcessorT) and the meta-optimizer individual
# (GMetaOptimizerIndividualT), both of which are always built. The shared
# parent-child base (GParChild / GBaseParChildPersonalityTraits, used by EA and
# SA) likewise stays in core.
#
# Consumers are not gated here: the courtier consumers are header-only
# templates with no .cpp to exclude, and the MPI consumer is already gated by
# GENEVA_BUILD_WITH_MPI_CONSUMER. Consumer plugin selection is deferred to a
# later plugin-architecture phase.
#
################################################################################

OPTION(GENEVA_ALGO_SA    "Build the Simulated Annealing algorithm"         ON)
OPTION(GENEVA_ALGO_SWARM "Build the Swarm algorithm"                       ON)
OPTION(GENEVA_ALGO_CGD   "Build the Conjugate Gradient Descent algorithm"  ON)
OPTION(GENEVA_ALGO_NM    "Build the Nelder-Mead algorithm"                 ON)
OPTION(GENEVA_ALGO_PS    "Build the Parameter Scan algorithm"              ON)

# Slim-core builds and the test suite are mutually exclusive. The in-library
# unit-test fixtures (notably GTestIndividual1) and the algorithms' own
# GEM_TESTING self-tests reference the optimization algorithms by type, so a
# testing-enabled build needs every algorithm compiled in. Disabling any
# algorithm while GENEVA_BUILD_TESTS is ON would otherwise fail at link time
# with a cryptic undefined-reference error; catch it here with a clear message.
# (Production slim-core builds run with GENEVA_BUILD_TESTS=OFF anyway.)
IF (GENEVA_BUILD_TESTS AND NOT (GENEVA_ALGO_SA AND GENEVA_ALGO_SWARM
		AND GENEVA_ALGO_CGD AND GENEVA_ALGO_NM
		AND GENEVA_ALGO_PS))
	MESSAGE(FATAL_ERROR
		"GENEVA_BUILD_TESTS=ON requires all GENEVA_ALGO_* options to be ON: the "
		"in-library test fixtures reference every optimization algorithm. For a "
		"slim-core build with an algorithm disabled, configure with "
		"-DGENEVA_BUILD_TESTS=OFF.")
ENDIF ()

IF(GENEVA_ALGO_SA)
	SET(GENEVA_ALGO_DEFINES ${GENEVA_ALGO_DEFINES} GENEVA_ALGO_SA)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			oa/GSimulatedAnnealing.cpp
			oa/GSimulatedAnnealing_PersonalityTraits.cpp)
ENDIF()

IF(GENEVA_ALGO_SWARM)
	SET(GENEVA_ALGO_DEFINES ${GENEVA_ALGO_DEFINES} GENEVA_ALGO_SWARM)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			oa/GSwarmAlgorithm.cpp
			oa/GSwarmAlgorithm_PersonalityTraits.cpp)
ENDIF()

IF(GENEVA_ALGO_CGD)
	SET(GENEVA_ALGO_DEFINES ${GENEVA_ALGO_DEFINES} GENEVA_ALGO_CGD)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			oa/GConjugateGradientDescent.cpp
			oa/GConjugateGradientDescent_PersonalityTraits.cpp)
ENDIF()

IF(GENEVA_ALGO_NM)
	SET(GENEVA_ALGO_DEFINES ${GENEVA_ALGO_DEFINES} GENEVA_ALGO_NM)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			oa/GNelderMead.cpp
			oa/GNelderMead_PersonalityTraits.cpp)
ENDIF()

IF(GENEVA_ALGO_PS)
	SET(GENEVA_ALGO_DEFINES ${GENEVA_ALGO_DEFINES} GENEVA_ALGO_PS)
	SET(OPTIMIZATIONSOURCES
			${OPTIMIZATIONSOURCES}
			oa/GParameterScan.cpp
			oa/GParameterScanFactory.cpp
			oa/GParameterScan_PersonalityTraits.cpp)
ENDIF()

ADD_LIBRARY ( ${GENEVA_LIBNAME} SHARED ${OPTIMIZATIONSOURCES} )

# The optional algorithms' switches, handed to the compiler. Only the single registration translation
# unit (oa/GBuiltinAlgorithms.cpp) reads them: it registers exactly the algorithms whose sources the
# GENEVA_ALGO_* options above put into the library, so a slim-core build cannot reference an algorithm
# it did not compile.
TARGET_COMPILE_DEFINITIONS ( ${GENEVA_LIBNAME} PRIVATE ${GENEVA_ALGO_DEFINES} )

SET_TARGET_PROPERTIES(
    ${GENEVA_LIBNAME}
    PROPERTIES
    VERSION ${GENEVA_VERSION}
	EXPORT_NAME geneva
)



# Export the include directory. The C++ standard is selected centrally in
# CMakeModules/CommonGenevaBuild.cmake.
TARGET_INCLUDE_DIRECTORIES(
	${GENEVA_LIBNAME}
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/projects/geneva/include>
	$<INSTALL_INTERFACE:include>
)

# Inter-library dependency: geneva builds on courtier (and transitively on hap
# and common). PUBLIC so the full include/Boost interface propagates to
# downstream consumers that link Geneva::geneva.
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PUBLIC ${COURTIER_LIBNAME})

# geneva's pluggable monitors render through the dietrich plotting library.
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PUBLIC ${DIETRICH_LIBNAME})

# The runtime module loader (GModuleLoader) uses Boost.DLL, which needs the dynamic
# loading library (libdl) on platforms that provide it. Boost.DLL is otherwise header-only, so
# libdl is its only link-time requirement here.
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PRIVATE ${CMAKE_DL_LIBS})

IF ( GENEVA_BUILD_TESTS )
	# When built with testing support (-DGEM_TESTING), the library embeds
	# Catch2-based test scaffolding (GTestIndividual*, GUnitTestFrameworkT) and
	# therefore references Catch2 symbols. Consumers of such a testing-enabled
	# install must link Catch2 too, so the requirement is propagated through the
	# public interface (both in-tree and exported). The installed config package
	# re-finds Catch2 accordingly (see GenevaConfig.cmake.in / GENEVA_TESTING).
	TARGET_LINK_LIBRARIES(
		${GENEVA_LIBNAME}
		INTERFACE
		Catch2::Catch2
	)
ENDIF ()

IF (GENEVA_BUILD_WITH_MPI_CONSUMER)
	TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PRIVATE MPI::MPI_CXX)
ENDIF ()

INSTALL ( TARGETS ${GENEVA_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )

