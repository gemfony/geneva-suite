#!/usr/bin/env python3

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
"""Compares the speed-up tables (shortDelayResults.txt) of several runs of GParallelisationOverhead.

Usage: compareSpeedups.py [--tolerance T] <baseline table>... [--against <table>...]

Without --against it prints, for every evaluation time, the mean speed-up of the given runs and
the error of that mean: the larger of what the scatter of the runs gives and what the errors of
the single runs give, so that a few runs that agree by chance do not claim a precision they do
not have. With --against it does the same for the second group and reports, per evaluation time,
how far the two means are apart in units of their combined error. The exit code is 1 if the
second group is lower than the first by more than the tolerance (default 3) at any evaluation
time; being faster never fails.
"""
import math
import sys


def read(name):
    """Returns {evaluation time: (speed-up, its error)} of one table."""
    table = {}
    with open(name, encoding='utf-8') as lines:
        for line in lines:
            fields = line.split()
            if not fields or fields[0].startswith('#'):
                continue
            table[float(fields[0])] = (float(fields[5]), float(fields[6]))
    return table


def combine(tables):
    """Returns {evaluation time: (mean, standard error of the mean, number of runs)}."""
    result = {}
    for delay in sorted(set().union(*tables)):
        values = [t[delay] for t in tables if delay in t]
        runs = len(values)
        mean = sum(v for v, _ in values) / runs
        error = math.sqrt(sum(e * e for _, e in values)) / runs
        if runs > 1:
            scatter = math.sqrt(sum((v - mean) ** 2 for v, _ in values) / (runs - 1))
            error = max(error, scatter / math.sqrt(runs))
        result[delay] = (mean, error, runs)
    return result


def main(args):
    tolerance = 3.0
    if args and args[0] == '--tolerance':
        tolerance = float(args[1])
        args = args[2:]
    split = args.index('--against') if '--against' in args else len(args)
    first, second = args[:split], args[split + 1:]
    if not first:
        sys.exit(__doc__)
    base = combine([read(name) for name in first])
    if not second:
        print('| Evaluation time | Speed-up | Error of the mean | Runs |')
        print('|---|---|---|---|')
        for delay, (mean, error, runs) in base.items():
            print(f'| {delay:g} s | {mean:.1f} | {error:.2f} | {runs} |')
        return 0
    new = combine([read(name) for name in second])
    failed = False
    print('| Evaluation time | Baseline | Now | Difference | In combined errors | |')
    print('|---|---|---|---|---|---|')
    for delay, (mean, error, _) in base.items():
        if delay not in new:
            continue
        now, now_error, _ = new[delay]
        combined = math.hypot(error, now_error)
        distance = (now - mean) / combined if combined > 0 else 0.0
        slower = distance < -tolerance
        failed = failed or slower
        print(f'| {delay:g} s | {mean:.1f} ± {error:.2f} | {now:.1f} ± {now_error:.2f} | '
              f'{now - mean:+.1f} | {distance:+.1f} | {"SLOWER" if slower else ""} |')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
