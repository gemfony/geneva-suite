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

SET ( EXECUTABLENAME GenevaStandardTests )

SET ( ${EXECUTABLENAME}_SRCS
	GenevaStandardTests.cpp
	GLineSearchTests.cpp
	GHesseErrorTests.cpp
	GAuxiliaryStoreTests.cpp
	GAdaptionKernelTests.cpp
	GGenomeTests.cpp
	GGenomeProcessingTests.cpp
	GNormalizedGenomeTests.cpp
	GOptimizationAlgorithmsTests.cpp
	GStandardPSO2011Tests.cpp
	GAntColonyOptimizationTests.cpp
	GGeneralizedSimulatedAnnealingTests.cpp
	GOABenchmark.cpp
	GEvolutionaryAlgorithmModesTests.cpp
	GAdaptionConfigTests.cpp
	GSharedConsumerTests.cpp
	GProgressPlotterTests.cpp
	GMultiConstraintTests.cpp
	GLateReturnIntegrationTests.cpp
	GFaultInjectorTests.cpp
	GNeuralNetworkDataTests.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		Catch2::Catch2WithMain
		# These individuals are no longer compiled into the geneva library; the tests construct and serialize
		# them directly, so they link the individuals' object libraries (which carry their GEM_REGISTER_ARCHIVABLE).
		GLineFitIndividual-obj
		GNeuralNetworkIndividual-obj
		GExternalEvaluatorIndividual-obj
		GDelayIndividual-obj
		GFunctionIndividual-obj
		GMetaOptimizerIndividualT-obj
		# GTestIndividual2/3 are packaged as object libraries (GTestIndividual1 stays in the GEM_TESTING library).
		GTestIndividual2-obj
		GTestIndividual3-obj)

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})


#
# Deliberately broken modules, for the loader's negative paths. Each is a real shared object, built with
# exactly the recipe a genuine module uses (GENEVA_ADD_INDIVIDUAL_MODULE: no Geneva libraries linked,
# -UGEM_TESTING, pinned ABI flags), carrying one specific defect. The unit tests really dlopen them, so the
# rejection is proved through the whole loader path rather than against a hand-made in-process struct.
# Their paths are handed to the test executable as compile definitions -- nothing has to be installed, and
# the tests skip themselves if this wiring is ever dropped.
#
SET(GENEVA_BAD_MODULES
	GStaleAbiModule
	GUnknownKindModule
	GNoContributionsModule
	GNullFactoryModule)

FOREACH(_bad_module ${GENEVA_BAD_MODULES})
	GENEVA_ADD_INDIVIDUAL_MODULE(${_bad_module} badmodules/${_bad_module}.cpp)
	ADD_DEPENDENCIES(${EXECUTABLENAME} ${_bad_module})
ENDFOREACH()

TARGET_COMPILE_DEFINITIONS(${EXECUTABLENAME} PRIVATE
	"GENEVA_TEST_MODULE_STALE_ABI=\"$<TARGET_FILE:GStaleAbiModule>\""
	"GENEVA_TEST_MODULE_UNKNOWN_KIND=\"$<TARGET_FILE:GUnknownKindModule>\""
	"GENEVA_TEST_MODULE_NO_CONTRIBUTIONS=\"$<TARGET_FILE:GNoContributionsModule>\""
	"GENEVA_TEST_MODULE_NULL_FACTORY=\"$<TARGET_FILE:GNullFactoryModule>\""
	# A perfectly good shared library that is not a Geneva module at all -- the "you pointed me at
	# something else" path. Geneva's own library is the one guaranteed to be there.
	"GENEVA_TEST_NOT_A_MODULE=\"$<TARGET_FILE:${GENEVA_LIBNAME}>\"")

