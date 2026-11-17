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
#       <iterations> "<delays>" "<reference delays>" <measurements> [options]
#
#     binary       the GParallelisationOverhead (or GSubClientOverhead) executable
#     workdir      a directory OUTSIDE the source tree
#     consumer     the consumer to measure: a networked one such as beast (a
#                  server process and N client processes), or mpi (one mpirun
#                  with N + 1 ranks: rank 0 is the server, the others its clients;
#                  with GROUP > 1, 1 + N x GROUP ranks laid out in groups)
#     delays       the evaluation times, as "(seconds,milliseconds), ..."
#     reference delays   the (short) evaluation times of the serial reference
#     measurements how often each evaluation time is measured
#
# The options after the nine arguments go to every process: the server and each
# client (with mpi: every rank).
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
#   GROUP    processes per client group (default 1), for GSubClientOverhead: every
#            client shares its evaluations with GROUP - 1 sub-clients, in a group
#            of the MPI cascade (--group GROUP). With mpi the job has
#            1 + clients x GROUP ranks; with a networked consumer every client is
#            an MPI job of GROUP ranks of its own.
#   INTER_DELAY  seconds to wait after the measurements of an evaluation time
#            (the configuration's inter_measurement_delay, 1 unless set here)
#   POLL_PAUSES  GSubClientOverhead: min:max microseconds a process pauses between
#            two looks for a message (unset: the configuration's)
#   CLIENT_GRACE  seconds the networked clients have to end by themselves after
#            their server (default 30); the rest are ended and counted
#   EXPECT_CLIENTS  how many clients the server waits for before its first run
#            (--waitForClients; default: the number of clients started). The
#            measurement's clock starts when they are all connected, not when the
#            server starts. A consumer that cannot tell how many clients it has
#            is not waited for; its log says so.
#   CLIENT_WAIT_LIMIT  seconds the server waits for them (--clientWaitLimit,
#            default 600); with fewer clients by then the server ends with an error
#   LAST_CLIENT_DELAY  seconds the last networked client is started after the
#            server has begun to wait for its clients (default 0: with the others)
#   SERVER_PORT  the port the websocket server tries first (default: one drawn at random from
#            15000 to 16999); a port another process holds is replaced by another one
#   TIMEOUT_TREATMENT  the networked consumer's timeout treatment (the key
#            "treatment" of config/GNetworkedConsumer.json: adaptive, fixed or
#            wait_indefinitely; unset: the configuration's). A check that every
#            evaluation is done exactly once sets wait_indefinitely: under load an
#            adaptive lease may give an item to a second client.
#
# The program may be GParallelisationOverhead or GSubClientOverhead: the script
# sets the configuration of either individual. The output of the clients of
# this run goes to clients.log.
####################################################################

set -u

