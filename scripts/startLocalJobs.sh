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
# Starts a Geneva program once in server mode and a given number of times in
# client mode on the same host, using local (reliable, low-latency) networking.
# Useful for testing networked execution of a Geneva program -- including the
# submission path (broker / consumer / client round-trip) -- without needing a
# cluster.
#
# Usage:
#   ./startLocalJobs.sh <program> <n_clients> <port> [consumer] [-- <extra args>...]
#
#     program     Path to the Geneva executable.
#     n_clients   Number of client processes to start (>= 1). For the MPI consumer
#                 this is the number of WORKER ranks; mpirun then launches
#                 n_clients + 1 ranks (one rank-0 master plus n_clients workers).
#     port        TCP port the server listens on (> 1000). IGNORED for the MPI
#                 consumer (MPI uses no listening socket); pass any placeholder,
#                 e.g. 0.
#     consumer    Consumer mnemonic: "beast" (websocket, the default) or "mpi".
#                 For "beast" the program is started once in server
#                 mode plus n_clients times in client mode over local TCP. For
#                 "mpi" the program is launched ONCE via mpirun (one process that
#                 internally splits into the rank-0 master and the worker ranks);
#                 there are no separate client invocations and no port.
#     extra args  Everything after a literal "--" is forwarded VERBATIM to every
#                 program invocation (server, clients, and the mpirun program), in
#                 addition to the consumer/port/--client flags the script already
#                 supplies. Use it for options the script does not model itself --
#                 e.g. an algorithm chain (-a "ea,cgd") or a client prefetch depth
#                 (--beast_prefetchDepth=4). Flags meant for one role are harmless to
#                 the other (a client ignores -a; a server has no prefetch).
#
# Examples:
#   ./startLocalJobs.sh ./GSimpleOptimizer 15 10000 beast -- -a "ea,cgd" --beast_prefetchDepth=4
#
# Environment:
#   GEM_RUN_TIMEOUT      If set to a positive number of seconds, the script runs
#                        NON-interactively: it waits up to that long for the
#                        server (or, for MPI, the mpirun job) to finish, then tears
#                        everything down and exits with its exit code (124 on
#                        timeout). This makes the script usable as an automated
#                        integration test. If unset, the script tails the server
#                        output interactively (Ctrl-C to stop; all jobs are cleaned
#                        up on exit).
#   MPIRUN               Override the MPI launcher (default: mpirun). MPI only.
####################################################################

set -u

# ---- argument handling ------------------------------------------------------
# Split off any pass-through arguments after a literal "--": everything before it is
# parsed by the script, everything after it is forwarded verbatim to the program.
EXTRA_ARGS=()
POSITIONAL=()
seen_dashdash=0
for arg in "$@"; do
    if [ "${seen_dashdash}" -eq 1 ]; then
        EXTRA_ARGS+=("${arg}")
    elif [ "${arg}" = "--" ]; then
        seen_dashdash=1
    else
        POSITIONAL+=("${arg}")
    fi
done
set -- "${POSITIONAL[@]}"

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
    echo "Usage: ./startLocalJobs.sh <program> <n_clients> <port> [consumer: beast|mpi] [-- <extra args>...]"
    echo "       (for mpi the port is ignored and n_clients is the number of worker ranks)"
    echo "       (everything after -- is forwarded to every program invocation)"
    exit 1
fi

PROGNAME=$1
NCLIENTS=$2
PORT=$3
CONSUMER=${4:-beast}

if [ ! -x "${PROGNAME}" ] && [ ! -e "${PROGNAME}" ]; then
    echo "Error: program file '${PROGNAME}' does not exist."
    exit 1
fi
if ! echo "${NCLIENTS}" | grep -qE "^[0-9]+$" || [ "${NCLIENTS}" -lt 1 ]; then
    echo "Error: number of clients '${NCLIENTS}' must be an integer >= 1. Leaving."
    exit 1
fi
# The MPI consumer uses no TCP port (mpirun handles transport), so only validate the
# port for the socket consumers.
if [ "${CONSUMER}" != "mpi" ]; then
    if ! echo "${PORT}" | grep -qE "^[0-9]+$" || [ "${PORT}" -le 1000 ]; then
        echo "Error: port '${PORT}' must be an integer > 1000. Leaving."
        exit 1
    fi
