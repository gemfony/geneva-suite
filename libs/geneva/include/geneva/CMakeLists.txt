SET ( OPTIMIZATIONINCLUDES
    GenevaHelperFunctions.hpp
	GenevaInitializer.hpp
	Go2.hpp
	GConsumerSetup.hpp
	GMarshallerSetup.hpp
	GModuleLoader.hpp
    GOptimizationEnums.hpp
	GPersonalityTraits.hpp
)

# Parameter / adaptor object hierarchy (namespace Gem::Geneva::Genome),
# installed under geneva/par to mirror the `gen` alias.
SET ( PARAM_INCLUDES
    genome/GParameterPropertyParser.hpp
    genome/GGenomeFactory.hpp
    genome/GGenomeFixedSizePriorityQueue.hpp
    genome/GGenomeMultiConstraint.hpp
)

# Individual / genome headers (namespace Gem::Geneva::Genome), installed
# under geneva/ind. This is the public flat-genome authoring + storage API:
# the abstract individual base, the sole flat genome, the genome builder /
# layout / architecture / factory, the auxiliary store and the adaption
# kernels. All are pulled in transitively by the installed individuals/*.hpp,
# so every one must ship.
SET ( IND_INCLUDES
    genome/GAuxiliaryStore.hpp
    genome/GIndividualProcessingResult.hpp
    genome/GProblemPolicy.hpp
    genome/GGenomeT.hpp
    genome/GAdaptionAuxKeys.hpp
    genome/GAdaptionKernels.hpp
    genome/GGenomeLayout.hpp
    genome/GGenomeLayoutSerialization.hpp
    genome/GGenomeBuilder.hpp
    genome/GGenomeArchitecture.hpp
    genome/GGenome.hpp
    genome/GGenomeWireProtocol.hpp
    genome/GIndividualFactory.hpp
    genome/GIndividualPlugin.hpp
    genome/GMultiConstraintT.hpp
    genome/GIndividualMultiConstraint.hpp
)

# Optimization-algorithm headers (namespace Gem::Geneva::OptimizationAlgorithms),
# installed under geneva/oa to mirror the `oa` alias.
SET ( OA_INCLUDES
    oa/GOptimizationAlgorithmBase.hpp
    oa/GOptimizationAlgorithmT.hpp
    oa/GOptimizationAlgorithmFactoryT.hpp
    oa/GPluggableOptimizationMonitors.hpp
    oa/GPostProcessorT.hpp
    oa/GParChild.hpp
    oa/GAdaptionConfig.hpp
    oa/GParetoTools.hpp
    oa/GAdaption.hpp
    oa/GInitializerT.hpp
    oa/GOAFactoryT.hpp
    oa/GOAPlugin.hpp
    oa/GFactoryStore.hpp
    oa/GBaseParChildPersonalityTraits.hpp
    oa/GPositionPersonalityTraits.hpp
    oa/GConjugateGradientDescent.hpp
    oa/GConjugateGradientDescent_PersonalityTraits.hpp
    oa/GHesseError.hpp
    oa/GLineSearch.hpp
    oa/GEvolutionaryAlgorithm.hpp
    oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp
    oa/GNelderMead.hpp
    oa/GNelderMead_PersonalityTraits.hpp
    oa/GParameterScan.hpp
    oa/GParameterScanFactory.hpp
    oa/GParameterScan_PersonalityTraits.hpp
    oa/GSimulatedAnnealing.hpp
    oa/GSimulatedAnnealing_PersonalityTraits.hpp
    oa/GSwarmAlgorithm.hpp
    oa/GSwarmAlgorithm_PersonalityTraits.hpp
    oa/GSepCmaEvolutionStrategy.hpp
    oa/GSepCmaEvolutionStrategy_PersonalityTraits.hpp
    oa/GStandardPSO2011.hpp
    oa/GStandardPSO2011_PersonalityTraits.hpp
    oa/GAntColonyOptimization.hpp
    oa/GAntColonyOptimization_PersonalityTraits.hpp
    oa/GGeneralizedSimulatedAnnealing.hpp
    oa/GGeneralizedSimulatedAnnealing_PersonalityTraits.hpp
)

# Interface headers (namespace Gem::Geneva::Interface), installed under
# geneva/Interface to mirror the nested `Interface` namespace.
SET ( IFACE_INCLUDES
    Interface/GRateableI.hpp
    Interface/GOptimizerIT.hpp
)

# Reusable sample individuals (namespace Gem::Geneva::Individuals),
# installed under geneva/individuals to mirror the `gind` alias. The
# former geneva-individuals library was dissolved into the geneva
# library; these headers live on here.
SET ( INDIVIDUALS_INCLUDES
    individuals/GBenchmarkFunctions.hpp
    individuals/GDelayIndividual.hpp
    individuals/GExternalEvaluatorIndividual.hpp
    individuals/GFunctionIndividual.hpp
    individuals/GLineFitIndividual.hpp
    individuals/GMetaOptimizerIndividualT.hpp
    individuals/GNeuralNetworkIndividual.hpp
    individuals/GTestIndividual1.hpp
    individuals/GTestIndividual2.hpp
    individuals/GTestIndividual3.hpp
)

# Add MPI specific files
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(OPTIMIZATIONINCLUDES
			${OPTIMIZATIONINCLUDES}
			GMPISubClientOptimizer.hpp
			GMPISubClientIndividual.hpp)
ENDIF()

# This is a workaround for a CLion-problem -- see CPP270 in the JetBrains issue tracker
ADD_CUSTOM_TARGET(CLION_ALL_GENEVA_HEADERS SOURCES ${OPTIMIZATIONINCLUDES} ${OA_INCLUDES} ${IFACE_INCLUDES} ${PARAM_INCLUDES} ${IND_INCLUDES} ${INDIVIDUALS_INCLUDES})

# Install ALL headers under this directory wholesale (directory install), so the installed public interface
# is self-contained by construction and cannot drift out of sync with newly added headers or transitive
# includes. (The SET lists above are retained only to populate the CLion header target for IDE indexing.)
INSTALL ( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${INSTALL_PREFIX_INCLUDES}/geneva
	FILES_MATCHING PATTERN "*.hpp" )

# GEqualityPrinter.hpp (the only former include/geneva/tests/ header) is
# pure test scaffolding and now lives next to its sole consumer in
# tests/geneva/UnitTests/ (internal only, never installed) — like
# Geneva_tests.hpp before it.
