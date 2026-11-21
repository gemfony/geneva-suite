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
      of that earlier run is used instead of a new one (MPIRUN in the environment names the MPI
      launcher; every check that starts MPI jobs refuses to run without it). The run passes two
      client options, the prefetch depth and the evaluations at a time, and checks that both
      clients used them. With mqtt the server and its clients meet on the tests' MQTT broker,
      whose port the file port in GEM_MQTT_TEST_BROKER_DIR names (the broker fixture of courtier's
      tests writes it).
  testSpeedupTools.py subclients <GSubClientOverhead> <workdir> <consumer> [<reference dir>]
      a short run with two clients of two sub-clients each: every evaluation is done once, in a
      group of three, every process ends by itself, and both clients used the prefetch depth and
      the evaluations at a time the run passes
  testSpeedupTools.py samejobid <GSubClientOverhead> <workdir> [<reference dir>]
      as subclients with beast, but Open MPI gives both client jobs the same job id (their
      launcher is placed on pids whose ids collide): the jobs must not share Open MPI's shared
      memory (runWithClients.sh gives each job a directory of its own for it); Open MPI only
  testSpeedupTools.py placement [<competitors>]
      the placement of the samejobid check on a busy host: while <competitors> processes (2 by
      default) take pids as fast as they can, a program is started three times on a pid of a given
      job family, each time within 60 s
  testSpeedupTools.py placement-killed
      the placement check, ended with SIGKILL while its competitors run (as ctest ends an entry
      at its TIMEOUT), leaves no process behind
  testSpeedupTools.py interrupt <GSubClientOverhead> <workdir> <reference dir>
      runWithClients.sh with two websocket clients of three processes each, ended from outside
      with SIGINT while it measures: no process it started may remain, nor the geneva-sm.*
      directory of its MPI jobs' shared-memory files
  testSpeedupTools.py groups <GSubClientOverhead> <workdir>
      a job that does not form whole groups is refused, in both layouts, with a message that
      names its size and the group; a job of -c mpi whose submitter fails (an option only it
      refuses, no serial reference) ends at once, with the submitter's message
  testSpeedupTools.py bounded
      a command that runs into its limit is ended, and the check's failure names what still ran
      and the end of its output
  testSpeedupTools.py refusal <GParallelisationOverhead> <workdir>
      a serial reference of two measurements is refused with a message that says why
  testSpeedupTools.py gate <GParallelisationOverhead> <workdir> <reference dir>
      the server waits for its clients before the first run: a client started seconds after the
      other is waited for; with one client too few the server ends at the limit, non-zero, and
      says how many came

