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

# Regression guard: no Geneva library or program may depend at load time on a library that not every
# machine has -- the CUDA driver (libcuda.so.1, which comes with the NVIDIA driver, not the toolkit) or
# the MPI library. Linked in, either keeps every program from starting where it is missing, even for a
# run that does not use it. Geneva therefore loads both at run time: the CUDA driver through
# libs/courtier/src/gpu/GCUDADriver, the MPI consumer as the module libgemfony-geneva-mpi. This check
# fails if a link dependency on such a library comes back, directly or through another library; ldd
# lists the whole dependency closure.
#
# Driven via `cmake -P` with -DLIBS="<file>;<file>;..." and -DPATTERN=<regex> (default: libcuda\.so).

if(NOT DEFINED PATTERN)
    set(PATTERN "libcuda\\.so")
endif()

foreach(_lib IN LISTS LIBS)
    execute_process(
        COMMAND ldd "${_lib}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "ldd failed on ${_lib}: ${_err}")
    endif()
    if(_out MATCHES "${PATTERN}")
        message(FATAL_ERROR
            "${_lib} depends at load time on a library matching '${PATTERN}':\n${_out}\n"
            "Programs linked against it would not start on a machine without that library. It must be "
            "loaded at run time instead (see libs/courtier/src/gpu/GCUDADriver.hpp for the CUDA driver, "
            "libs/geneva/src/GMPIConsumerModule.cpp for MPI).")
    endif()
endforeach()

message(STATUS "CheckNoDriverLink: nothing checked needs a library matching '${PATTERN}' at load time")
