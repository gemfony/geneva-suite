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
  testSpeedupTools.py run <GParallelisationOverhead> <workdir> <consumer> [<reference dir>]
      a short run of runWithClients.sh with two clients; with a reference dir, the serial reference
      of that earlier run is used instead of a new one (MPIRUN in the environment says how mpi is
      started)
  testSpeedupTools.py refusal <GParallelisationOverhead> <workdir>
      a serial reference of two measurements is refused with a message that says why

The work directory is created; it must lie outside the source tree.
"""
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def fail(message):
    """Ends the test with a message."""
    sys.exit('FAILED: ' + message)


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
    if len(args) >= 4 and args[0] == 'run':
        check_run(os.path.abspath(args[1]), os.path.abspath(args[2]), args[3],
                  os.path.abspath(args[4]) if len(args) > 4 else None)
    elif len(args) >= 3 and args[0] == 'refusal':
        check_refusal(os.path.abspath(args[1]), os.path.abspath(args[2]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