The samejobid and placement checks end with 77 (SKIP, the entries' SKIP_RETURN_CODE) on a host
whose pid_max is too small for a job family to hold pids reliably; SPEEDUP_TOOLS_PID_MAX in the
environment stands in for the kernel's pid_max, so that this can be checked on any host.

The work directory is created; it must lie outside the source tree.
"""
import ctypes
import importlib.util
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = ('# evaluation time [s]; serial run time [s] (fitted) and its error; parallel run time '
          '[s] and its error; speed-up and its error\n')


# The exit code of a check that cannot run on this host (the entries' SKIP_RETURN_CODE)
SKIP = 77
# The pids a job family (16 bits of Open MPI's job id) must hold on average in the pid cycle for
# the placement on one of them to be reliable
MIN_FAMILY_PIDS = 16
# The smallest pid_max with that many (past pid_max the kernel starts again at 300)
DENSE_PID_MAX = MIN_FAMILY_PIDS * 65536 + 300


def fail(message):
    """Ends the test with a message."""
    sys.exit('FAILED: ' + message)


def mpi_launcher(env=None):
    """The MPI launcher, its options and its flag for the rank count, from MPIRUN in env (the
    process's environment by default). There is no default from PATH: the build names its
    launcher, the one GemMpiTestsRegistered looks for in the registered commands, so that the
    entry is seen to start MPI jobs and runs under GemRunMpiTest.sh."""
    launcher = (os.environ if env is None else env).get('MPIRUN', '').split()
    if not launcher:
        fail('MPIRUN is not set: this check starts MPI jobs and needs the MPI launcher the build '
             'found (its registration passes it)')
    return launcher


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
                          '--table', 'fast=fast-1.txt', '--table', 'fast=fast-2.txt',
                          '--table', 'slow=slow.txt'],
                         capture_output=True, text=True, check=False)
    if run.returncode != 0:
        fail(f'GSpeedupFigure exited with {run.returncode}: {run.stderr}')

    figure = open('figure.C', encoding='utf-8').read()
    # Two pads (all times, and those up to 0.1 s), each with both labels and the ideal line
    # The series are named by their labels alone, each label once as its graph's title and once
    # as its legend entry; each pad has a title of its own and a legend at the lower right.
    for text, count in (('TGraphErrors(', 6), ('new TLegend(', 2), ('Divide(1,2)', 1),
                        ('SetTitle("fast")', 2), ('SetTitle("slow")', 2),
                        ('SetTitle("ideal (4)")', 2), ('"fast", "lp");', 2),
                        ('"slow", "lp");', 2), ('"ideal (4)", "lp");', 2),
                        ('"All evaluation times"', 1), ('"Up to 0.1 s"', 1), ('fast:', 0),
                        ('new TLegend(0.3, 0.21)', 0), ('SetLineWidth(2)', 6),
                        ('canvasTitle->Draw()', 1)):
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
        combined = reference.combine([reference.read(t) for t in tables])
        for time, (mean, error, runs) in combined.items():
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
                                + args + ['--table', 'high=high.txt', '--table', 'low=low.txt'],
                                capture_output=True, text=True, check=False)
        if placed.returncode != 0 or expected not in open('placed.C', encoding='utf-8').read():
            fail(f'the legend is not at {expected} with {args}')

    untitled = subprocess.run([program, '--no-title', '--figure', 'untitled.C',
                               '--table', 'fast=fast-1.txt'],
                              capture_output=True, text=True, check=False)
    if untitled.returncode != 0 or 'TPaveLabel' in open('untitled.C', encoding='utf-8').read():
        fail('--no-title does not leave the title out')

    bad = subprocess.run([program, '--table', 'fast=missing.txt'], capture_output=True, text=True,
                         check=False)
    if bad.returncode == 0 or 'missing.txt' not in bad.stderr:
        fail('a missing table is not refused with its name')
    none = subprocess.run([program, '--figure', 'none.C'], capture_output=True, text=True,
                          check=False)
    if none.returncode == 0 or '--table' not in none.stderr:
        fail('a call without a table is not refused with a hint to --table')
    print('figure and means as expected')


def run_bounded(command, limit, env=None, capture=True):
    """Runs a command in a process group of its own and returns its exit code and its output
    (stdout and stderr; empty unless captured). At the limit the whole group is ended -- the
    command and every process it started, an MPI job included, which would otherwise outlive it
    -- and the check fails, with what still ran (every thread of the session and the kernel
    function it waited in, where ps knows -L) and the end of the output, so that a hang that
    happens rarely is not lost."""
    with subprocess.Popen(command, env=env, text=True, start_new_session=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None) as job:
        try:
            out, _ = job.communicate(timeout=limit)
        except subprocess.TimeoutExpired:
            running = subprocess.run(['ps', '-L', '-o', 'pid,tid,stat,wchan:32,comm', '--sid',
                                      str(job.pid)], text=True, capture_output=True,
                                     check=False).stdout
            os.killpg(job.pid, signal.SIGTERM)
            try:
                out, _ = job.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(job.pid, signal.SIGKILL)
                out, _ = job.communicate()
            fail(f'{" ".join(command[:3])} ... did not end within {limit} s; it was ended with '
                 f'every process it started. What still ran:\n{running}'
                 f'The end of its output:\n{(out or "")[-4000:]}')
    return job.returncode, out or ''


def run_script(program, workdir, consumer, rows, reference_delays, env=None, options=None):
    """Runs runWithClients.sh with two clients and returns its exit code and its output; options
    follow the nine arguments."""
    status, out = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                               consumer, '2', '4', '2', rows, reference_delays, '1']
                              + (options or []), 240, env=dict(os.environ, **(env or {})))
    sys.stdout.write(out)
    return status, out


def check_client_options(log, depth):
    """Both clients name the prefetch depth and the two evaluations at a time that the run passed
    them, in the summary each writes when it ends, and no other client writes one."""
    used = {d: log.count(f'prefetch depth was {d}\n') for d in ('1', '2', '3')}
    if used[depth] != 2 or sum(used.values()) != 2:
        fail(f'expected two clients with prefetch depth {depth}, found the depths {used} '
             '(depth: number of clients)')
    parallel = {m: log.count(f'parallel evaluations were {m}\n') for m in ('1', '2')}
    if parallel['2'] != 2 or sum(parallel.values()) != 2:
        fail('expected two clients with two evaluations at a time, found '
             f'{parallel} (evaluations at a time: number of clients)')


def mqtt_broker_port():
    """The port of the tests' MQTT broker, which its fixture writes into GEM_MQTT_TEST_BROKER_DIR."""
    directory = os.environ.get('GEM_MQTT_TEST_BROKER_DIR', '')
    try:
        port = open(os.path.join(directory, 'port'), encoding='utf-8').read().strip()
    except OSError:
        port = ''
    if not port.isdigit():
        fail(f'no MQTT test broker: GEM_MQTT_TEST_BROKER_DIR ("{directory}") names no port')
    return port


def check_run(program, workdir, consumer, reference_dir):
    """A short run of runWithClients.sh, with a reference of its own or a shared one."""
    if consumer == 'mpi':
        mpi_launcher()
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'reference.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    if reference_dir:
        shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    # A websocket server's first port is held by another process: the script must try another one.
    # An MQTT server binds no port: it and its clients meet on the tests' broker.
    holder = None
    env = None
    if consumer == 'mqtt':
        env = {'BROKER_PORT': mqtt_broker_port()}
    elif consumer != 'mpi':
        holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        holder.bind(('0.0.0.0', 0))
        holder.listen(1)
        env = {'SERVER_PORT': str(holder.getsockname()[1])}
    # Client options must reach every client: a prefetch depth other than the consumer's default
    # (1 for a networked consumer, 2 for mpi) and two evaluations at a time (default 1), which
    # each client names when it ends.
    depth = '3' if consumer == 'mpi' else '2'
    options = [f'--{consumer}_prefetchDepth', depth, f'--{consumer}_parallelEvaluations', '2']
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
    check_client_options(log, depth)
    if reference_dir:
        line = open(os.path.join(reference_dir, 'referenceLine.txt'), encoding='utf-8').read()
        a, _, b, _ = (float(v) for v in line.split())
        if not math.isclose(float(rows[0][1]), a + b * 0.01, rel_tol=1e-5):
            fail('the table does not use the shared reference')
    print(f'{consumer}: speed-up {rows[0][5]} with two clients')


def check_subclients(program, workdir, consumer, reference_dir, env=None):
    """Two clients of two sub-clients each: every evaluation once, every process ends by itself;
    env is added to the environment of runWithClients.sh."""
    mpi_launcher(dict(os.environ, **(env or {})))
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'reference.log', 'server.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    if reference_dir:
        shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    # The clients.log of an earlier run in the same work directory must not be taken for this
    # run's (with mpi this run writes none)
    with open(os.path.join(workdir, 'clients.log'), 'w', encoding='utf-8') as stale:
        stale.write('GSleepGroup: lead of 3 processes led 99 rounds (an earlier run)\n'
                    'prefetch depth was 1\n')
    # The leads build their clients from the consumer options themselves: the options of
    # check_run() must reach both.
    depth = '3' if consumer == 'mpi' else '2'
    options = [f'--{consumer}_prefetchDepth', depth, f'--{consumer}_parallelEvaluations', '2']
    status, out = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                               consumer, '2', '4', '3', '(0,20)', '(0,10), (0,20), (0,30)', '1']
                              + options, 240,
                              # Every evaluation exactly once: there is no time limit by default,
                              # so no item is given to a second client because its first was slow.
                              env=dict(os.environ, GROUP='3', CLIENT_GRACE='60', INTER_DELAY='0',
                                       **(env or {})))
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
    check_client_options(log, depth)
    print(f'{consumer}: {expected} evaluations, each in a group of three; every process ended')


