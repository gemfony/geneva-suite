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
#
################################################################################
# Runs the speed-up measurement of GParallelisationOverhead with a server and N
# clients on this host and leaves the results in a work directory.
#
# Usage:
#   ./runWithClients.sh <binary> <workdir> <consumer> <clients> <population> \
#       <iterations> "<delays>" "<reference delays>" <measurements> [server options]
#
#     binary       the GParallelisationOverhead executable of a build tree
#     workdir      a directory OUTSIDE the source tree
#     consumer     the consumer to measure: a networked one such as beast (a
#                  server process and N client processes), or mpi (one mpirun
#                  with N + 1 ranks: rank 0 is the server, the others its clients)
#     delays       the evaluation times, as "(seconds,milliseconds), ..."
#     reference delays   the (short) evaluation times of the serial reference
#     measurements how often each evaluation time is measured
#
# The serial reference is a run of its own on one local worker. It is kept if
# the work directory already holds a referenceLine.txt, so that several runs can
# share one reference (copy the file in, or set REFRESH_REFERENCE=1 to measure
# it again). Copy the same reference into the work directories of both
# consumers, and their speed-ups are measured against the same serial time. The
# results are shortDelayResults.txt (the table) and the plot script the
# configuration names; server.log holds the output of the server (for mpi: of
# all ranks). compareSpeedups.py compares the tables of several runs,
# GSpeedupFigure draws them into one figure.
#
# Environment:
#   MPIRUN   how mpi is started, followed by the rank count (default
#            "mpirun --oversubscribe -np": Open MPI needs the flag when there are
#            more ranks than cores; MPICH's mpiexec knows no such flag)
####################################################################

set -u

if [ $# -lt 9 ]; then
    echo "Usage: ./runWithClients.sh <binary> <workdir> <consumer> <clients> <population>" \
         "<iterations> \"<delays>\" \"<reference delays>\" <measurements> [server options]"
    exit 1
fi

X=$(readlink -f "$1"); W=$2; C=$3; N=$4; POP=$5; ITER=$6; DELAYS=$7; REFDELAYS=$8; NMEAS=$9
shift 9
if [ ! -x "$X" ]; then
    echo "Error: $X is not an executable"; exit 1
fi
mkdir -p "$W" && cd "$W" || exit 1
rm -rf config && cp -r "$(dirname "$X")/config" config

python3 - "$POP" "$ITER" "$DELAYS" "$REFDELAYS" "$NMEAS" <<'P'
import json, sys
pop, iterations, delays, refdelays, nmeas = sys.argv[1:6]
f = 'config/GEvolutionaryAlgorithm.json'; d = json.load(open(f))
d['population']['size']['value'] = int(pop); d['max_iteration']['value'] = int(iterations)
d['max_stall_iteration']['value'] = 0; json.dump(d, open(f, 'w'), indent=1)
for f, dl in (('config/GDelayIndividual.json', delays),
              ('config/GDelayIndividual-reference.json', refdelays)):
    d = json.load(open(f)); d['delays']['value'] = dl; d['n_measurements']['value'] = int(nmeas)
    d['n_variables']['value'] = 100; json.dump(d, open(f, 'w'), indent=1)
P

if [ ! -s referenceLine.txt ] || [ "${REFRESH_REFERENCE:-0}" = 1 ]; then
    "$X" --reference > reference.log 2>&1; echo "reference exit: $?"
fi

if [ "$C" = mpi ]; then
    # The ranks take their roles from MPI; the server options reach every rank.
    # shellcheck disable=SC2086
    ${MPIRUN:-mpirun --oversubscribe -np} $((N + 1)) "$X" -c mpi "$@" > server.log 2>&1
    STATUS=$?
else
    PORT=$((15000 + RANDOM % 2000))
    "$X" -c "$C" "--${C}_port" $PORT "$@" > server.log 2>&1 &
    SRV=$!
    sleep 2
    PIDS=()
    for _ in $(seq 1 "$N"); do
        "$X" -c "$C" --client "--${C}_port" $PORT > /dev/null 2>&1 & PIDS+=($!)
    done
    wait $SRV; STATUS=$?
    kill "${PIDS[@]}" 2>/dev/null; wait "${PIDS[@]}" 2>/dev/null
fi
echo "server exit: $STATUS"
exit $STATUS
