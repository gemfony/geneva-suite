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

ADD_SUBDIRECTORY ( GConsumerOverhead )
ADD_SUBDIRECTORY ( GConsumerSanityChecks )
ADD_SUBDIRECTORY ( GOptimizationBenchmark )
ADD_SUBDIRECTORY ( GParallelisationOverhead )
ADD_SUBDIRECTORY ( GSerializationOverhead )

# The GPU optimisation benchmark drives the courtier GPU consumer, so it is built only when that
# consumer is (GENEVA_BUILD_WITH_GPU_CONSUMER -- the opt-in add-on folded into gemfony-courtier). Like
# example 14 its kernel is runtime-compiled (NVRTC), so it needs no CUDA language at build time
# -- it builds with an ordinary C++ toolchain and selects CPU/CUDA at run time.
IF ( GENEVA_GPU_CONSUMER_ENABLED )
	ADD_SUBDIRECTORY ( GCUDAOptBenchmark )
ENDIF ()

GENEVA_AGGREGATE_TARGET( "benchmarks-geneva" )