def hash_add(value, text):
    """Adds text to a state of Jenkins' one-at-a-time hash."""
    for char in text.encode():
        value = (value + char) & 0xffffffff
        value = (value + (value << 10)) & 0xffffffff
        value ^= value >> 6
    return value


def hash_family(value):
    """Finishes a state of Jenkins' one-at-a-time hash and folds it to 16 bits."""
    value = (value + (value << 3)) & 0xffffffff
    value ^= value >> 11
    value = (value + (value << 15)) & 0xffffffff
    return ((value >> 16) ^ value) & 0xffff


def job_family(host, pid):
    """The upper 16 bits of the job id Open MPI 5 gives the job of the mpirun with this pid: its
    namespace "prterun-<host>-<pid>" hashed (Jenkins' one-at-a-time hash) and folded to 16 bits.
    Open MPI names the files of its shared-memory transport after the job id, so two independent
    jobs of one family on one host use the same files unless each has a directory of its own."""
    return hash_family(hash_add(0, f'prterun-{host}-{pid}'))


def family_pids(host, family, after, count, pid_max):
    """The pids of the job family among the count pids that follow after, in the order the kernel
    gives them (past pid_max it starts again at 300). As job_family(), with the hash of what the
    pids share computed once."""
    prefix = hash_add(0, f'prterun-{host}-')
    heads = {}
    found, pid = [], after
    for _ in range(count):
        pid = pid + 1 if pid + 1 < pid_max else 300
        text = str(pid)
        state = heads.get(text[:-2])
        if state is None:
            state = heads[text[:-2]] = hash_add(prefix, text[:-2])
        if hash_family(hash_add(state, text[-2:])) == family:
            found.append(pid)
    return found


