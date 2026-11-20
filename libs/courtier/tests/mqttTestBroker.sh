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
# Starts or stops the MQTT broker of courtier's tests: a mosquitto of its own, on a free port of the
# loopback interface -- never the host's broker. A CTest fixture runs it (tests/CMakeLists.txt):
#
#   mqttTestBroker.sh start <mosquitto> <dir> [<port>]   writes <dir>/port and <dir>/pid
#   mqttTestBroker.sh stop  <mosquitto> <dir>            ends the broker of <dir>/pid
#
# With a port the broker is started on that one (a test that restarts its own broker where its
# clients look for it), waiting up to five seconds for it to be free; without one on a free one.
#
# The tests that use it read <dir>/port; each uses a topic prefix of its own, so that tests that run
# at the same time do not see each other. The broker runs without a configuration file
# (mosquitto -p): where mosquitto is confined by an AppArmor profile (Ubuntu 26.10 ships one), it
# may read a configuration only from /etc/mosquitto, so none of the build directory can be used,
# and no setting either (set_tcp_nodelay, for one, stays off). Without one it listens on the
# loopback interface only and takes packets of up to 2,000,000 bytes (mosquitto 2.1). Its output
# goes to <dir>/broker.log.
################################################################################

set -u

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
    echo "Usage: mqttTestBroker.sh start|stop <mosquitto> <dir> [<port>]"; exit 2
fi
WANTED=${4:-}
ACTION=$1; MOSQUITTO=$2; DIR=$3
mkdir -p "$DIR" || exit 1

# Whether something listens on port $1 of the loopback interface
listening() {
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

# Ends the broker of the pid file, if it is one of ours and runs; waits up to five seconds
stop_broker() {
    local pid
    pid=$(cat "$DIR/pid" 2>/dev/null)
    : > "$DIR/pid"
    if [ -z "$pid" ] || [ "$(cat "/proc/$pid/comm" 2>/dev/null)" != mosquitto ]; then
        return 0
    fi
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    echo "the test broker (pid $pid) did not end; it is killed"
    kill -9 "$pid" 2>/dev/null
    return 0
}

case "$ACTION" in
start)
    stop_broker # one left behind by a run that was ended from outside
    : > "$DIR/port"
    for _ in $(seq 1 50); do
        PORT=${WANTED:-$((20000 + RANDOM % 20000))}
        if listening "$PORT"; then
            [ -n "$WANTED" ] && sleep 0.1
            continue
        fi
        setsid "$MOSQUITTO" -p "$PORT" >> "$DIR/broker.log" 2>&1 < /dev/null &
        PID=$!
        for _ in $(seq 1 50); do
            if listening "$PORT"; then
                echo "$PID" > "$DIR/pid"
                echo "$PORT" > "$DIR/port"
                echo "the test broker listens on 127.0.0.1:$PORT (pid $PID)"
                exit 0
            fi
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.1
        done
        kill "$PID" 2>/dev/null
    done
    echo "no test broker could be started; see $DIR/broker.log"
    exit 1
    ;;
stop)
    stop_broker
    : > "$DIR/port"
    echo "the test broker has ended"
    ;;
*)
    echo "Usage: mqttTestBroker.sh start|stop <mosquitto> <dir> [<port>]"; exit 2
    ;;
esac
