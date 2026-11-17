#!/usr/bin/env bash

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
#
# Usage: GenevaRunMpiTest.sh <bound in seconds> <command> [<argument>...]
#
# Runs a ctest entry that starts MPI jobs (GENEVA_ADD_MPI_TEST, GenevaMpiTest.cmake) with a
# directory of its own for the files of Open MPI's shared-memory transport: made below /dev/shm
# (below TMPDIR where there is none) before <command> starts, named to it and to every MPI job it
# starts in OMPI_MCA_btl_sm_backing_directory (other MPI libraries ignore it), and removed when
# the script ends -- also when <command> fails, reaches <bound>, or the script is interrupted.
#
# Open MPI 5 names these files after the job's id, which it derives from a 16-bit hash. Two jobs
# that run at the same time -- entries of one "ctest -j", or of two build trees -- can get the same
# id, and then use each other's memory and crash or hang.
#
# The bound. At <bound> seconds <command> gets SIGTERM, and SIGKILL ten seconds later if it is
# still there -- <command> and every process it started, in a process group of their own. Where
# coreutils' timeout exists, it does this (it puts itself and <command> into a group of their
# own); elsewhere <command> is started in a group of its own (bash's job control) and a watch of
# this script signals that group. A bash script for that reason: sh has no job control without a
# terminal. GENEVA_ADD_MPI_TEST sets the entry's TIMEOUT above the
# bound, so that the script still removes the directory before ctest would end it with SIGKILL,
# which no script can answer.
#
# Interrupts. <command> runs in the background while the script waits for it, so that SIGINT,
# SIGTERM and SIGHUP reach the script at once; it passes each on to <command>'s process group as
# SIGTERM (through timeout, where it runs), waits for <command> to end and removes the directory. A Ctrl-C at
# a terminal reaches ctest's process group -- this script, but not timeout's group -- so without
# this the MPI jobs would run on to their bound.
#
# The exit code is <command>'s; at the bound 124 (137 after SIGKILL); after an interrupt 128 plus
# the signal's number (130, 143, 129).
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
OMPI_MCA_btl_sm_backing_directory=$dir
export OMPI_MCA_btl_sm_backing_directory

# An interrupt is noted, and passed on once <command> runs: to timeout, which passes it on to its
# group, or to <command>'s group
child=
group=
interrupted=
interrupt() {
    interrupted=$1
    if [ -n "$group" ]; then
        kill -TERM -- "$group" 2> /dev/null
    fi
}
trap 'interrupt 130' INT
trap 'interrupt 143' TERM
trap 'interrupt 129' HUP

# The bound without coreutils' timeout: SIGTERM to <command>'s group at the bound, SIGKILL ten
# seconds later to what is left of it; the file $dir/.bound tells the script that the bound was
# reached
watch() {
    end=$(($(date +%s) + bound))
    while [ "$(date +%s)" -lt "$end" ]; do
        sleep 1
        kill -0 "$child" 2> /dev/null || exit 0
    done
    : > "$dir/.bound"
    kill -TERM -- "$group" 2> /dev/null
    end=$(($(date +%s) + 10))
    while [ "$(date +%s)" -lt "$end" ]; do
        sleep 1
        kill -0 -- "$group" 2> /dev/null || exit 0
    done
    kill -KILL -- "$group" 2> /dev/null
}

watcher=
if command -v timeout > /dev/null 2>&1; then
    timeout --kill-after=10 "$bound" "$@" &
    child=$!
    group=$child
else
    # A group of its own (job control, for this one start); its input is not the terminal, which
    # a process outside the terminal's foreground group could not read
    set -m
    "$@" < /dev/null &
    child=$!
    set +m
    group=-$child
    watch &
    watcher=$!
fi
if [ -n "$interrupted" ]; then
    kill -TERM -- "$group" 2> /dev/null
fi

# A trapped signal ends a wait early; wait again until <command> has ended
wait "$child"
rc=$?
while kill -0 "$child" 2> /dev/null; do
    wait "$child"
    rc=$?
done
if [ -n "$watcher" ]; then
    kill "$watcher" 2> /dev/null
    wait "$watcher"
    if [ -e "$dir/.bound" ] && [ "$rc" -ne 137 ]; then
        rc=124
    fi
fi
if [ -n "$interrupted" ]; then
    rc=$interrupted
fi
exit $rc