def pid_max():
    """The kernel's pid_max, or SPEEDUP_TOOLS_PID_MAX from the environment instead."""
    if os.environ.get('SPEEDUP_TOOLS_PID_MAX'):
        return int(os.environ['SPEEDUP_TOOLS_PID_MAX'])
    with open('/proc/sys/kernel/pid_max', encoding='ascii') as pid_file:
        return int(pid_file.read())


def require_dense_families():
    """Ends the check with SKIP where pid_max is too small for the placement on a pid of a job
    family: a family holds on average (pid_max - 300) / 65536 pids of the pid cycle (past pid_max
    the kernel starts again at 300), and below MIN_FAMILY_PIDS it too often holds none, or only the
    one the first job has. The kernel's default is max(32768, 1024 x CPUs) where nothing raises
    it, which is about one pid per family."""
    limit = pid_max()
    if limit < DENSE_PID_MAX:
        print(f'SKIPPED: pid_max is {limit}; placing a job on a pid of a job family needs at '
              f'least {DENSE_PID_MAX} (about {MIN_FAMILY_PIDS} pids per family)', flush=True)
        sys.exit(SKIP)


def pid_ahead(pid, count, limit):
    """The pid the kernel gives count pids after pid: past limit (pid_max) it starts again at
    300."""
    return 300 + (pid - 300 + count) % (limit - 300)


def last_pid():
    """The pid this pid namespace gave last."""
    with open('/proc/sys/kernel/ns_last_pid', encoding='ascii') as pid_file:
        return int(pid_file.read())


def start_on(command):
    """Starts command in a child process and returns its pid."""
    child = os.fork()
    if child == 0:
        try:
            os.execvp(command[0], command)
        finally:
            os._exit(127)  # pylint: disable=protected-access
    return child


def race_for(command, target, start, cycle, racers):
    """Racer processes fork as fast as they can; the child that the kernel gives the pid target
    execs command, every other child ends at once. A racer ends when its child got the target
    (0) or a later pid (1). Returns whether command runs on target; it is then this process's
    child (the caller is the subreaper of its descendants)."""
    def offset(pid):
        return (pid - start) % cycle

    racing = []
    for _ in range(racers):
        racer = os.fork()
        if racer == 0:
            end = time.monotonic() + 5
            while time.monotonic() < end:
                child = os.fork()
                if child == 0:
                    if os.getpid() == target:
                        try:
                            os.execvp(command[0], command)
                        finally:
                            os._exit(127)  # pylint: disable=protected-access
                    os._exit(0)  # pylint: disable=protected-access
                if child == target:
                    os._exit(0)  # pylint: disable=protected-access
                os.waitpid(child, 0)
                if offset(child) > offset(target):
                    os._exit(1)  # pylint: disable=protected-access
            os._exit(1)  # pylint: disable=protected-access
        racing.append(racer)
    won = False
    for racer in racing:
        won = os.waitstatus_to_exitcode(os.waitpid(racer, 0)[1]) == 0 or won
    return won


