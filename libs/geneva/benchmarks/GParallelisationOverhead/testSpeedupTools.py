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
      started). The run passes a client option, the prefetch depth, and checks that both clients
      used it.
  testSpeedupTools.py subclients <GSubClientOverhead> <workdir> <consumer> [<reference dir>]
      a short run with two clients of two sub-clients each: every evaluation is done once, in a
      group of three, every process ends by itself, and both clients used the prefetch depth the
      run passes
  testSpeedupTools.py groups <GSubClientOverhead> <workdir>
      a job that does not form whole groups is refused, in both layouts, with a message that
      names its size and the group
  testSpeedupTools.py refusal <GParallelisationOverhead> <workdir>
      a serial reference of two measurements is refused with a message that says why
  testSpeedupTools.py gate <GParallelisationOverhead> <workdir> <reference dir>
      the server waits for its clients before the first run: a client started seconds after the
      other is waited for; with one client too few the server ends at the limit, non-zero, and
      says how many came

The work directory is created; it must lie outside the source tree.
"""
import importlib.util
import math
import os
import shutil
import signal
import socket
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
    # The series are named by their labels alone, each label once as its graph's title and once
    # as its legend entry; each pad has a title of its own and a legend at the lower right.
    for text, count in (('TGraphErrors(', 6), ('new TLegend(', 2), ('Divide(1,2)', 1),
                        ('SetTitle("fast")', 2), ('SetTitle("slow")', 2), ('SetTitle("ideal (4)")', 2),
                        ('"fast", "lp");', 2), ('"slow", "lp");', 2), ('"ideal (4)", "lp");', 2),
                        ('"All evaluation times"', 1), ('"Up to 0.1 s"', 1), ('fast:', 0),
                        ('new TLegend(0.3, 0.21)', 0), ('SetLineWidth(2)', 6), ('canvasTitle->Draw()', 1)):
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

    # A series that stays along the bottom covers the lower right, so the legend moves up; asked
    # for a corner, it goes there.
    write_table('low.txt', [(0.01, 0.1, 0.01), (0.1, 0.2, 0.01), (1, 0.3, 0.01)])
    write_table('high.txt', [(0.01, 3.0, 0.01), (0.1, 3.9, 0.01), (1, 4.0, 0.01)])
    for args, expected in (([], 'new TLegend(0.56, 0.3875, 0.88, 0.6125)'),
                           (['--legend', 'top_left'], 'new TLegend(0.12, 0.655, 0.44, 0.88)')):
        placed = subprocess.run([program, '--ideal', '4', '--zoom', '0', '--figure', 'placed.C']
                                + args + ['high=high.txt', 'low=low.txt'],
                                capture_output=True, text=True, check=False)
        if placed.returncode != 0 or expected not in open('placed.C', encoding='utf-8').read():
            fail(f'the legend is not at {expected} with {args}')

    untitled = subprocess.run([program, '--no-title', '--figure', 'untitled.C', 'fast=fast-1.txt'],
                              capture_output=True, text=True, check=False)
    if untitled.returncode != 0 or 'TPaveLabel' in open('untitled.C', encoding='utf-8').read():
        fail('--no-title does not leave the title out')

    bad = subprocess.run([program, 'fast=missing.txt'], capture_output=True, text=True,
                         check=False)
    if bad.returncode == 0 or 'missing.txt' not in bad.stderr:
        fail('a missing table is not refused with its name')
    print('figure and means as expected')


def run_bounded(command, limit, env=None, capture=True):
    """Runs a command in a process group of its own and returns its exit code and its output
    (stdout and stderr; empty unless captured). At the limit the whole group is ended -- the
    command and every process it started, an MPI job included, which would otherwise outlive it
    -- and the check fails."""
    with subprocess.Popen(command, env=env, text=True, start_new_session=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None) as job:
        try:
            out, _ = job.communicate(timeout=limit)
        except subprocess.TimeoutExpired:
            os.killpg(job.pid, signal.SIGTERM)
            try:
                job.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(job.pid, signal.SIGKILL)
                job.communicate()
            fail(f'{" ".join(command[:3])} ... did not end within {limit} s; it was ended with '
                 'every process it started')
    return job.returncode, out or ''


def run_script(program, workdir, consumer, rows, reference_delays, env=None, options=None):
    """Runs runWithClients.sh with two clients and returns its exit code and its output; options
    follow the nine arguments."""
    status, out = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                               consumer, '2', '4', '2', rows, reference_delays, '1']
                              + (options or []), 240, env=dict(os.environ, **(env or {})))
    sys.stdout.write(out)
    return status, out


def check_run(program, workdir, consumer, reference_dir):
    """A short run of runWithClients.sh, with a reference of its own or a shared one."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'reference.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    if reference_dir:
        shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    # A websocket server's first port is held by another process: the script must try another one
    holder = None
    env = None
    if consumer != 'mpi':
        holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        holder.bind(('0.0.0.0', 0))
        holder.listen(1)
        env = {'SERVER_PORT': str(holder.getsockname()[1])}
    # A client option must reach every client: a prefetch depth other than the consumer's default
    # (1 for a networked consumer, 2 for mpi), which each client names when it ends.
    depth = '3' if consumer == 'mpi' else '2'
    options = [f'--{consumer}_prefetchDepth', depth, f'--{consumer}_parallelEvaluations', '1']
    try:
        status, out = run_script(program, workdir, consumer, '(0,10)', '(0,10), (0,20), (0,30)',
                                 env, options)
    finally:
        if holder is not None:
            holder.close()
    if status != 0:
        fail(f'runWithClients.sh exited with {status}; see {workdir}/server.log')
    if holder is not None and 'is taken; the server is started on another one' not in out:
        fail('the script did not move to another port when the first one was taken')
    if reference_dir and os.path.exists(os.path.join(workdir, 'reference.log')):
        fail('the shared reference was measured again')
    rows = [l.split() for l in open(os.path.join(workdir, 'shortDelayResults.txt'),
                                    encoding='utf-8') if not l.startswith('#')]
    if len(rows) != 1 or float(rows[0][0]) != 0.01 or not float(rows[0][5]) > 0.:
        fail(f'unexpected table {rows}')
    log = open(os.path.join(workdir, 'server.log'), encoding='utf-8', errors='replace').read()
    if 'Waiting for clients: 2 of 2 clients connected after' not in log:
        fail('the server did not wait for its two clients')
    # The clients of a networked consumer write to clients.log, the ranks of mpi to server.log. A
    # client that had to be ended may not have said its prefetch depth.
    if consumer != 'mpi':
        if 'clients that did not end by themselves: 0' not in out:
            fail(f'a client had to be ended: {out}')
        log = open(os.path.join(workdir, 'clients.log'), encoding='utf-8', errors='replace').read()
    used = {d: log.count(f'prefetch depth was {d}\n') for d in ('1', '2', '3')}
    if used[depth] != 2 or sum(used.values()) != 2:
        fail(f'expected two clients with prefetch depth {depth}, found the depths {used} '
             '(depth: number of clients)')
    if reference_dir:
        line = open(os.path.join(reference_dir, 'referenceLine.txt'), encoding='utf-8').read()
        a, _, b, _ = (float(v) for v in line.split())
        if not math.isclose(float(rows[0][1]), a + b * 0.01, rel_tol=1e-5):
            fail('the table does not use the shared reference')
    print(f'{consumer}: speed-up {rows[0][5]} with two clients')


