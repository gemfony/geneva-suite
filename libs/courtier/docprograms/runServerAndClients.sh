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
# Runs a program on courtier's launcher with a networked consumer as one server and its clients,
# each a process of its own, on this host (the ctest entries of grid_minimum):
#
#   runServerAndClients.sh <program> <consumer> <clients> <broker-dir|-> [options...]
#
# consumer    beast: the server listens on a port drawn from 30000-32767; mqtt: server and
#             clients meet on the broker whose port <broker-dir>/port names (the tests' broker,
#             tests/mqttTestBroker.sh), under a topic prefix of this run's own
# options     go to the server and to every client
#
# The server waits for its clients before it submits anything (the launcher's --waitForClients
# <clients>, at most 60 s): a client that came after the server had finished would wait for it
# forever. The clients are started once the server waits for them -- on that line of its output,
# not after a fixed pause.
# LAST_CLIENT_DELAY  (environment) seconds the last client is started after the others (default 0)
#
# The server must end with 0 within 120 s, the clients within 30 s after it. The script starts
# in, and its programs write into, the current directory (the server's output: server.log, shown
# at the end; client i's: client<i>.log).

set -u
program=$1; consumer=$2; clients=$3; broker_dir=$4; shift 4

case $consumer in
    beast) where=(--beast_ip 127.0.0.1 --beast_port $((30000 + RANDOM % 2768))) ;;
    mqtt)  where=(--mqtt_ip 127.0.0.1 --mqtt_port "$(cat "$broker_dir/port")"
                  --mqtt_topic "gridmin$$-$RANDOM") ;;
    *)     echo "runServerAndClients.sh: no networked consumer: $consumer"; exit 2 ;;
esac

timeout 120 "$program" --consumer "$consumer" "${where[@]}" --waitForClients "$clients" \
    --clientWaitLimit 60 "$@" > server.log 2>&1 &
server=$!
for ((t = 0; t < 300; t++)); do
    grep -q '^Waiting for [0-9]* clients' server.log 2> /dev/null && break
    kill -0 "$server" 2> /dev/null || break
    sleep 0.1
done
client_pids=()
for ((i = 0; i < clients; i++)); do
    if ((i == clients - 1)); then sleep "${LAST_CLIENT_DELAY:-0}"; fi
    timeout 150 "$program" --consumer "$consumer" --client "${where[@]}" "$@" > "client$i.log" 2>&1 &
    client_pids+=($!)
done

wait "$server"; server_exit=$?
cat server.log
failed=0
for pid in "${client_pids[@]}"; do
    # A client ends when its server has gone; give it 30 s
    for ((t = 0; t < 30 && $(kill -0 "$pid" 2> /dev/null; echo $?) == 0; t++)); do sleep 1; done
    if kill -0 "$pid" 2> /dev/null; then kill "$pid"; failed=1; echo "a client did not end"; fi
    wait "$pid" || failed=1
done
echo "server exit $server_exit, clients $([ $failed = 0 ] && echo ok || echo failed)"
[ "$server_exit" = 0 ] && [ "$failed" = 0 ]