def start_in_family(command, host, family, limit=120, racers=16, window=3000):
    """Starts command (mpirun and its arguments) on a pid whose job has the given family and returns
    that pid, or None if no such pid could be taken within limit seconds.

    The pids before the next pid of the family are used up by threads that end at once, a few at a
    time, as other processes take pids too. The last window pids before it are raced for
    (race_for()): on a busy host other processes take thousands of pids a second, and a single
    fork that waits for the pid before the target almost never gets it -- the race gives this
    process a share of the pids large enough that one of the next targets is taken."""
    limit = pid_max()
    cycle = limit - 300  # past pid_max the pids start again at 300
    # The job, a child of a racer, becomes this process's child when its racer ends
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        fail(f'PR_SET_CHILD_SUBREAPER failed: {os.strerror(ctypes.get_errno())}')

    def burn(count):
        for _ in range(count):
            thread = threading.Thread(target=int)
            thread.start()
            thread.join()

    deadline = time.monotonic() + limit
    start, candidates = None, []
    while time.monotonic() < deadline:
        current = last_pid()
        if start is None or not candidates:
            start = current
            candidates = family_pids(host, family, current, 2000000, limit)
        position = (current - start) % cycle
        # A target that close is passed before the racers are under way
        candidates = [c for c in candidates if (c - start) % cycle > position + 20]
        if not candidates:
            continue
        distance = (candidates[0] - start) % cycle - position
        if distance > window:
            burn(min(distance - window // 2, 1000))
        elif race_for(command, candidates[0], start, cycle, racers):
            return candidates[0]
        else:
            candidates = candidates[1:]
    return None


def launch_same_job_id(state, command):
    """The launcher of the samejobid check (its MPIRUN): starts command, an mpirun with its
    arguments, so that Open MPI gives its job the id of the first job started this way. The first
    launcher writes its job's family to <state>.first, the later ones place their mpirun on a pid
    of that family. Each launcher then looks for the job's shared-memory files under that id in
    the job's directory for them and appends "<job id> seen" (or "unseen") to state, so that the
    check knows the ids really were equal. A launcher that cannot take a pid of the family in time
    starts its job on any pid and appends "<job id> unplaced": the run goes on, and the check fails
    at its end with that reason instead of waiting for a client that never comes. Returns the exit
    code of the job."""
    host = socket.gethostname().split('.')[0]
    try:
        first = os.open(state + '.first', os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        first = None
    placed = True
    if first is not None:
        child = start_on(command)
        family = job_family(host, child)
        os.write(first, f'{family}\n'.encode())
        os.close(first)
    else:
        family = None
        for _ in range(600):
            with open(state + '.first', encoding='ascii') as first_file:
                text = first_file.read()
            if text.endswith('\n'):
                family = int(text)
                break
            time.sleep(0.1)
        if family is None:
            fail('the first job did not say its job family')
        child = start_in_family(command, host, family)
        if child is None:
            print(f'no pid of the job family {family:04x} could be taken in time; the job is '
                  f'started on any pid', flush=True)
            child = start_on(command)
            family, placed = job_family(host, child), False
    signal.signal(signal.SIGTERM, lambda *_: os.kill(child, signal.SIGTERM))
    job_id = (family << 16) | 1
    directory = os.environ.get('OMPI_MCA_btl_sm_backing_directory', '/dev/shm')
    segment = os.path.join(directory, f'sm_segment.{host}.{os.getuid()}.{job_id:x}.0')
    seen, status = False, None
    for _ in range(600):
        if os.path.exists(segment):
            seen = True
            break
        ended, status = os.waitpid(child, os.WNOHANG)
        if ended != 0:
            break
        status = None
        time.sleep(0.1)
    with open(state, 'a', encoding='ascii') as out:
        out.write(f'{job_id:x} {"seen" if seen else "unseen"}{"" if placed else " unplaced"}\n')
    if status is None:
        status = os.waitpid(child, 0)[1]
    code = os.waitstatus_to_exitcode(status)
    return code if code >= 0 else 128 - code


def check_placement(competitors):
    """start_in_family() on a busy host: competing processes take pids as fast as they can (a
    parallel ctest takes 20000 to 30000 a second), and a program must still be started on a pid
    of the family of a pid some 40000 ahead, three times, within 60 s each. The competitors end
    with this process, also when it is killed (PR_SET_PDEATHSIG)."""
    require_dense_families()
    host = socket.gethostname().split('.')[0]
    limit = pid_max()
    libc = ctypes.CDLL(None, use_errno=True)
    parent = os.getpid()
    burners = []
    for _ in range(competitors):
        burner = os.fork()
        if burner == 0:
            # SIGKILL when the parent ends, whichever way; a parent that ended before the call
            # is seen in the parent pid
            if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != parent:
                os._exit(1)  # pylint: disable=protected-access
            while True:
                thread = threading.Thread(target=int)
                thread.start()
                thread.join()
        burners.append(burner)
    try:
        before = last_pid()
        time.sleep(1)
        print(f'{competitors} competitors take {last_pid() - before} pids a second', flush=True)
        for attempt in range(3):
            family = job_family(host, pid_ahead(last_pid(), 40000, limit))
            began = time.monotonic()
            child = start_in_family(['true'], host, family, limit=60)
            took = time.monotonic() - began
            if child is None:
                fail(f'attempt {attempt + 1}: no pid of the job family {family:04x} was taken '
                     f'within 60 s')
            code = os.waitstatus_to_exitcode(os.waitpid(child, 0)[1])
            if job_family(host, child) != family or code != 0:
                fail(f'attempt {attempt + 1}: the program ran on pid {child} of the family '
                     f'{job_family(host, child):04x}, not {family:04x}, and ended with {code}')
            print(f'attempt {attempt + 1}: started on pid {child} of the family {family:04x} '
                  f'after {took:.1f} s', flush=True)
    finally:
        for burner in burners:
            os.kill(burner, signal.SIGKILL)
            os.waitpid(burner, 0)


def check_placement_killed():
    """The placement check, killed with SIGKILL once its competitors run, leaves no process of
    its session behind within 10 s (the racers of a placement end within 5 s by themselves).

    The check is killed before it places anything, so its pid_max does not matter: on a host
    whose pid_max is too small for the placement, a stand-in lets it start its competitors
    instead of skipping."""
    env = dict(os.environ, SPEEDUP_TOOLS_PID_MAX=str(max(pid_max(), DENSE_PID_MAX)))
    job = subprocess.Popen([sys.executable, '-B', os.path.abspath(__file__), 'placement', '2'],
                           start_new_session=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, env=env)
    try:
        line = job.stdout.readline()
        if 'competitors take' not in line:
            fail(f'the placement check did not start its competitors: {line}')
        os.kill(job.pid, signal.SIGKILL)
        job.wait()
        left = session_processes(job.pid)
        for _ in range(100):
            if not left:
                break
            time.sleep(0.1)
            left = session_processes(job.pid)
        if left:
            fail(f'{len(left)} processes of the killed placement check still run 10 s after it '
                 f'ended: {left}')
    finally:
        for pid in session_processes(job.pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        if job.poll() is None:
            job.kill()
            job.wait()
        job.stdout.close()
    print('the placement check, killed, left no process behind')


def check_same_job_id(program, workdir, reference_dir):
    """As check_subclients() with beast, but both client jobs get the same Open MPI job id."""
    require_dense_families()
    launcher = ' '.join(mpi_launcher())
    os.makedirs(workdir, exist_ok=True)
    state = os.path.join(workdir, 'jobids.txt')
    for name in (state, state + '.first'):
        if os.path.exists(name):
            os.remove(name)
    # Open MPI keeps its session files below TMPDIR: a fresh one, so that no file of an earlier
    # job with the same pid (in a pid namespace of a sandbox) is in the way; short, because it
    # holds sockets
    session = tempfile.mkdtemp(prefix='gsj')
    try:
        check_subclients(program, workdir, 'beast', reference_dir,
                         {'MPIRUN': f'{sys.executable} -B {os.path.abspath(__file__)} '
                                    f'launch-same-job-id {state} {launcher}',
                          'TMPDIR': session})
    finally:
        shutil.rmtree(session, ignore_errors=True)
    jobs = open(state, encoding='ascii').read().split('\n')[:-1]
    if len(jobs) != 2 or len({j.split()[0] for j in jobs}) != 1 or \
            any(not j.endswith(' seen') for j in jobs):
        fail(f'the two client jobs did not run with one Open MPI job id: {jobs} ("unplaced": '
             f'the launcher took no pid of the first job\'s family in time)')
    print(f'two client jobs with the Open MPI job id {jobs[0].split()[0]} kept apart')


def session_processes(session):
    """The pids of the processes of a session (/proc/<pid>/stat, field 6)."""
    pids = []
    for entry in os.listdir('/proc'):
        if entry.isdigit():
            try:
                with open(f'/proc/{entry}/stat', encoding='ascii', errors='replace') as stat:
                    fields = stat.read().rsplit(')', 1)[1].split()
            except OSError:
                continue
            if int(fields[3]) == session:
                pids.append(int(entry))
    return pids


def backing_directories(pids):
    """The values of OMPI_MCA_btl_sm_backing_directory in the environments of the processes."""
    found = set()
    for pid in pids:
        try:
            with open(f'/proc/{pid}/environ', 'rb') as environ:
                for entry in environ.read().split(b'\0'):
                    if entry.startswith(b'OMPI_MCA_btl_sm_backing_directory='):
                        found.add(entry.split(b'=', 1)[1].decode())
        except OSError:
            continue
    return found


def check_interrupt(program, workdir, reference_dir):
    """runWithClients.sh, ended with SIGINT while it measures, ends what it started and removes
    the directory of its MPI jobs' shared-memory files."""
    mpi_launcher()
    os.makedirs(workdir, exist_ok=True)
    for name in ('shortDelayResults.txt', 'referenceLine.txt', 'server.log', 'clients.log'):
        if os.path.exists(os.path.join(workdir, name)):
            os.remove(os.path.join(workdir, name))
    shutil.copy(os.path.join(reference_dir, 'referenceLine.txt'), workdir)
    # Evaluations of a second over many iterations: the run is still measuring when it is ended
    script = subprocess.Popen(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                               'beast', '2', '4', '100', '(1,0)', '(0,10), (0,20), (0,30)', '1'],
                              env=dict(os.environ, GROUP='3', INTER_DELAY='0'),
                              start_new_session=True, stdout=subprocess.DEVNULL,
                              stderr=subprocess.STDOUT)
    try:
        log = os.path.join(workdir, 'server.log')
        for _ in range(600):
            if os.path.exists(log) and 'Warm-up run' in open(log, encoding='utf-8',
                                                             errors='replace').read():
                break
            time.sleep(0.1)
        else:
            fail('the run did not begin to measure within 60 s')
        # The directory the script made for its MPI jobs (the client jobs' directories lie in it).
        # Every process of the session also inherits the directory this check was given by ctest
        # (GEM_ADD_MPI_TEST), if any; it is not the script's.
        job_dirs = backing_directories(session_processes(script.pid))
        job_dirs.discard(os.environ.get('OMPI_MCA_btl_sm_backing_directory'))
        shm_dirs = {os.path.dirname(d) for d in job_dirs}
        if len(shm_dirs) != 1 or not os.path.basename(next(iter(shm_dirs))).startswith(
                'geneva-sm.'):
            fail(f'the client jobs do not have one geneva-sm.* directory: {sorted(job_dirs)}')
        shm_dir = next(iter(shm_dirs))
        # To the script alone. A Ctrl-C at a terminal reaches the whole foreground process group
        # at once; the script's background processes ignore SIGINT, so this ends the same way.
        # It is also the case of `kill -INT <script>`.
        script.send_signal(signal.SIGINT)
        try:
            script.wait(timeout=30)
        except subprocess.TimeoutExpired:
            fail('the script did not end within 30 s of SIGINT')
        left = []
        for _ in range(300):
            left = session_processes(script.pid)
            if not left:
                break
            time.sleep(0.1)
        if left:
            fail(f'{len(left)} processes the script started were still running 30 s after it '
                 f'ended: {left}')
        if os.path.exists(shm_dir):
            fail(f'the directory of the MPI jobs\' shared-memory files is left: {shm_dir} '
                 f'holds {sorted(os.listdir(shm_dir))}')
    finally:
        for pid in session_processes(script.pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        if script.poll() is None:
            script.kill()
            script.wait()
    print(f'the script ended with {script.returncode} and left no process and no '
          f'geneva-sm.* directory behind')


def check_groups(program, workdir):
    """A job that does not form whole groups ends at once, non-zero, on every rank; so does a job
    of -c mpi whose submitter fails while its leads wait for it."""
    os.makedirs(workdir, exist_ok=True)
    os.chdir(workdir)
    # The second failure of the submitter: no serial reference in the work directory
    if os.path.exists('referenceLine.txt'):
        os.remove('referenceLine.txt')
    launcher = mpi_launcher()
    for ranks, args, expected in (
            ('6', ['-c', 'mpi', '--group', '3'], 'the MPI job has 6 processes, but with -c mpi '
             'and --group 3 it needs 1 + clients x 3'),
            ('2', ['-c', 'beast', '--client', '--group', '3'], 'the MPI job has 2 processes, '
             'but a client with --group 3 is one job of 3'),
            # An option only the submitter's Go2 refuses (the leads parse leniently)
            ('4', ['-c', 'mpi', '--group', '3', '--no-such-option', '1'], 'no-such-option'),
            ('4', ['-c', 'mpi', '--group', '3'], 'no serial reference in "referenceLine.txt"')):
        status, out = run_bounded(launcher + [ranks, program] + args, 60)
        if status == 0 or expected not in out:
            fail(f'{ranks} processes with {args} were not refused (exit {status}) '
                 f'with "{expected}": {out[-1000:]}')
        print(f'{ranks} processes with {" ".join(args)}: refused')


def check_bounded():
    """A command that does not end within its limit: the failure carries its output."""
    try:
        # The output is computed, so that the command line the failure names does not hold it
        run_bounded(['sh', '-c', 'echo output-$((6 * 7)); exec sleep 60'], 1)
    except SystemExit as e:
        message = str(e.code)
        if 'did not end within 1 s' not in message or 'output-42' not in message:
            fail(f'the failure at the limit does not carry the output: {message}')
        print('a command at its limit is ended, and the failure carries its output')
        return
    fail('a command that ran into its limit did not fail the check')


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
    # Two runs of at most 120 s and 20 s to end each fit into the entry's TIMEOUT of 300 s
    status, _ = run_bounded(['bash', os.path.join(HERE, 'runWithClients.sh'), program, workdir,
                             'beast', '2', '4', '2', '(0,10)', '(0,10), (0,20), (0,30)', '1'],
                            120, env=dict(os.environ, INTER_DELAY='0', **env), capture=False)
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
    elif len(args) >= 3 and args[0] == 'samejobid':
        check_same_job_id(os.path.abspath(args[1]), os.path.abspath(args[2]),
                          os.path.abspath(args[3]) if len(args) > 3 else None)
    elif len(args) >= 1 and args[0] == 'placement-killed':
        check_placement_killed()
    elif len(args) >= 1 and args[0] == 'placement':
        check_placement(int(args[1]) if len(args) > 1 else 2)
    elif len(args) >= 3 and args[0] == 'launch-same-job-id':
        sys.exit(launch_same_job_id(args[1], args[2:]))
    elif len(args) >= 4 and args[0] == 'interrupt':
        check_interrupt(os.path.abspath(args[1]), os.path.abspath(args[2]),
                        os.path.abspath(args[3]))
    elif len(args) >= 3 and args[0] == 'groups':
        check_groups(os.path.abspath(args[1]), os.path.abspath(args[2]))
    elif len(args) >= 4 and args[0] == 'gate':
        check_gate(os.path.abspath(args[1]), os.path.abspath(args[2]), os.path.abspath(args[3]))
    elif len(args) >= 1 and args[0] == 'bounded':
        check_bounded()
    elif len(args) >= 3 and args[0] == 'refusal':
        check_refusal(os.path.abspath(args[1]), os.path.abspath(args[2]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
