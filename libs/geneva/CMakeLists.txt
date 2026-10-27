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
#
# Orchestrator for the self-contained Geneva library subtree (the largest
# of the four; absorbed the former geneva-individuals library in the 1.99 redesign).
#
# Layout (per the 2026-05-28 repo-layout reorganisation):
#   geneva/include/geneva/             public headers (install layout 1:1)
#     *.hpp                              top-level orchestration / cross-layer
#                                        (Go2.hpp, GConsumerSetup.hpp,
#                                        GPersonalityTraits.hpp, ...)
#     genome/*.hpp                       genome / entity infrastructure (GGenome,
#                                        GGenome, factories, constraints
#                                        (GMultiConstraintT / GIndividualMultiConstraint),
#                                        priority queue, parameter-property parser)
#     oa/*.hpp                           optimization algorithms (EA, SA, Swarm, GD,
#                                        CGD, NelderMead, PS) + OA monitors
#                                        (GPluggableOptimizationMonitors) and the
#                                        nested-EA refiner (GPostProcessorT)
#     individuals/*.hpp                  reusable problem definitions
#     Interface/*.hpp                    abstract interfaces
#   geneva/src/*.cpp                   implementation (flat, mirrors the
#                                        top-level header set); 73 sources
#   geneva/tests/                      UnitTests + ManualTests
#   geneva/examples/{01_..21_}/        all numbered examples
#   geneva/docprograms/                compiled listings and figure producers for
#                                        the manual, each registered as an S-size test
#   geneva/quickstart/                 the self-contained downstream template
#                                        (find_package(Geneva); NOT built in tree)
#   geneva/benchmarks/                 6 benchmark binaries
#
# Tests / examples / benchmarks gated on the top-level options. The
# "geneva-all" aggregate is picked up by gemfony-build-all at top level.
################################################################################

ADD_SUBDIRECTORY ( include/geneva )
ADD_SUBDIRECTORY ( src )

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( docprograms )
ENDIF ()

IF ( GENEVA_BUILD_BENCHMARKS )
	ADD_SUBDIRECTORY ( benchmarks )
ENDIF ()

# The unified GPU consumer framework (CPU/CUDA backends, runtime kernel loading) lives in the
# Courtier library (courtier/include/courtier/gpu + courtier/src/gpu). It is an opt-in add-on folded
# INTO gemfony-courtier when GENEVA_BUILD_WITH_GPU_CONSUMER is ON (like the MPI consumer); there is no
# separate GPU library. It is exercised by example 14 (the Mona-Lisa problem) and the GPU optimisation
# benchmark, both built only when the GPU consumer is.

# Per-library Doxygen target (doc-geneva).
ADD_SUBDIRECTORY ( doc )

GENEVA_AGGREGATE_TARGET( "geneva-all" )
