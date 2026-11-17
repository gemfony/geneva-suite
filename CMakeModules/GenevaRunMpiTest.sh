#!/bin/sh

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
################################################################################
#
# Usage: GenevaRunMpiTest.sh <bound in seconds> <command> [<argument>...]
#
# Runs a ctest entry that starts MPI jobs (GENEVA_ADD_MPI_TEST, GenevaMpiTest.cmake) with a
# directory of its own for the files of Open MPI's shared-memory transport: made below /dev/shm
# (below TMPDIR where there is none) before <command> starts, named to it and to every MPI job it
# starts in OMPI_MCA_btl_sm_backing_directory (other MPI libraries ignore it), and removed when
# the script ends -- also when <command> fails, or is ended at <bound> (by coreutils' timeout,
# where there is one: SIGTERM, SIGKILL ten seconds later). The bound lies below the entry's
# TIMEOUT, so that the directory is removed before ctest ends the script: ctest ends a test with
# SIGKILL, which no script can answer.
#
# Open MPI 5 names these files after the job's id, which it derives from a 16-bit hash. Two jobs
# that run at the same time -- entries of one "ctest -j", or of two build trees -- can get the same
# id, and then use each other's memory and crash or hang.
#
# The exit code is <command>'s; at the bound timeout's (124, or 137 after SIGKILL).
#
################################################################################

if [ $# -lt 2 ]; then
    echo "Usage: GenevaRunMpiTest.sh <bound in seconds> <command> [<argument>...]" >&2
    exit 2
fi
bound=$1
shift

base=/dev/shm
if [ ! -d "$base" ] || [ ! -w "$base" ]; then
    base=${TMPDIR:-/tmp}
fi
dir=$(mktemp -d "$base/geneva-ctest-sm.XXXXXX") || exit 2
trap 'rm -rf "$dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
OMPI_MCA_btl_sm_backing_directory=$dir
export OMPI_MCA_btl_sm_backing_directory

if command -v timeout > /dev/null 2>&1; then
    timeout --kill-after=10 "$bound" "$@"
else
    "$@"
fi
rc=$?
exit $rc