#
# CTest registration: the binary is registered as SHARDS, not as one entry.
#
# As a single entry this suite was the serial long pole of the whole test run -- it took as long on
# its own as every other test put together, so `ctest -j` could never finish faster than it. Nothing
# in it is genuinely sequential: the cost is concentrated in a handful of convergence measurements.
# Registering test-spec subsets of the SAME binary lets CTest overlap them. No test code moves; the
# executable is unchanged and still runs everything when invoked by hand.
#
# The specs are chosen by measured cost, heaviest first, so the shards are balanced rather than
# merely equal in case count. The two EA convergence cases are ~25 s and ~22 s EACH -- a single
# TEST_CASE is atomic, so those two set the floor for the whole suite and are shards of their own.
#
#   spec                    what it selects                                     measured  size
#   [highdim10k]            one EA convergence case, n = 10000                    ~26 s     L
#   [highdim]               one EA convergence case, high-dim                     ~22 s     L
#   [gsa]                   the annealing convergence cases                       ~16 s     L
#   [net]                   the cases that really open a loopback socket (own      ~7 s     M
#                           shard so networked work overlaps CPU-bound work)
#   [oa]~<the above>        the remaining algorithm-level cases                    ~4 s     M
#   ~[oa]~<the above>       everything else                                        ~5 s     M
#
# Each spec excludes the tags claimed before it, so the last two are complements of each other within
# what the first four leave. The last one is the CATCH-ALL: a newly added TEST_CASE lands in it
# automatically instead of falling out of the suite. That the six specs really do partition the case
# list is not assumed -- GenevaStandardTests-shard-completeness proves it on every run (see
# CheckShardCompleteness.cmake).
#
SET(GENEVA_STANDARD_TEST_SHARDS
	"ea-highdim10k"  L  "[highdim10k]"
	"ea-highdim"     L  "[highdim]"
	"gsa"            L  "[gsa]"
	"net"            M  "[net]"
	"oa"             M  "[oa]~[highdim10k]~[highdim]~[gsa]~[net]"
	"core"           M  "~[oa]~[highdim10k]~[highdim]~[gsa]~[net]")

SET(GENEVA_STANDARD_TEST_SPECS "")
LIST(LENGTH GENEVA_STANDARD_TEST_SHARDS _n_shard_entries)
MATH(EXPR _last_shard_index "${_n_shard_entries} / 3 - 1")
FOREACH(_i RANGE ${_last_shard_index})
	MATH(EXPR _name_index "${_i} * 3")
	MATH(EXPR _size_index "${_i} * 3 + 1")
	MATH(EXPR _spec_index "${_i} * 3 + 2")
	LIST(GET GENEVA_STANDARD_TEST_SHARDS ${_name_index} _shard_name)
	LIST(GET GENEVA_STANDARD_TEST_SHARDS ${_size_index} _shard_size)
	LIST(GET GENEVA_STANDARD_TEST_SHARDS ${_spec_index} _shard_spec)
	ADD_TEST(NAME ${EXECUTABLENAME}-${_shard_name} COMMAND ${EXECUTABLENAME} "${_shard_spec}")
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}-${_shard_name} PROPERTIES LABELS "geneva")
	GENEVA_TEST_SIZE(${EXECUTABLENAME}-${_shard_name} ${_shard_size})
	LIST(APPEND GENEVA_STANDARD_TEST_SPECS "${_shard_spec}")
ENDFOREACH()

# The guard that keeps the sharding honest: it fails if any case is selected by no shard or by more
# than one. Cheap -- it only lists test cases, it does not run them.
ADD_TEST(
	NAME ${EXECUTABLENAME}-shard-completeness
	COMMAND ${CMAKE_COMMAND}
		-DTEST_EXE=$<TARGET_FILE:${EXECUTABLENAME}>
		"-DSPECS=${GENEVA_STANDARD_TEST_SPECS}"
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckShardCompleteness.cmake
)
SET_TESTS_PROPERTIES(${EXECUTABLENAME}-shard-completeness PROPERTIES LABELS "geneva")
GENEVA_TEST_SIZE(${EXECUTABLENAME}-shard-completeness M)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/tests/geneva/UnitTests )


#
# Take care of other particularities of this test
#

GENEVA_AGGREGATE_TARGET( "tests-geneva-unit" )
