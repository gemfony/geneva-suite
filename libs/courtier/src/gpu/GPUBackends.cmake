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

# GPU-consumer backend sources + device-toolkit detection. This fragment is INCLUDEd from
# courtier/src/CMakeLists.txt when GENEVA_BUILD_WITH_GPU_CONSUMER is ON, so the device-model-agnostic
# GPU consumer ships INSIDE the one gemfony-courtier library (just like the MPI consumer), rather than
# in a separate library. It only sets variables; the caller folds the sources into the courtier target
# and applies the matching link libraries + compile definitions:
#   COURTIER_GPU_SRCS         -- backend .cpp to compile into courtier (paths relative to courtier/src)
#   COURTIER_GPU_HAVE_CUDA    -- TRUE when the CUDA (NVRTC + driver API) backend is included
# The GPU consumer is device-only (no CPU backend); the CUDA headers are confined to these .cpp,
# so folding them in does not leak device headers into courtier's public interface.

SET ( COURTIER_GPU_SRCS
	gpu/GGPUBackendFactory.cpp
)

# --- optional CUDA backend (NVRTC + CUDA driver API; no nvcc compilation here) ----------------
# CUDA is enabled globally by the top-level CMakeLists when a toolkit is found (CMAKE_CUDA_COMPILER)
# and GENEVA_SKIP_CUDA is OFF.
SET ( COURTIER_GPU_HAVE_CUDA FALSE )
IF ( CMAKE_CUDA_COMPILER )
	SET ( COURTIER_GPU_HAVE_CUDA TRUE )
	SET ( COURTIER_GPU_SRCS ${COURTIER_GPU_SRCS} gpu/GCUDABackend.cpp )
ENDIF ()
