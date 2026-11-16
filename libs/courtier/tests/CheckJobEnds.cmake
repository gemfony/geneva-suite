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

# Driver of the ctest entries that start an MPI job and judge how it ends: the exit code, texts
# that its output must or must not contain, and that it ends within a bound at all. A job that
# hangs is red, not a ctest timeout; a job that must fail is checked for why it failed, which
# WILL_FAIL cannot do.
#
# Driven via `cmake -P` with
#   -DCOMMAND=<list>             the command (empty elements, e.g. an empty MPIEXEC_PREFLAGS, are
#                                dropped)
#   -DEXPECT_EXIT=zero|nonzero   the exit code the job must end with
#   -DEXPECT=<list of texts>     texts the output must contain (optional)
#   -DREFUSE=<list of texts>     further texts the output must not contain (optional)
#   -DLIMIT=<seconds>            the bound
#
# One rule for both kinds of job: a program under this driver prints "FAIL:" only for what is
# wrong, so the output of every job -- one that must end with 0 and one that must end otherwise --
# must not contain "FAIL:". What a job must say is in EXPECT.
#
# The command runs under coreutils' timeout where it exists: at the bound the launcher gets
# SIGTERM, so that it ends its ranks rather than leaving them behind. Every line of the verdict
# starts with OK: or FAIL:.

foreach(_required COMMAND EXPECT_EXIT LIMIT)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "FAIL: CheckJobEnds.cmake needs -D${_required}=...")
    endif()
endforeach()
if(NOT EXPECT_EXIT MATCHES "^(zero|nonzero)$")
    message(FATAL_ERROR "FAIL: EXPECT_EXIT must be 'zero' or 'nonzero', not '${EXPECT_EXIT}'")
endif()
list(REMOVE_ITEM COMMAND "")

find_program(_timeout_program timeout)
if(_timeout_program)
    set(_run ${_timeout_program} --kill-after=10 ${LIMIT} ${COMMAND})
    math(EXPR _backstop "${LIMIT} + 20")
else()
    set(_run ${COMMAND})
    set(_backstop ${LIMIT})
endif()

execute_process(
    COMMAND ${_run}
    TIMEOUT ${_backstop}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _out)
message("${_out}")

if(NOT _rc MATCHES "^-?[0-9]+$" OR (_timeout_program AND _rc EQUAL 124))
    # Processes of this test program that are still there (of this entry or of another one that
    # runs at the same time): a hint for whoever looks at the failure.
    execute_process(COMMAND pgrep -xc GCourtierMpiCas OUTPUT_VARIABLE _left ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(FATAL_ERROR "FAIL: the job did not end within ${LIMIT} s (${_rc}); "
        "processes named GCourtierMpiCas still running: ${_left}")
endif()
if(EXPECT_EXIT STREQUAL "zero")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "FAIL: the job ended with the exit code ${_rc}, not 0")
    endif()
elseif(_rc EQUAL 0)
    message(FATAL_ERROR "FAIL: the job ended with the exit code 0")
endif()
list(APPEND REFUSE "FAIL:")
foreach(_text IN LISTS EXPECT)
    string(FIND "${_out}" "${_text}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "FAIL: the output does not contain '${_text}'")
    endif()
endforeach()
foreach(_text IN LISTS REFUSE)
    string(FIND "${_out}" "${_text}" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "FAIL: the output contains '${_text}'")
    endif()
endforeach()
message(STATUS "OK: the job ended with ${_rc} and said what it had to say")