fi

# Map the consumer mnemonic to its CLI option names (verified against the current
# courtier consumers: GWebsocketConsumerT -> "beast"/--beast_*, GMPIConsumerT -> "mpi").
# MPI is launched via mpirun rather than as server + clients.
case "${CONSUMER}" in
    beast) IP_OPT="beast_ip"; PORT_OPT="beast_port" ;;
    mpi)   ;; # handled separately below
    *)
        echo "Error: unknown consumer '${CONSUMER}'. Use 'beast' or 'mpi'. Leaving."
        exit 1
        ;;
esac

mkdir -p ./output

# Invoke the program as given if it contains a path separator, else as ./name.
case "${PROGNAME}" in
    */*) PROG="${PROGNAME}" ;;
    *)   PROG="./${PROGNAME}" ;;
esac

# ---- cleanup: never leave orphaned server/client processes behind -----------
SERVER_PID=""
CLIENT_PIDS=()
cleanup() {
    # Kill clients first, then the server. Ignore errors (already gone).
    for p in "${CLIENT_PIDS[@]}"; do
        kill "${p}" 2>/dev/null
    done
    [ -n "${SERVER_PID}" ] && kill "${SERVER_PID}" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

if [ "${CONSUMER}" = "mpi" ]; then
    # ---- MPI: a single mpirun launches 1 master (rank 0) + NCLIENTS worker ranks ----
    MPIRUN="${MPIRUN:-mpirun}"
    if ! command -v "${MPIRUN}" >/dev/null 2>&1; then
        echo "Error: MPI launcher '${MPIRUN}' not found in PATH. Leaving."
        exit 1
    fi
    NPROC=$((NCLIENTS + 1))
    SERVER_OUT=./output/output_mpirun
    echo "Launching ${MPIRUN} -np ${NPROC} (1 master + ${NCLIENTS} worker rank(s)) ..."
    ( "${MPIRUN}" -np "${NPROC}" "${PROG}" -c mpi "${EXTRA_ARGS[@]}" >& "${SERVER_OUT}" ) &
    SERVER_PID=$!
else
    # ---- socket consumers: one server process + NCLIENTS client processes ----
    SERVER_OUT=./output/output_server
    echo "Starting server (${CONSUMER}) on port ${PORT} ..."
    ( "${PROG}" -c "${CONSUMER}" "--${PORT_OPT}=${PORT}" "${EXTRA_ARGS[@]}" >& "${SERVER_OUT}" ) &
    SERVER_PID=$!

    # Give the server a moment to bind its listening socket before the clients connect.
    sleep 1

    echo "Starting ${NCLIENTS} client(s) ..."
    for i in $(seq 1 "${NCLIENTS}"); do
        ( "${PROG}" -c "${CONSUMER}" --client "--${IP_OPT}=localhost" "--${PORT_OPT}=${PORT}" \
            "${EXTRA_ARGS[@]}" >& "./output/output_client_${i}" ) &
        CLIENT_PIDS+=($!)
    done
fi

# ---- wait for completion ----------------------------------------------------
TIMEOUT="${GEM_RUN_TIMEOUT:-0}"
if echo "${TIMEOUT}" | grep -qE "^[0-9]+$" && [ "${TIMEOUT}" -gt 0 ]; then
    # Automated mode: wait up to TIMEOUT seconds for the server, then report.
    echo "Waiting up to ${TIMEOUT}s for the server to finish (automated mode) ..."
    waited=0
    while kill -0 "${SERVER_PID}" 2>/dev/null; do
        if [ "${waited}" -ge "${TIMEOUT}" ]; then
            echo "TIMEOUT: server did not finish within ${TIMEOUT}s." >&2
            exit 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "${SERVER_PID}"
    rc=$?
    echo "Server finished with exit code ${rc}."
    exit "${rc}"
else
    # Interactive mode: stream the server (or mpirun) output until the user interrupts.
    tail -f "${SERVER_OUT}"
fi
