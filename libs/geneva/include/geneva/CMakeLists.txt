SET ( OPTIMIZATIONINCLUDES
    GFaultInjector.hpp
    GMarshallerPlugin.hpp
    GMarshallerSetup.hpp
    GModuleLoader.hpp
    GNamespaceAliases.hpp
    GOptimizationDescription.hpp
    GOptimizationEnums.hpp
    GPersonalityTraits.hpp
    GenevaHelperFunctions.hpp
    GenevaInitializer.hpp
    Go2.hpp
)

# Parameter / adaptor object hierarchy (namespace Gem::Geneva::Genome),
# installed under geneva/genome to mirror the `gen` alias.
SET ( PARAM_INCLUDES
    genome/GGenomeFactory.hpp
    genome/GGenomeFixedSizePriorityQueue.hpp
    genome/GGenomeMultiConstraint.hpp
    genome/GParameterPropertyParser.hpp
)

# Individual / genome headers (namespace Gem::Geneva::Genome), installed
# under geneva/genome. This is the public flat-genome authoring + storage API:
# the abstract individual base, the sole flat genome, the genome builder /
# layout / architecture / factory, the auxiliary store and the adaption
# kernels. All are pulled in transitively by the installed individuals/*.hpp,
# so every one must ship.
SET ( IND_INCLUDES
    genome/GAdaptionAuxKeys.hpp
    genome/GAdaptionKernels.hpp
    genome/GAuxiliaryStore.hpp
    genome/GBaseGPUMarshallerT.hpp
    genome/GGenome.hpp
    genome/GGenomeArchitecture.hpp
    genome/GGenomeBuilder.hpp
    genome/GGenomeLayout.hpp
    genome/GGenomeLayoutSerialization.hpp
    genome/GGenomeT.hpp
    genome/GGenomeWireProtocol.hpp
    genome/GIndividualFactory.hpp
    genome/GIndividualMultiConstraint.hpp
    genome/GIndividualPlugin.hpp
    genome/GIndividualProcessingResult.hpp
    genome/GMultiConstraintT.hpp
    genome/GProblemPolicy.hpp
    genome/GProblemStoreT.hpp
)

# Optimization-algorithm headers (namespace Gem::Geneva::OptimizationAlgorithms),
# installed under geneva/oa to mirror the `oa` alias.
SET ( OA_INCLUDES
    oa/GAdaption.hpp
    oa/GAdaptionConfig.hpp
    oa/GAlgorithmPersonalityTraitsT.hpp
    oa/GAntColonyOptimization.hpp
    oa/GAntColonyOptimization_PersonalityTraits.hpp
    oa/GBaseParChildPersonalityTraits.hpp
    oa/GBasePluggableOM.hpp
    oa/GConjugateGradientDescent.hpp
    oa/GConjugateGradientDescent_PersonalityTraits.hpp
    oa/GEvolutionaryAlgorithm.hpp
    oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp
    oa/GFactoryStore.hpp
    oa/GGeneralizedSimulatedAnnealing.hpp
    oa/GGeneralizedSimulatedAnnealing_PersonalityTraits.hpp
    oa/GHesseError.hpp
    oa/GInitializerT.hpp
    oa/GLineSearch.hpp
    oa/GMetaEvolutionaryAlgorithm.hpp
    oa/GMonitorStream.hpp
    oa/GNelderMead.hpp
    oa/GNelderMead_PersonalityTraits.hpp
    oa/GOAFactoryT.hpp
    oa/GOAPlugin.hpp
    oa/GOptimizationAlgorithmBase.hpp
    oa/GOptimizationAlgorithmFactoryT.hpp
    oa/GOptimizationAlgorithmT.hpp
    oa/GParChild.hpp
    oa/GParameterScan.hpp
    oa/GParameterScanFactory.hpp
    oa/GParameterScan_PersonalityTraits.hpp
    oa/GParetoTag.hpp
    oa/GParetoTools.hpp
    oa/GPluggableOptimizationMonitors.hpp
    oa/GPositionPersonalityTraits.hpp
    oa/GPostProcessorT.hpp
    oa/GSepCmaEvolutionStrategy.hpp
    oa/GSepCmaEvolutionStrategy_PersonalityTraits.hpp
    oa/GSimulatedAnnealing.hpp
    oa/GSimulatedAnnealing_PersonalityTraits.hpp
    oa/GStandardPSO2011.hpp
    oa/GStandardPSO2011_PersonalityTraits.hpp
    oa/GSwarmAlgorithm.hpp
    oa/GSwarmAlgorithm_PersonalityTraits.hpp
    oa/GTunableManifest.hpp
)

# Interface headers (namespace Gem::Geneva::Interface), installed under
# geneva/Interface to mirror the nested `Interface` namespace.
SET ( IFACE_INCLUDES
    Interface/GOptimizerIT.hpp
    Interface/GRateableI.hpp
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
