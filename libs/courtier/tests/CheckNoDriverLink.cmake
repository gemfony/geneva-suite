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

# Regression guard: no Geneva library may depend on the CUDA driver library.
#
# libcuda.so.1 comes with the NVIDIA driver, not with the CUDA toolkit. A library that needs it at
# load time keeps every program linked against it from starting on a machine without a driver --
# even for a run on the CPU consumers. The GPU consumer therefore loads the driver at run time
# (libs/courtier/src/gpu/GCUDADriver), and this check fails if a link dependency on it comes back,
# directly or through another library. ldd lists the whole dependency closure.
#
# Driven via `cmake -P` with -DLIBS="<library>;<library>;...".

foreach(_lib IN LISTS LIBS)
    execute_process(
        COMMAND ldd "${_lib}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "ldd failed on ${_lib}: ${_err}")
    endif()
    if(_out MATCHES "libcuda\\.so")
        message(FATAL_ERROR
            "${_lib} depends on the CUDA driver library at load time:\n${_out}\n"
            "Programs linked against it would not start on a machine without an NVIDIA driver. "
            "The driver must be loaded at run time (see libs/courtier/src/gpu/GCUDADriver.hpp).")
    endif()
endforeach()

message(STATUS "CheckNoDriverLink: no Geneva library needs libcuda.so at load time")
