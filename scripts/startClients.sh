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
# Starts a Geneva program a given number of times in client mode, connecting to
# an already-running server (started elsewhere, e.g. via startLocalJobs.sh or on
# another host). No server is started here. Useful for testing networked
# execution under real-world conditions.
#
# Usage:
#   ./startClients.sh <program> <n_clients> <ip/hostname> <port> [consumer]
#
#     consumer   Consumer mnemonic: "beast" (websocket, the default).
####################################################################

set -u

if [ $# -lt 4 ] || [ $# -gt 5 ]; then
    echo "Usage: ./startClients.sh <program> <n_clients> <ip/hostname> <port> [consumer: beast]"
    exit 1
fi

PROGNAME=$1
NCLIENTS=$2
IP=$3
PORT=$4
CONSUMER=${5:-beast}

if [ ! -x "${PROGNAME}" ] && [ ! -e "${PROGNAME}" ]; then
    echo "Error: program file '${PROGNAME}' does not exist."
    exit 1
fi
if ! echo "${NCLIENTS}" | grep -qE "^[0-9]+$" || [ "${NCLIENTS}" -lt 1 ]; then
    echo "Error: number of clients '${NCLIENTS}' must be an integer >= 1. Leaving."
    exit 1
fi
if ! echo "${PORT}" | grep -qE "^[0-9]+$" || [ "${PORT}" -le 1000 ]; then
    echo "Error: port '${PORT}' must be an integer > 1000. Leaving."
    exit 1
fi

# Map the consumer mnemonic to its CLI option names (verified against the current
# courtier consumers: GWebsocketConsumerT -> "beast"/--beast_*).
case "${CONSUMER}" in
    beast) IP_OPT="beast_ip"; PORT_OPT="beast_port" ;;
    *)
        echo "Error: unknown consumer '${CONSUMER}'. Use 'beast'. Leaving."
        exit 1
        ;;
esac

mkdir -p ./output

# Invoke the program as given if it contains a path separator, else as ./name.
case "${PROGNAME}" in
    */*) PROG="${PROGNAME}" ;;
    *)   PROG="./${PROGNAME}" ;;
esac

# Clean up any clients we started if this script is interrupted.
CLIENT_PIDS=()
cleanup() {
    for p in "${CLIENT_PIDS[@]}"; do
        kill "${p}" 2>/dev/null
    done
    wait 2>/dev/null
}
trap cleanup INT TERM

echo "Starting ${NCLIENTS} ${CONSUMER} client(s), connecting to ${IP}:${PORT} ..."
for i in $(seq 1 "${NCLIENTS}"); do
    ( "${PROG}" -c "${CONSUMER}" --client "--${IP_OPT}=${IP}" "--${PORT_OPT}=${PORT}" \
        >& "./output/output_client_${i}" ) &
    CLIENT_PIDS+=($!)
done

# Wait for all clients to finish (they exit when the server signals completion).
wait
