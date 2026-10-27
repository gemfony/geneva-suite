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
# GGlobalDefines.hpp is a regular, manually maintained header — it is no longer
# generated from a .in template.  When bumping the Geneva version or the minimum
# Boost version, update the GENEVA_VERSION and MIN_BOOST_VERSION macros in that
# file directly (see the comments there for the encoding rules).
# The authoritative version numbers are VERSION_MAJOR / VERSION_MINOR /
# VERSION_PATCH in the top-level CMakeLists.txt and GENEVA_MIN_BOOST_VERSION in
# CMakeModules/CommonGenevaBuild.cmake.
#

SET ( GENEVACOMMONINCLUDES
	GBuildFingerprint.hpp
	GCommonEnums.hpp
	GCommonHelperFunctions.hpp
	GCommonHelperFunctionsT.hpp
	GCommonInterfaceT.hpp
	GCommonMathHelperFunctionsT.hpp
	GConfigEmission.hpp
	GContainerT.hpp
	GDefaultValueT.hpp
	GErrorStreamer.hpp
	GExceptions.hpp
	GExpectationChecksT.hpp
	GFactoryT.hpp
	GFixedSizePriorityQueueT.hpp
	GGlobalDefines.hpp
	GGlobalOptionsT.hpp
	GLogger.hpp
	GMemberReflectionT.hpp
	GModuleManifest.hpp
	GParserBuilder.hpp
	GProviderT.hpp
	GSerializableFunctionObjectT.hpp
	GSerializationHelperFunctionsT.hpp
	GSigHupHandler.hpp
	GSingletonT.hpp
	GTupleIO.hpp
	GTypeToStringT.hpp
	GTypeTraitsT.hpp
	GUnitTestFrameworkT.hpp
)

# The concurrency facilities (thread pool/group + MPMC queues) live in a distinct
# sub-module: the common/concurrency/ sub-directory + the Gem::Common::Concurrency
# namespace, so they are clearly separated and hard to overlook.
SET ( GENEVACOMMONCONCURRENCYINCLUDES
	concurrency/GAgingStoreT.hpp
	concurrency/GBlockingMPMCQueueT.hpp
	concurrency/GCompletionLatchT.hpp
	concurrency/GContentAddressedStoreT.hpp
	concurrency/GMPMCQueueT.hpp
	concurrency/GPreallocatedMPMCQueueT.hpp
	concurrency/GQueueCommon.hpp
	concurrency/GSPSCStagingRingT.hpp
	concurrency/GThreadGroup.hpp
	concurrency/GThreadPool.hpp
	concurrency/GThreadSafeKeyedStoreT.hpp
	concurrency/GThreadSafeSetT.hpp
)

# This is a workaround for a CLion-problem -- see CPP270 in the JetBrains issue tracker
ADD_CUSTOM_TARGET(CLION_ALL_COMMON_HEADERS SOURCES ${GENEVACOMMONINCLUDES} ${GENEVACOMMONCONCURRENCYINCLUDES})


# Directory install: ship the whole header tree (Inv 19), so the installed interface is self-contained and
# cannot drift. (The SET lists above are retained only for the CLion header target / IDE indexing.)
INSTALL ( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${INSTALL_PREFIX_INCLUDES}/common
	FILES_MATCHING PATTERN "*.hpp" )

# The plotting subsystem was extracted into its own first-class library, "dietrich"
# (dietrich/include/dietrich/...). common no longer ships any plotting headers.