if [ $# -lt 9 ]; then
    echo "Usage: ./runWithClients.sh <binary> <workdir> <consumer> <clients> <population>" \
         "<iterations> \"<delays>\" \"<reference delays>\" <measurements> [options]"
    exit 1
fi

GROUP=${GROUP:-1}
X=$(readlink -f "$1"); W=$2; C=$3; N=$4; POP=$5; ITER=$6; DELAYS=$7; REFDELAYS=$8; NMEAS=$9
shift 9
if [ ! -x "$X" ]; then
    echo "Error: $X is not an executable"; exit 1
fi
mkdir -p "$W" && cd "$W" || exit 1
rm -rf config && cp -r "$(dirname "$X")/config" config

python3 - "$POP" "$ITER" "$DELAYS" "$REFDELAYS" "$NMEAS" "$GROUP" <<'P' || exit 1
import glob, json, os, sys
pop, iterations, delays, refdelays, nmeas, group = sys.argv[1:7]
f = 'config/GEvolutionaryAlgorithm.json'; d = json.load(open(f))
d['population']['size']['value'] = int(pop); d['max_iteration']['value'] = int(iterations)
d['max_stall_iteration']['value'] = 0; json.dump(d, open(f, 'w'), indent=1)
# The configuration of the individual and of its reference (GDelayIndividual's, or the one of
# GSubClientOverhead's individual, which has the same keys)
for f, dl in [(f, delays) for f in glob.glob('config/*Individual.json')] + \
             [(f, refdelays) for f in glob.glob('config/*Individual-reference.json')]:
    d = json.load(open(f)); d['delays']['value'] = dl; d['n_measurements']['value'] = int(nmeas)
    d['n_variables']['value'] = 100
    if 'INTER_DELAY' in os.environ:
        d['inter_measurement_delay']['value'] = int(os.environ['INTER_DELAY'])
    if os.environ.get('POLL_PAUSES') and 'poll_pause_min' in d:
        low, high = os.environ['POLL_PAUSES'].split(':')
        d['poll_pause_min']['value'] = int(low); d['poll_pause_max']['value'] = int(high)
    json.dump(d, open(f, 'w'), indent=1)
# The timeout treatment of the networked consumer
f = 'config/GNetworkedConsumer.json'
if os.environ.get('TIMEOUT_TREATMENT'):
    d = json.load(open(f)); d['treatment']['value'] = os.environ['TIMEOUT_TREATMENT']
    json.dump(d, open(f, 'w'), indent=1)
# Only GSubClientOverhead has sub-clients: its individual's file holds the poll pauses
if int(group) != 1 and not any('poll_pause_min' in json.load(open(f))
                               for f in glob.glob('config/*Individual.json')):
    sys.exit('this program has no sub-clients; GROUP must be 1')
P

if [ ! -s referenceLine.txt ] || [ "${REFRESH_REFERENCE:-0}" = 1 ]; then
    "$X" --reference > reference.log 2>&1; echo "reference exit: $?"
fi

# The server starts its clock when the clients it waits for are connected.
GATE=(--waitForClients "${EXPECT_CLIENTS:-$N}" --clientWaitLimit "${CLIENT_WAIT_LIMIT:-600}")
# The group size, for the processes of the groups (a program without groups does not know it)
GROUPOPT=()
if [ "$GROUP" -gt 1 ]; then
    GROUPOPT=(--group "$GROUP")
fi

if [ "$C" = mpi ]; then
    # The ranks take their roles from MPI (and with groups from the cascade); the options reach
    # every rank.
    # shellcheck disable=SC2086
    ${MPIRUN:-mpirun --oversubscribe -np} $((1 + N * GROUP)) "$X" -c mpi "${GATE[@]}" \
        "${GROUPOPT[@]}" "$@" > server.log 2>&1
    STATUS=$?
else
    # The port is drawn at random; another process on the host may hold it. A server that could
    # not bind it is started again on another one (SERVER_PORT: the first port to try).
    PORT=${SERVER_PORT:-$((15000 + RANDOM % 2000))}
    for _ in $(seq 1 10); do
        "$X" -c "$C" "--${C}_port" $PORT "${GATE[@]}" "$@" > server.log 2>&1 &
        SRV=$!
        sleep 2
        if kill -0 "$SRV" 2>/dev/null || ! grep -q 'could not bind to port' server.log; then
            break
        fi
        wait "$SRV" 2>/dev/null
        echo "port $PORT is taken; the server is started on another one"
        PORT=$((15000 + RANDOM % 2000))
    done
    # A client with sub-clients is an MPI job of its own; its rank 0 is the client.
    LAUNCH=()
    if [ "$GROUP" -gt 1 ]; then
        # shellcheck disable=SC2206
        LAUNCH=(${MPIRUN:-mpirun --oversubscribe -np} "$GROUP")
    fi
    # clients.log holds the clients of this run only
    : > clients.log
    PIDS=()
    for i in $(seq 1 "$N"); do
        if [ "$i" = "$N" ] && [ "${LAST_CLIENT_DELAY:-0}" != 0 ]; then
            # The delay counts from the moment the server begins to wait (its log says so), not
            # from its start, which may take a while on a busy host
            for _ in $(seq 1 600); do
                grep -q '^Waiting for [0-9]* clients' server.log 2>/dev/null && break
                sleep 0.1
            done
            sleep "$LAST_CLIENT_DELAY"
        fi
        "${LAUNCH[@]}" "$X" -c "$C" --client "--${C}_port" $PORT "${GROUPOPT[@]}" "$@" \
            >> clients.log 2>&1 &
        PIDS+=($!)
    done
    wait $SRV; STATUS=$?
    # The clients end by themselves once their server is gone; those still there after the grace
    # period are ended and counted.
    left() { local n=0 p; for p in "${PIDS[@]}"; do kill -0 "$p" 2>/dev/null && n=$((n + 1)); done; echo $n; }
    for _ in $(seq 1 "${CLIENT_GRACE:-30}"); do
        [ "$(left)" = 0 ] && break
        sleep 1
    done
    echo "clients that did not end by themselves: $(left)"
    kill "${PIDS[@]}" 2>/dev/null; wait "${PIDS[@]}" 2>/dev/null
fi
echo "server exit: $STATUS"
exit $STATUS