def check_subclients(program, workdir, consumer, reference_dir):
    """Two clients of two sub-clients each: every evaluation once, every process ends by itself."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'reference.log', 'clients.log',
                 'server.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    if reference_dir:
        shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    # The leads build their clients from the consumer options themselves: a prefetch depth other
    # than the consumer's default must reach both (see check_run()).
    depth = '3' if consumer == 'mpi' else '2'
    options = [f'--{consumer}_prefetchDepth', depth, f'--{consumer}_parallelEvaluations', '1']
    status, out = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                               consumer, '2', '4', '3', '(0,20)', '(0,10), (0,20), (0,30)', '1']
                              + options, 240,
                              # Every evaluation exactly once: no item may be given to a second
                              # client because its first one was slow under load (an adaptive
                              # lease).
                              env=dict(os.environ, GROUP='3', CLIENT_GRACE='60', INTER_DELAY='0',
                                       TIMEOUT_TREATMENT='wait_indefinitely'))
    if status != 0:
        fail(f'runWithClients.sh exited with {status}: {out}')
    if 'clients that did not end by themselves: 0' not in out and consumer != 'mpi':
        fail(f'a client had to be ended: {out}')
    log = ''
    for name in ('server.log', 'clients.log'):
        if os.path.exists(os.path.join(workdir, name)):
            log += open(os.path.join(workdir, name), encoding='utf-8').read()
    leads = [int(l.split()[6]) for l in log.splitlines()
             if l.startswith('GSleepGroup: lead of 3 processes led')]
    members = sorted(int(l.split()[-2]) for l in log.splitlines()
                     if l.startswith('GSleepGroup: member'))
    # Population 4 with 2 parents, 3 iterations: 4 + 2 * 2 evaluations per run; a warm-up run and
    # one measured run.
    expected = 2 * (4 + 2 * 2)
    if len(leads) != 2 or sum(leads) != expected:
        fail(f'the leads led {leads} rounds instead of {expected} together')
    if members != sorted(leads * 2):
        fail(f'the members served {members} parts, their leads led {leads} rounds')
    used = {d: log.count(f'prefetch depth was {d}\n') for d in ('1', '2', '3')}
    if used[depth] != 2 or sum(used.values()) != 2:
        fail(f'expected two clients with prefetch depth {depth}, found the depths {used} '
             '(depth: number of clients)')
    print(f'{consumer}: {expected} evaluations, each in a group of three; every process ended')


def check_groups(program, workdir):
    """A job that does not form whole groups ends at once, non-zero, on every rank."""
    os.makedirs(workdir, exist_ok=True)
    os.chdir(workdir)
    launcher = os.environ.get('MPIRUN', 'mpirun --oversubscribe -np').split()
    for ranks, args, expected in (
            ('6', ['-c', 'mpi', '--group', '3'], 'the MPI job has 6 processes, but with -c mpi '
             'and --group 3 it needs 1 + clients x 3'),
            ('2', ['-c', 'beast', '--client', '--group', '3'], 'the MPI job has 2 processes, '
             'but a client with --group 3 is one job of 3')):
        status, out = run_bounded(launcher + [ranks, program] + args, 60)
        if status == 0 or expected not in out:
            fail(f'{ranks} processes with {args} were not refused (exit {status}) '
                 f'with "{expected}": {out[-1000:]}')
        print(f'{ranks} processes with {" ".join(args)}: refused')


def check_refusal(program, workdir):
    """A reference of two measurements cannot give its line an error and must be refused."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('referenceLine.txt', 'reference.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    status, _ = run_script(program, workdir, 'beast', '(0,10)', '(0,10), (0,20)')
    log = open(os.path.join(workdir, 'reference.log'), encoding='utf-8').read()
    if status == 0 or 'it needs at least 3' not in log:
        fail(f'a reference of two measurements was not refused (exit {status}): {log[-300:]}')
    print('a reference of two measurements is refused')


