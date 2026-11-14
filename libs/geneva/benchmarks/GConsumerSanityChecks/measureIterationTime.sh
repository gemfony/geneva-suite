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
# Measures the time one iteration takes when N clients serve a Geneva server, and
# the CPU the server needs for it. The program is GConsumerSanityChecks: its
# individuals sleep instead of calculating, so the time above the sleep is what
# the submission infrastructure and the transport add.
#
# Usage:
#   ./measureIterationTime.sh <binary> <workdir> <outfile> <label> <runs> <spec>...
#
#     binary    the GConsumerSanityChecks executable of a build tree
#     workdir   a directory OUTSIDE the source tree; the run's configuration and
#               log are written there
#     outfile   one RESULT line per run is appended to it
#     label     a name for the variant measured (first field of the RESULT line)
#     runs      how often each specification is measured
#     spec      consumer:clients:seconds:milliseconds:duration[:population]
#               e.g. beast:400:0:10:30 -- 400 websocket clients, evaluations of
#               10 ms, measured for 30 s. The population defaults to
#               2*clients+2, which gives every client two items per iteration,
#               so the ideal iteration time is twice the evaluation time.
#
# Environment:
#   TREATMENT  timeout treatment written to config/GNetworkedConsumer.json
#              (adaptive | fixed | wait_indefinitely); a test of "nothing is
#              lost" wants wait_indefinitely, or a lost item is resubmitted
#              after the give-up window and the loss stays hidden
#   SRVOPTS    further options for the server (for MPI: for every rank)
#   MPISTART   seconds to wait after mpirun before rank 0 is looked up (8)
#
# What a RESULT line holds: the number of iterations seen, the smallest, median,
# largest and mean iteration time in seconds (the first two iterations are left
# out), the age of the last iteration line when the run was stopped (a batch
# that hangs shows as a large age), the CPU use of the server process (for MPI:
# of rank 0) over the last ten seconds in percent of one core, with the raw
# clock ticks, the same for all processes of the benchmark together, and the
# name and thread count of the process that was measured.
#
# The runs take place one after another. Nothing else should run on the host,
# and no build. summarizeIterationTimes.py turns the RESULT lines into a table.
####################################################################

set -u
export LC_ALL=C

