#!/bin/sh
# Usage: checkpointRestart.sh limit|kill <limit> <ea-config> <checkpoint-dir> [<config-dir>] --
#            <command> [arguments...]
#
# A batch job that is started again from its last checkpoint. <command> is the MPI job (the
# launcher with its program and arguments); <ea-config> and <checkpoint-dir> are the submitter's
# configuration file of the evolutionary algorithm and its checkpoint directory, relative to a run
# directory that this script creates below the current one. A <config-dir> is copied there first,
# as config/ (for ranks that share the run directory and would otherwise create the same files at
# the same time). The script writes the configuration of the algorithm (a checkpoint every
# iteration, no stall limit, a small population) and runs two jobs:
#
#   limit: job 1 ends by its iteration limit <limit>; job 2 starts from its newest checkpoint (the
#          final one) with the limit <limit> + 2.
#   kill:  job 1 has no reachable limit and is killed (SIGKILL to the launcher, then to what is left
#          of the job, as a batch system does) once it has written a checkpoint; job 2 starts from
#          the newest checkpoint, with a limit of that checkpoint's iteration + <limit>.
#
# Job 2 must end with 0, start at the iteration after the checkpoint's, run on to its own limit, and
# end with a best result that is not worse than the checkpoint's (minimisation). Every line of the
# verdict starts with OK: or FAIL:. The run directory is removed when the test passes.
mode=$1; limit=$2; ea_config=$3; cp_dir=$4; shift 4
config_dir=
if [ "$1" != "--" ]; then config_dir=$1; shift; fi
[ "$1" = "--" ] && [ $# -gt 1 ] || {
    echo "FAIL: usage: $0 limit|kill <limit> <ea-config> <checkpoint-dir> [<config-dir>] -- <command>"
    exit 2
}
shift

run=$(mktemp -d "$PWD/run-XXXXXX") || exit 2
cd "$run" || exit 2
fail() { echo "FAIL: $*"; echo "FAIL: the files of the run are in $run"; exit 1; }

[ -z "$config_dir" ] || cp -r "$config_dir" config || exit 2
mkdir -p "$(dirname "$ea_config")"
cat > "$ea_config" <<'JSON'
{
  "population": { "size": { "value": 8 }, "n_parents": { "value": 2 } },
  "max_stall_iteration": { "value": 0 },
  "cp_interval": { "value": 1 }
}
JSON

# The iterations a job reported ("<iteration>: (<best>, ...) // best past: ..."), first and last
iterations() { grep -E '^[0-9]+: \(' "$1" | cut -d: -f1; }
# The checkpoint files of a directory, without the temporary files of a write that was cut off
checkpoints() { ls "$1" 2>/dev/null | grep '^checkpoint-'; }
# The iteration and the best fitness a checkpoint holds (its content, and its name)
cp_iteration() { grep -o '"iteration_":[0-9]*' "$1" | cut -d: -f2; }
cp_fitness() { basename "$1" | sed -e 's/^checkpoint-[^-]*-[^-]*-//' -e 's/-[^-]*$//'; }

# --- job 1
if [ "$mode" = limit ]; then
    timeout -k 10 60 "$@" --eaMaxIterations "$limit" > job1.log 2>&1 || fail "job 1 ended with $?"
else
    # A session of its own, so that the job can be ended as a whole; its launcher's pid from inside
    setsid sh -c 'echo $$ > launcher.pid; exec "$@"' sh "$@" --eaMaxIterations 1000000 > job1.log 2>&1 &
    end_job1() {
        [ -s launcher.pid ] || return 0
        kill -KILL "$(cat launcher.pid)" 2>/dev/null
        left=$(ps -o pid= -s "$(cat launcher.pid)")
        [ -n "$left" ] && kill -KILL $left 2>/dev/null
        i=0; while [ -n "$(ps -o pid= -s "$(cat launcher.pid)")" ] && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done
        [ -z "$(ps -o pid= -s "$(cat launcher.pid)")" ] || fail "job 1 is still there 20 s after its end"
        rm -f launcher.pid
    }
    trap 'end_job1' EXIT INT TERM
    i=0; while [ -z "$(checkpoints "$cp_dir")" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
    [ -n "$(checkpoints "$cp_dir")" ] || fail "job 1 wrote no checkpoint within 30 s"
    sleep 0.3
    end_job1
    trap - EXIT INT TERM
fi

# --- the newest checkpoint of job 1, moved aside so that job 2 writes its own beside nothing else
newest=$(checkpoints "$cp_dir" | grep -e '-final-' | head -n 1)
[ -n "$newest" ] || newest=$(checkpoints "$cp_dir" | sort -t- -k3,3n | tail -n 1)
[ -n "$newest" ] || fail "job 1 left no checkpoint"
mkdir job1-checkpoints && mv "$cp_dir/$newest" job1-checkpoints/ || exit 2
checkpoint=$PWD/job1-checkpoints/$newest
from=$(cp_iteration "$checkpoint")
[ -n "$from" ] || fail "the checkpoint $newest cannot be read (no iteration in it)"
if [ "$mode" = limit ]; then
    [ "$(iterations job1.log | tail -n 1)" = "$((limit - 1))" ] || fail "job 1 did not run to its limit"
    [ "$from" = "$((limit - 1))" ] || fail "the final checkpoint holds iteration $from, not $((limit - 1))"
    limit2=$((limit + 2))
else
    limit2=$((from + limit))
fi
echo "OK: job 1 left $newest (iteration $from)"

# --- job 2
timeout -k 10 60 "$@" --eaMaxIterations "$limit2" --cp_file "$checkpoint" > job2.log 2>&1 || fail "job 2 ended with $?"
first=$(iterations job2.log | head -n 1); last=$(iterations job2.log | tail -n 1)
[ "$first" = "$((from + 1))" ] || fail "job 2 started at iteration $first, not $((from + 1))"
[ "$last" = "$((limit2 - 1))" ] || fail "job 2 ended at iteration $last, not $((limit2 - 1))"
final2=$(checkpoints "$cp_dir" | grep -e '-final-' | head -n 1)
[ -n "$final2" ] || fail "job 2 wrote no final checkpoint"
awk -v a="$(cp_fitness "$final2")" -v b="$(cp_fitness "$checkpoint")" 'BEGIN { exit !(a + 0 <= b + 0) }' ||
    fail "job 2's best $(cp_fitness "$final2") is worse than the checkpoint's $(cp_fitness "$checkpoint")"
echo "OK: job 2 ran iterations $first to $last; best $(cp_fitness "$final2") (checkpoint: $(cp_fitness "$checkpoint"))"
cd .. && rm -r "$run"
