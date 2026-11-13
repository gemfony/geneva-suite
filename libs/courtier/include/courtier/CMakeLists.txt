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

SET ( COMMUNICATIONINCLUDES
    GBaseClientT.hpp
    GBaseConsumerT.hpp
    GCommandContainerT.hpp
    GCourtierEnums.hpp
    GCourtierHelperFunctions.hpp
    GDemoProcessingContainers.hpp
    GSubmissionStatusT.hpp
    GProcessable.hpp
    GProcessingContainerT.hpp
    GProcessingOutcome.hpp
    GServerSessionLogic.hpp
    GSubmissionPolicy.hpp
    GWireCodec.hpp
    GWireProtocolT.hpp
    GWireSerializationContext.hpp
)

# Consumer classes (namespace Gem::Courtier): the span+policy submission consumers (formerly the
# courtier2 rework, merged in at Stage E).
SET ( CONSUMER_INCLUDES
    consumers/GAsioConsumerT.hpp
    consumers/GNetworkedConsumerT.hpp
    consumers/GTcpAcceptingConsumerT.hpp
    consumers/GStdThreadConsumerT.hpp
    consumers/GWebsocketConsumerT.hpp
)

# Transport classes (namespace Gem::Courtier::Consumers): the asio/websocket sessions + clients and
# the MPI master/worker nodes that the networked consumers reuse. Named under transport/ so they no
# longer collide with the consumer headers of the same mnemonic.
SET ( TRANSPORT_INCLUDES
    transport/GAsioTransportT.hpp
    transport/GPrefetchingClientT.hpp
    transport/GWebsocketTransportT.hpp
)

# GPU consumer framework (namespace Gem::Courtier::GPU): the device-agnostic core, the CUDA backend
# and the bulk-dispatch consumer. The GPU consumer is DEVICE-ONLY (a CPU run uses the individual's own
# evaluate() via a CPU consumer such as --consumer stc). Header-only except for the backend .cpp files,
# which are compiled into gemfony-courtier as an opt-in add-on when GENEVA_BUILD_WITH_GPU_CONSUMER is ON
# (see src/gpu/GPUBackends.cmake). The headers are device-toolkit-clean and install unconditionally.
SET ( GPU_INCLUDES
    gpu/GCUDABackend.hpp
    gpu/GGPUBackendFactory.hpp
    gpu/GGPUConsumer.hpp
    gpu/GGPUConsumerConfig.hpp
    gpu/GGPUDeviceBackendI.hpp
    gpu/GGPUEvaluableI.hpp
    gpu/GGPUKernelSpec.hpp
)

# MPI consumer/transport is optional, only build it when requested
IF (GENEVA_BUILD_WITH_MPI_CONSUMER)
    SET( COMMUNICATIONINCLUDES
            ${COMMUNICATIONINCLUDES}
            GMPIHelperFunctions.hpp)
    SET( CONSUMER_INCLUDES
            ${CONSUMER_INCLUDES}
            consumers/GMPIConsumerT.hpp)
    SET( TRANSPORT_INCLUDES
            ${TRANSPORT_INCLUDES}
            transport/GMPITransportT.hpp)
ENDIF ()

set_source_files_properties(
    ${COMMUNICATIONINCLUDES} ${CONSUMER_INCLUDES} ${TRANSPORT_INCLUDES} ${GPU_INCLUDES}
    PROPERTIES
    LANGUAGE CXX
)

# This is a workaround for a CLion-problem -- see CPP270 in the JetBrains issue tracker
ADD_CUSTOM_TARGET(CLION_ALL_COURTIER_HEADERS
    SOURCES ${COMMUNICATIONINCLUDES} ${CONSUMER_INCLUDES} ${TRANSPORT_INCLUDES} ${GPU_INCLUDES})

# Directory install: ship the whole header tree (Inv 19), so the installed interface is self-contained and
# cannot drift. (The SET lists above are retained only for the CLion header target / IDE indexing.)
INSTALL ( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${INSTALL_PREFIX_INCLUDES}/courtier
	FILES_MATCHING PATTERN "*.hpp" )
