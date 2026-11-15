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
"""Checks the tools of the speed-up benchmark; registered as tests of the build.

Usage:
  testSpeedupTools.py figure <GSpeedupFigure> <workdir>
      draws prepared tables of two labels (two runs of the first) and checks the figure and that
      the means it shows are those of compareSpeedups.py
  testSpeedupTools.py run <GParallelisationOverhead> <workdir> <consumer> [<reference dir>]
      a short run of runWithClients.sh with two clients; with a reference dir, the serial reference
      of that earlier run is used instead of a new one (MPIRUN in the environment says how mpi is
      started)
  testSpeedupTools.py refusal <GParallelisationOverhead> <workdir>
      a serial reference of two measurements is refused with a message that says why

The work directory is created; it must lie outside the source tree.
"""
import importlib.util
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = ('# evaluation time [s]; serial run time [s] (fitted) and its error; parallel run time '
          '[s] and its error; speed-up and its error\n')


def fail(message):
    """Ends the test with a message."""
    sys.exit('FAILED: ' + message)


def compare_speedups():
    """Loads compareSpeedups.py as a module, so that its rule of combining runs can be called."""
    spec = importlib.util.spec_from_file_location('compareSpeedups',
                                                  os.path.join(HERE, 'compareSpeedups.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_table(name, rows):
    """Writes a speed-up table; rows are (evaluation time, speed-up, its error)."""
    with open(name, 'w', encoding='utf-8') as out:
        out.write(HEADER)
        for time, speedup, error in rows:
            out.write(f'{time}\t1\t0.1\t1\t0.1\t{speedup}\t{error}\n')


def check_figure(program, workdir):
    """Draws prepared tables and checks the figure and the means behind it."""
    os.makedirs(workdir, exist_ok=True)
    os.chdir(workdir)
    # Two runs of "fast": at 0.01 s they disagree, so their scatter makes the error; at 0.1 s they
    # agree, so the errors of the single runs make it. "slow" is one run with a third time.
    write_table('fast-1.txt', [(0.01, 3.0, 0.1), (0.1, 3.8, 0.05)])
    write_table('fast-2.txt', [(0.01, 3.4, 0.1), (0.1, 3.81, 0.05)])
    write_table('slow.txt', [(0.01, 1.0, 0.2), (0.1, 2.0, 0.1), (1, 3.9, 0.01)])
    run = subprocess.run([program, '--ideal', '4', '--figure', 'figure.C', '--means', 'means.txt',
                          'fast=fast-1.txt', 'fast=fast-2.txt', 'slow=slow.txt'],
                         capture_output=True, text=True, check=False)
    if run.returncode != 0:
        fail(f'GSpeedupFigure exited with {run.returncode}: {run.stderr}')

    figure = open('figure.C', encoding='utf-8').read()
    # Two pads (all times, and those up to 0.1 s), each with both labels and the ideal line
    for text, count in (('TGraphErrors(', 6), ('BuildLegend', 2), ('Divide(1,2)', 1),
                        ('"All evaluation times: fast"', 1), ('"Up to 0.1 s: fast"', 1),
                        ('"slow"', 2), ('"ideal (4)"', 2)):
        if figure.count(text) != count:
            fail(f'the figure holds "{text}" {figure.count(text)} times instead of {count}')
    if 'SetLogx' in figure or 'SetLogy' in figure:
        fail('the figure has a logarithmic axis')

    means = {}
    for line in open('means.txt', encoding='utf-8'):
        if not line.startswith('#'):
            label, time, mean, error, runs = line.split('\t')
            means[(label, float(time))] = (float(mean), float(error), int(runs))
    reference = compare_speedups()
    for label, tables in (('fast', ['fast-1.txt', 'fast-2.txt']), ('slow', ['slow.txt'])):
        for time, (mean, error, runs) in reference.combine([reference.read(t) for t in tables]).items():
            got = means.get((label, time))
            if got is None:
                fail(f'no mean for {label} at {time} s')
            if (not math.isclose(got[0], mean, rel_tol=1e-12) or
                    not math.isclose(got[1], error, rel_tol=1e-12) or got[2] != runs):
                fail(f'{label} at {time} s: GSpeedupFigure gives {got}, compareSpeedups.py '
                     f'{(mean, error, runs)}')
    if len(means) != 5:
        fail(f'{len(means)} means instead of 5')

    bad = subprocess.run([program, 'fast=missing.txt'], capture_output=True, text=True,
                         check=False)
    if bad.returncode == 0 or 'missing.txt' not in bad.stderr:
        fail('a missing table is not refused with its name')
    print('figure and means as expected')


def run_script(program, workdir, consumer, rows, reference_delays):
    """Runs runWithClients.sh with two clients and returns its exit code."""
    return subprocess.run(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                           consumer, '2', '4', '2', rows, reference_delays, '1'],
                          check=False, timeout=240).returncode


def check_run(program, workdir, consumer, reference_dir):
    """A short run of runWithClients.sh, with a reference of its own or a shared one."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'reference.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    if reference_dir:
        shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    status = run_script(program, workdir, consumer, '(0,10)', '(0,10), (0,20), (0,30)')
    if status != 0:
        fail(f'runWithClients.sh exited with {status}; see {workdir}/server.log')
    if reference_dir and os.path.exists(os.path.join(workdir, 'reference.log')):
        fail('the shared reference was measured again')
    rows = [l.split() for l in open(os.path.join(workdir, 'shortDelayResults.txt'),
                                    encoding='utf-8') if not l.startswith('#')]
    if len(rows) != 1 or float(rows[0][0]) != 0.01 or not float(rows[0][5]) > 0.:
        fail(f'unexpected table {rows}')
    if reference_dir:
        line = open(os.path.join(reference_dir, 'referenceLine.txt'), encoding='utf-8').read()
        a, _, b, _ = (float(v) for v in line.split())
        if not math.isclose(float(rows[0][1]), a + b * 0.01, rel_tol=1e-5):
            fail('the table does not use the shared reference')
    print(f'{consumer}: speed-up {rows[0][5]} with two clients')


def check_refusal(program, workdir):
    """A reference of two measurements cannot give its line an error and must be refused."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('referenceLine.txt', 'reference.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    status = run_script(program, workdir, 'beast', '(0,10)', '(0,10), (0,20)')
    log = open(os.path.join(workdir, 'reference.log'), encoding='utf-8').read()
    if status == 0 or 'it needs at least 3' not in log:
        fail(f'a reference of two measurements was not refused (exit {status}): {log[-300:]}')
    print('a reference of two measurements is refused')


def main(args):
    if len(args) >= 3 and args[0] == 'figure':
        check_figure(os.path.abspath(args[1]), args[2])
    elif len(args) >= 4 and args[0] == 'run':
        check_run(os.path.abspath(args[1]), os.path.abspath(args[2]), args[3],
                  os.path.abspath(args[4]) if len(args) > 4 else None)
    elif len(args) >= 3 and args[0] == 'refusal':
        check_refusal(os.path.abspath(args[1]), os.path.abspath(args[2]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
