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

SET ( EXECUTABLENAME GOptimizationBenchmark )

SET ( ${EXECUTABLENAME}_HEADERS
	GOptimizationBenchmarkConfig.hpp
)

# This is a workaround for a CLion-problem -- see CPP270 in the JetBrains issue tracker
ADD_CUSTOM_TARGET(CLION_ALL_GOPTIMIZATIONBENCHMARK_HEADERS SOURCES ${${EXECUTABLENAME}_HEADERS})

SET ( ${EXECUTABLENAME}_SRCS
	GOptimizationBenchmark.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		# GFunctionIndividual is no longer in the geneva library; compiled in via its object library (before the
		# Geneva/Boost libraries, the in-tree convention for an object library carrying archive registrations).
		GFunctionIndividual-obj
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})


TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})

# config-overrides.json is what makes this a benchmark rather than a demo. The measurement it takes --
# quality reached, and time taken, as a function of problem dimension -- is only comparable across
# dimensions if every row gets the SAME budget, so the shipped configuration fixes the run at 1000
# iterations and turns the stall criterion OFF. With the library default (halt after 20 stalled
# generations) each dimension stopped whenever it happened to plateau, and the resulting curve measured
# the stall criterion as much as the problem: the whole sweep finished in five seconds.
#
# These are the benchmark's own values, overlaid onto its own materialized configuration. Nothing about
# the library defaults changes -- a fixed 1000-iteration budget with no stall halt is right here and
# wrong nearly everywhere else (Inv 22).
GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/tests/geneva/PerformanceTests/${EXECUTABLENAME}/config )

# A convergence-scale run: it optimizes the same problem at nine dimensions from 2 to 512, ten times
# each, at a fixed 1000-iteration budget -- minutes, not seconds (123 s measured on the development
# host, Debug). Registered like every other test; what keeps it out of the development loop is its
# measured size (XL), not a flag.
#
# The TIMEOUT is ~5x the measured runtime, so a slower or loaded machine fails on the merits rather
# than on the clock.
ADD_TEST(${EXECUTABLENAME} ${EXECUTABLENAME})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "benchmark" TIMEOUT 600)
GENEVA_TEST_SIZE(${EXECUTABLENAME} XL)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME} )
