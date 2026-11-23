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
	GFaultInjector.cpp
	GenevaInitializer.cpp
	Go2.cpp
	GOptimizationDescription.cpp
    oa/GBasePluggableOM.cpp
    oa/GMonitorStream.cpp
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
    oa/GSimulatedAnnealing.cpp
    oa/GSimulatedAnnealing_PersonalityTraits.cpp
    oa/GSwarmAlgorithm.cpp
    oa/GSwarmAlgorithm_PersonalityTraits.cpp
    oa/GConjugateGradientDescent.cpp
    oa/GConjugateGradientDescent_PersonalityTraits.cpp
    oa/GNelderMead.cpp
    oa/GNelderMead_PersonalityTraits.cpp
    oa/GParameterScan.cpp
    oa/GParameterScanFactory.cpp
    oa/GParameterScan_PersonalityTraits.cpp
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
# GEM_BUILD_TESTS (-DGEM_TESTING) -- a production build omits it, so libgemfony-geneva still ships zero
# concrete individuals. Its GEM_TESTING-only test consumers (GenevaStandardTests) link the library and get it
# from there. GTestIndividual2 and GTestIndividual3 are NOT used by the library; they are packaged as object
# libraries below.
IF(GEM_BUILD_TESTS)
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
IF(GEM_BUILD_TESTS OR GEM_BUILD_BENCHMARKS)
	GENEVA_DECLARE_INDIVIDUAL(GTestIndividual2
		MODE    compile
		SOURCES individuals/GTestIndividual2.cpp)
ENDIF()
IF(GEM_BUILD_TESTS)
	GENEVA_DECLARE_INDIVIDUAL(GTestIndividual3
		MODE    compile
		SOURCES individuals/GTestIndividual3.cpp)
ENDIF()

ADD_LIBRARY ( ${GENEVA_LIBNAME} SHARED ${OPTIMIZATIONSOURCES} )

SET_TARGET_PROPERTIES(
    ${GENEVA_LIBNAME}
    PROPERTIES
    VERSION ${GEM_VERSION}
	EXPORT_NAME geneva
)



# Export the include directory. The C++ standard is selected centrally in
# CMakeModules/CommonGemBuild.cmake.
TARGET_INCLUDE_DIRECTORIES(
	${GENEVA_LIBNAME}
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libs/geneva/include>
	$<INSTALL_INTERFACE:include>
)

# Inter-library dependency: geneva builds on courtier, hap, dietrich (below) and common (through
# each of them). hap is linked here, not through courtier, which does not depend on it. PUBLIC so
# the full include/Boost interface propagates to downstream consumers that link Geneva::geneva.
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PUBLIC ${COURTIER_LIBNAME} ${HAP_LIBNAME})

# geneva's pluggable monitors render through the dietrich plotting library.
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PUBLIC ${DIETRICH_LIBNAME})

# consumerModules() finds this library's own path with Boost.DLL (boost::dll::this_line_location()):
# Geneva's consumer modules lie next to it. Boost.DLL needs the dynamic loading
# library (libdl) on platforms that provide it; it is otherwise header-only, so libdl is its only
# link-time requirement here. (The module loader itself is common's.)
TARGET_LINK_LIBRARIES(${GENEVA_LIBNAME} PRIVATE ${CMAKE_DL_LIBS})

IF ( GEM_BUILD_TESTS )
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

INSTALL ( TARGETS ${GENEVA_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )

################################################################################
# Geneva's consumers: one module per consumer, built for Geneva's item type GGenome exactly like a
# module a third party would write (one line on courtier's provider each), and named
# libgemfony-geneva-<consumer>.so. They lie in the same directory as libgemfony-geneva, in the build
# tree as in the installation, which is where the launcher looks (consumerModules()); a consumer
# exists exactly when its module does, and only the chosen one is loaded. stc is the default. The MPI
# module needs MPI, a library not every machine has: where it is missing, the module cannot be
# loaded and a run that asks for "mpi" is refused with the reason; every other run starts.
SET ( _GENEVA_CONSUMERS stc beast )
SET ( _GENEVA_CONSUMER_SOURCE_stc GStcConsumerModule.cpp )
SET ( _GENEVA_CONSUMER_SOURCE_beast GBeastConsumerModule.cpp )
IF ( GEM_MQTT_CONSUMER_ENABLED )
	LIST ( APPEND _GENEVA_CONSUMERS mqtt )
	SET ( _GENEVA_CONSUMER_SOURCE_mqtt GMqttConsumerModule.cpp )
ENDIF ()
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	LIST ( APPEND _GENEVA_CONSUMERS mpi )
	SET ( _GENEVA_CONSUMER_SOURCE_mpi GMPIConsumerModule.cpp )
ENDIF ()
IF ( GEM_GPU_CONSUMER_ENABLED )
	LIST ( APPEND _GENEVA_CONSUMERS gpu )
	SET ( _GENEVA_CONSUMER_SOURCE_gpu GGPUConsumerModule.cpp )
ENDIF ()
# Every program that links Geneva is built after the modules (an interface library's dependencies
# are followed by whatever links it); in-tree only, an installation carries the modules.
ADD_LIBRARY ( ${GENEVA_LIBNAME}-consumer-modules INTERFACE )
TARGET_LINK_LIBRARIES ( ${GENEVA_LIBNAME} INTERFACE
	$<BUILD_INTERFACE:${GENEVA_LIBNAME}-consumer-modules> )
FOREACH ( _consumer ${_GENEVA_CONSUMERS} )
	SET ( _module ${GENEVA_LIBNAME}-${_consumer} )
	# A MODULE: loaded, never linked, so it is not part of the exported package. It keeps the "lib"
	# prefix, which MODULE libraries drop by default.
	ADD_LIBRARY ( ${_module} MODULE ${_GENEVA_CONSUMER_SOURCE_${_consumer}} )
	SET_TARGET_PROPERTIES ( ${_module} PROPERTIES PREFIX "lib" )
	# Like a loadable individual module, it links no Geneva library -- the process that loads it
	# already carries libgemfony-geneva, and a testing build's Geneva would hand the static Catch2 on
	# to a shared library that cannot take it. COMPILE_ONLY takes Geneva's include paths and compile
	# settings without linking.
	TARGET_LINK_LIBRARIES ( ${_module} PRIVATE $<COMPILE_ONLY:${GENEVA_LIBNAME}> )
	ADD_DEPENDENCIES ( ${_module} ${GENEVA_LIBNAME} )
	ADD_DEPENDENCIES ( ${GENEVA_LIBNAME}-consumer-modules ${_module} )
	INSTALL ( TARGETS ${_module} DESTINATION ${INSTALL_PREFIX_LIBS} )
ENDFOREACH ()
# The one library a module links: courtier's MPI library, where the process's MPI environment is
# defined, once; the MPI module needs it at load time (also when a program that does not link that
# library loads the module).
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	TARGET_LINK_LIBRARIES ( ${GENEVA_MPI_LIBNAME} PRIVATE ${COURTIER_MPI_LIBNAME} )
ENDIF ()
# The modules' file names before the mnemonic
SET_PROPERTY ( SOURCE GOptimizationDescription.cpp APPEND PROPERTY COMPILE_DEFINITIONS
	"GENEVA_CONSUMER_MODULE_PREFIX=\"lib${GENEVA_LIBNAME}-\"" )