def gate_run(program, workdir, reference_dir, env):
    """A run of runWithClients.sh with two websocket clients and the shared reference."""
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'server.log', 'clients.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    status, _ = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                             'beast', '2', '4', '2', '(0,10)', '(0,10), (0,20), (0,30)', '1'],
                            240, env=dict(os.environ, INTER_DELAY='0', **env), capture=False)
    log = open(os.path.join(workdir, 'server.log'), encoding='utf-8', errors='replace').read()
    return status, log


def check_gate(program, workdir, reference_dir):
    """The server waits for its clients before its first run, within a limit."""
    delay = 4
    status, log = gate_run(program, os.path.join(workdir, 'late'), reference_dir,
                           {'LAST_CLIENT_DELAY': str(delay)})
    if status != 0:
        fail(f'the run with a late client exited with {status}: {log[-500:]}')
    waited = [l for l in log.splitlines() if l.startswith('Waiting for clients: ')]
    if len(waited) != 1 or not waited[0].startswith('Waiting for clients: 2 of 2 clients'):
        fail(f'the server did not wait for both clients: {waited}')
    seconds = float(waited[0].split()[-2])
    # The late client is started DELAY s after the server has begun to wait (runWithClients.sh).
    if seconds < delay:
        fail(f'the server waited {seconds} s, less than the {delay} s the last client came later')
    if log.find('Waiting for clients: ') > log.find('Warm-up run'):
        fail('the first run began before the clients were all connected')
    print(f'the server waited {seconds} s for its late client')

    status, log = gate_run(program, os.path.join(workdir, 'short'), reference_dir,
                           {'EXPECT_CLIENTS': '3', 'CLIENT_WAIT_LIMIT': '3',
                            'CLIENT_GRACE': '5'})
    message = 'only 2 of 3 clients connected within the limit of 3 s'
    if status == 0 or message not in log:
        fail(f'one client too few was not refused at the limit (exit {status}): {log[-500:]}')
    if 'Warm-up run' in log:
        fail('the server began to run although a client was missing')
    print('one client too few ends the server at the limit, with the message')


def main(args):
    if len(args) >= 3 and args[0] == 'figure':
        check_figure(os.path.abspath(args[1]), args[2])
    elif len(args) >= 4 and args[0] == 'run':
        check_run(os.path.abspath(args[1]), os.path.abspath(args[2]), args[3],
                  os.path.abspath(args[4]) if len(args) > 4 else None)
    elif len(args) >= 4 and args[0] == 'subclients':
        check_subclients(os.path.abspath(args[1]), os.path.abspath(args[2]), args[3],
                         os.path.abspath(args[4]) if len(args) > 4 else None)
    elif len(args) >= 3 and args[0] == 'groups':
        check_groups(os.path.abspath(args[1]), os.path.abspath(args[2]))
    elif len(args) >= 4 and args[0] == 'gate':
        check_gate(os.path.abspath(args[1]), os.path.abspath(args[2]), os.path.abspath(args[3]))
    elif len(args) >= 3 and args[0] == 'refusal':
        check_refusal(os.path.abspath(args[1]), os.path.abspath(args[2]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
