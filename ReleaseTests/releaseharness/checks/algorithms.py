"""Optimization-algorithm checks (ReleaseTestplan.md: "Optimization algorithms").

Runs each algorithm to completion on a simple individual via example 01, which
selects the algorithm with ``--algorithm`` (ea/sa/swarm/gd/ps). The Go2 config
round-trip and checkpoint save/restore identity are structured stubs.
"""

from __future__ import annotations

import time

from ..model import CheckResult, Tier
from ..runner import GUEST_BUILD, JobContext

# Run from the example's own build dir so it finds/creates ./config/Go2.json.
_EX01_DIR = "libs/geneva/examples/01_GSimpleOptimizer"
_EX01 = "./GSimpleOptimizer"
_ALGOS = {
    "ea": "Evolutionary Algorithm",
    "sa": "Simulated Annealing",
    "swarm": "Swarm",
    "cgd": "Gradient Descent",
    "ps": "Parameter Scan",
}


def run_all_algorithms(ctx: JobContext) -> list[CheckResult]:
    """Smoke-run each algorithm (EA, SA, Swarm, GD, PS) end-to-end on the
    01_GSimpleOptimizer example.

    SHORT tier so this runs in --medium (the example's stall criterion converges
    in seconds, so all five together take a few minutes wall-clock; the 300 s
    per-algorithm cap is the safety net for stuck runs, not the typical case).
    """
    tier = Tier.SHORT
    if not ctx.job.spec.build_examples:
        return [ctx.skipped("algorithm/all", tier, "examples not built")]
    if not ctx.should_run(tier):
        return [ctx.skipped("algorithm/all", tier, "skipped")]
    out: list[CheckResult] = []
    for key in _ALGOS:
        started = time.monotonic()
        res = ctx.exec_in_guest(
            ["bash", "-lc",
             f"cd {GUEST_BUILD}/{_EX01_DIR} && "
             f"{_EX01} -a {key} -c stc"],
            workdir=GUEST_BUILD, timeout=300,
        )
        out.append(ctx.record(f"algorithm/{key}", tier, res, started=started))
    return out


def go2_config_roundtrip(ctx: JobContext) -> CheckResult:
    """Wipe config, let Geneva regenerate defaults, then re-run consuming them.

    GParserBuilder auto-writes a default config file when one is missing (see
    src/common/GParserBuilder.cpp around line 443). The round-trip is therefore:
    delete the example's config/ directory, run once (which triggers default
    generation and an immediate optimization run on those defaults), confirm
    the expected JSON files were written, run a second time (which now consumes
    the just-written defaults). Both runs must exit 0.
    """
    tier = Tier.SHORT
    if not ctx.job.spec.build_examples:
        return ctx.skipped("algorithm/go2-config-roundtrip", tier, "examples not built")
    if not ctx.should_run(tier):
        return ctx.skipped("algorithm/go2-config-roundtrip", tier, "skipped")
    started = time.monotonic()
    script = (
        "set -e\n"
        f"cd {GUEST_BUILD}/{_EX01_DIR}\n"
        # Keep the config/ DIRECTORY -- GParserBuilder writes default *files* when
        # they are missing but does not create the directory itself.
        "mkdir -p config && rm -f config/*.json\n"
        # First run: defaults missing -> GParserBuilder regenerates them, then
        # the optimization runs on those just-written values.
        f"{_EX01} >/tmp/cfg_run1.log 2>&1\n"
        'test -s config/Go2.json '
        '|| { echo "Go2.json was not regenerated"; head -20 /tmp/cfg_run1.log; exit 1; }\n'
        'echo "regenerated configs:"; ls config/ | sed "s/^/  /"\n'
        # Second run: consumes the freshly-written defaults (round-trip).
        f"{_EX01} >/tmp/cfg_run2.log 2>&1\n"
        'echo "config round-trip OK (both runs completed against regenerated defaults)"\n'
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=900)
    return ctx.record("algorithm/go2-config-roundtrip", tier, res, started=started)


def checkpoint_identity(ctx: JobContext) -> CheckResult:
    """Save a checkpoint mid-run, restore via `Go2 -f`, confirm it continues.

    With the default config Geneva's checkpoint_interval is 0 (never). The check
    therefore: (1) lets the example regenerate its default config, (2) flips
    cp_interval to 1 in the EA config so a checkpoint is written every iteration,
    (3) runs the example so checkpoint files accumulate under ./checkpoints/, (4)
    picks the most recent one, (5) re-runs with ``-f <cp_file>`` which makes Go2
    load that checkpoint before the run (see Go2::cp_personality_fits +
    loadCheckpoint in geneva/src/Go2.cpp). PASS requires both runs to exit 0 and
    the restore-side run NOT to print Geneva's "Checkpoint file ... does not fit"
    rejection -- i.e. the serialised state actually round-tripped through
    save/load. This is a high-value smoke for the recent
    serialise/load/localMembers() refactor.
    """
    tier = Tier.SHORT
    if not ctx.job.spec.build_examples:
        return ctx.skipped("algorithm/checkpoint-identity", tier, "examples not built")
    if not ctx.should_run(tier):
        return ctx.skipped("algorithm/checkpoint-identity", tier, "skipped")
    started = time.monotonic()
    script = (
        "set -e\n"
        f"cd {GUEST_BUILD}/{_EX01_DIR}\n"
        # Keep config/ DIRECTORY (GParserBuilder only writes files, not the dir).
        "mkdir -p config && rm -f config/*.json && rm -rf checkpoints\n"
        # Run once to regenerate the default config files.
        f"{_EX01} >/dev/null 2>&1\n"
        # Flip cp_interval inside the cp_interval block from 0 to 1 (a JSON number) so the
        # algorithm writes a checkpoint every iteration. awk is portable and
        # scoped to the block, so it cannot accidentally touch other "value":
        # "0" entries elsewhere in the JSON.
        "awk 'BEGIN{in_cp=0} "
        '/\"cp_interval\"/{in_cp=1} '
        'in_cp && /\"value\"/{sub(/: *0/, ": 1"); in_cp=0} '
        "{print}' config/GEvolutionaryAlgorithm.json > config/_cp.json && "
        "mv config/_cp.json config/GEvolutionaryAlgorithm.json\n"
        'grep -A8 cp_interval config/GEvolutionaryAlgorithm.json | grep value\n'
        # Save-side run: writes checkpoints to ./checkpoints/.
        "rm -rf checkpoints\n"
        f"{_EX01} >/tmp/cp_save.log 2>&1\n"
        "cps=$(ls -1t checkpoints/ 2>/dev/null | head -1)\n"
        '[ -n "$cps" ] || { echo "no checkpoint files written"; '
        'ls -la checkpoints/ 2>&1; tail -20 /tmp/cp_save.log; exit 1; }\n'
        'echo "saved checkpoint: $cps"\n'
        # Restore-side run: Go2 loads the checkpoint via -f before the run.
        f"{_EX01} -f \"checkpoints/$cps\" >/tmp/cp_restore.log 2>&1\n"
        # Reject "does not fit" -- that means the checkpoint failed to round-trip.
        'if grep -q "does not" /tmp/cp_restore.log; then\n'
        '  echo "CHECKPOINT RESTORE REJECTED:"; tail -20 /tmp/cp_restore.log; exit 1\n'
        "fi\n"
        'echo "checkpoint save/restore OK (cp_file accepted, restored run completed)"\n'
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=1200)
    return ctx.record("algorithm/checkpoint-identity", tier, res, started=started)
