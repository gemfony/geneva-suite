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
"""Turns the RESULT lines of measureIterationTime.sh into a table.

Usage: summarizeIterationTimes.py <results file>...

One row per variant, transport, number of clients, population and evaluation time: the median
of the run medians with their spread over the runs, the overhead over the ideal iteration time,
the largest single iteration, and the mean CPU figures. The ideal is the evaluation time
multiplied by the number of items a client has to take in turn.
"""
import collections
import re
import sys

LINE = re.compile(r'RESULT run=\d+ (\S+) (\S+) clients=(\d+) pop=(\d+) '
                  r'delay=\((\d+),(\d+)\) iters=(\d+) (.*)')


def main(files):
    rows = collections.OrderedDict()
    for name in files:
        with open(name, encoding='utf-8') as lines:
            for line in lines:
                found = LINE.match(line)
                if not found:
                    continue
                label, consumer, clients, pop, sec, msec, _, rest = found.groups()
                key = (label, consumer, int(clients), int(pop), int(sec) * 1000 + int(msec))
                rows.setdefault(key, []).append(dict(re.findall(r'(\w+)=([-\d.]+)', rest)))

    print('| Variant | Transport | Clients | Evaluation | Ideal | Runs | Median iteration '
          '(spread of the run medians) | Overhead | Largest iteration | Server CPU | '
          'All processes |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for (label, consumer, clients, pop, msec), runs in rows.items():
        ideal = -(-(pop - 2) // clients) * msec / 1000.0
        head = f'| {label} | {consumer} | {clients} | {msec} ms | {ideal * 1000:.0f} ms '
        good = [run for run in runs if 'median' in run]
        if not good:
            print(head + f'| {len(runs)} | too few iterations | | | | |')
            continue
        medians = sorted(float(run['median']) for run in good)
        median = medians[len(medians) // 2]
        largest = max(float(run['max']) for run in good)
        server = sum(float(run['serverCPU']) for run in good) / len(good)
        everyone = sum(float(run['allCPU']) for run in good) / len(good)
        print(head + f'| {len(good)} | {median * 1000:.1f} ms ({medians[0] * 1000:.1f} .. '
              f'{medians[-1] * 1000:.1f}) | {(median - ideal) * 1000:+.1f} ms | '
              f'{largest * 1000:.0f} ms | {server:.1f} % | {everyone:.0f} % |')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