if [ $# -lt 6 ]; then
    echo "Usage: ./measureIterationTime.sh <binary> <workdir> <outfile> <label> <runs> <spec>..."
    echo "       spec = consumer:clients:seconds:milliseconds:duration[:population]"
    exit 1
fi

X=$(readlink -f "$1"); W=$2; OUT=$(readlink -f "$3"); LABEL=$4; RUNS=$5; shift 5
if [ ! -x "$X" ]; then
    echo "Error: $X is not an executable"; exit 1
fi
mkdir -p "$W" && W=$(readlink -f "$W") && cd "$W" || exit 1
NAME=$(basename "$X" | cut -c1-15) # the name the kernel keeps for the process

# CPU time of one process, and of all processes of the benchmark, in clock ticks
ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
allticks() { local s=0 p; for p in $(pgrep -x "$NAME"); do s=$((s + $(ticks "$p"))); done; echo $s; }
# puts the arrival time in front of every line of the server's output
stamp() { while IFS= read -r l; do printf '%s %s\n' "$(date +%s.%N)" "$l"; done > srv.log; }

measure() { # consumer clients population "(s,ms)" duration
    local C=$1 N=$2 POP=$3 DELAY=$4 SECS=$5 PIDS=() WRAP S PORT t0 t1 a0 a1 TEND NTHR COMM WARN=""
    rm -rf config srv.log && cp -r "$(dirname "$X")/config" config
    python3 - "$POP" "$DELAY" <<'P'
import json, os, sys
f = 'config/GEvolutionaryAlgorithm.json'; d = json.load(open(f))
d['population']['size']['value'] = int(sys.argv[1]); d['max_iteration']['value'] = 100000
d['max_stall_iteration']['value'] = 0; json.dump(d, open(f, 'w'), indent=1)
f = 'config/GDelayIndividual.json'; d = json.load(open(f))
d['delays']['value'] = sys.argv[2]; json.dump(d, open(f, 'w'), indent=1)
if 'TREATMENT' in os.environ:
    f = 'config/GNetworkedConsumer.json'; d = json.load(open(f))
    d['treatment']['value'] = os.environ['TREATMENT']; json.dump(d, open(f, 'w'), indent=1)
P
    # stdbuf need not replace itself by the program (uutils coreutils does not), so the process to
    # measure is looked up instead of taken from $!.
    if [ "$C" = mpi ]; then
        # shellcheck disable=SC2086
        mpirun --oversubscribe -np $((N + 1)) stdbuf -oL "$X" -c mpi ${SRVOPTS:-} > >(stamp) 2>&1 &
        WRAP=$!
        sleep "${MPISTART:-8}"
        S=$(for p in $(pgrep -x "$NAME"); do
                tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx 'OMPI_COMM_WORLD_RANK=0' && echo "$p"
            done | head -1)
    else
        PORT=$((11000 + RANDOM % 2000))
        # shellcheck disable=SC2086
        stdbuf -oL "$X" -c "$C" "--${C}_port" $PORT ${SRVOPTS:-} > >(stamp) 2>&1 &
        WRAP=$!
        sleep 2
        S=$(pgrep -P $WRAP -x "$NAME" | head -1)
        for _ in $(seq 1 "$N"); do
            "$X" -c "$C" --client "--${C}_port" $PORT > /dev/null 2>&1 & PIDS+=($!)
        done
    fi
    if [ -z "${S:-}" ]; then S=$WRAP; WARN=" WRAPPER-MEASURED"; fi

    sleep $((SECS - 10)); t0=$(ticks "$S"); a0=$(allticks)
    sleep 10; t1=$(ticks "$S"); a1=$(allticks)
    TEND=$(date +%s.%N)
    NTHR=$(ls "/proc/$S/task" 2>/dev/null | wc -l); COMM=$(cat "/proc/$S/comm" 2>/dev/null)
    kill -INT "${PIDS[@]}" "$S" "$WRAP" 2>/dev/null; sleep 3
    kill "${PIDS[@]}" "$S" "$WRAP" 2>/dev/null; wait "${PIDS[@]}" "$WRAP" 2>/dev/null
    pkill -x "$NAME" 2>/dev/null

    python3 - "$TEND" "$LABEL" "$C" "$N" "$POP" "$DELAY" "$((t1 - t0))" "$((a1 - a0))" \
        "$(getconf CLK_TCK)" "$COMM/$NTHR$WARN" <<'P'
import sys
tend, label, c, n, pop, delay, st, at, hz, who = sys.argv[1:]
# Only what the server printed up to the end of the measurement counts; it may run on for a
# moment after the signal that ends it.
ts = [float(l.split()[0]) for l in open('srv.log') if 'best past' in l]
ts = [t for t in ts if t <= float(tend)]
d = sorted(b - a for a, b in zip(ts[2:], ts[3:]))
scpu = int(st) * 100.0 / (int(hz) * 10); acpu = int(at) * 100.0 / (int(hz) * 10)
head = "%s %s clients=%s pop=%s delay=%s iters=%d" % (label, c, n, pop, delay, len(ts))
if d:
    print("%s min=%.4f median=%.4f max=%.4f mean=%.4f lastAge=%.3f serverCPU=%.1f%% (%s ticks) "
          "allCPU=%.0f%% who=%s" % (head, d[0], d[len(d) // 2], d[-1], sum(d) / len(d),
                                    float(tend) - ts[-1], scpu, st, acpu, who))
else:
    print("%s TOO-FEW serverCPU=%.1f%% who=%s" % (head, scpu, who))
P
}

for spec in "$@"; do
    IFS=: read -r C N DS DM SECS POP <<< "$spec"
    POP=${POP:-$((2 * N + 2))}
    for r in $(seq 1 "$RUNS"); do
        echo "RESULT run=$r $(measure "$C" "$N" "$POP" "($DS,$DM)" "$SECS")" | tee -a "$OUT"
        sleep 2
    done
done
