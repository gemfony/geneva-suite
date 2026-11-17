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
      started). The run passes two client options, the prefetch depth and the evaluations at a
      time, and checks that both clients used them.
  testSpeedupTools.py subclients <GSubClientOverhead> <workdir> <consumer> [<reference dir>]
      a short run with two clients of two sub-clients each: every evaluation is done once, in a
      group of three, every process ends by itself, and both clients used the prefetch depth and
      the evaluations at a time the run passes
  testSpeedupTools.py samejobid <GSubClientOverhead> <workdir> [<reference dir>]
      as subclients with beast, but Open MPI gives both client jobs the same job id (their
      launcher is placed on pids whose ids collide): the jobs must not share Open MPI's shared
      memory (runWithClients.sh gives each job a directory of its own for it); Open MPI only
  testSpeedupTools.py interrupt <GSubClientOverhead> <workdir> <reference dir>
      runWithClients.sh with two websocket clients of three processes each, ended from outside
      with SIGINT while it measures: no process it started may remain, nor the geneva-sm.*
      directory of its MPI jobs' shared-memory files
  testSpeedupTools.py groups <GSubClientOverhead> <workdir>
      a job that does not form whole groups is refused, in both layouts, with a message that
      names its size and the group; a job of -c mpi whose submitter fails (an option only it
      refuses, no serial reference) ends at once, with the submitter's message
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
import tempfile
import threading
import time

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
                              # Every evaluation exactly once: no item may be given to a second
                              # client because its first one was slow under load (an adaptive
                              # lease).
                              env=dict(os.environ, GROUP='3', CLIENT_GRACE='60', INTER_DELAY='0',
                                       TIMEOUT_TREATMENT='wait_indefinitely', **(env or {})))
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


def job_family(host, pid):
    """The upper 16 bits of the job id Open MPI 5 gives the job of the mpirun with this pid: its
    namespace "prterun-<host>-<pid>" hashed (Jenkins' one-at-a-time hash) and folded to 16 bits.
    Open MPI names the files of its shared-memory transport after the job id, so two independent
    jobs of one family on one host use the same files unless each has a directory of its own."""
    value = 0
    for char in f'prterun-{host}-{pid}'.encode():
        value = (value + char) & 0xffffffff
        value = (value + (value << 10)) & 0xffffffff
        value ^= value >> 6
    value = (value + (value << 3)) & 0xffffffff
    value ^= value >> 11
    value = (value + (value << 15)) & 0xffffffff
    return ((value >> 16) ^ value) & 0xffff


def last_pid():
    """The pid this pid namespace gave last."""
    with open('/proc/sys/kernel/ns_last_pid', encoding='ascii') as pid_file:
        return int(pid_file.read())


def start_on(command, expected=None):
    """Starts command in a child process and returns its pid; with expected, only if the child
    gets that pid (otherwise None)."""
    child = os.fork()
    if child == 0:
        if expected is not None and os.getpid() != expected:
            os._exit(99)  # pylint: disable=protected-access
        try:
            os.execvp(command[0], command)
        finally:
            os._exit(127)  # pylint: disable=protected-access
    if expected is not None and child != expected:
        os.waitpid(child, 0)
        return None
    return child


def start_in_family(command, host, family, limit=120):
    """Starts command (mpirun and its arguments) on the next pid whose job has the given family:
    the pids before it are used up by threads that end at once. Gives up after limit seconds."""
    with open('/proc/sys/kernel/pid_max', encoding='ascii') as pid_file:
        pid_max = int(pid_file.read())
    cycle = pid_max - 300  # past pid_max the pids start again at 300

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
            # The next pids of the family, in the order the kernel gives them
            start, candidates, pid = current, [], current
            for _ in range(600000):
                pid = pid + 1 if pid + 1 < pid_max else 300
                if job_family(host, pid) == family:
                    candidates.append(pid)
        position = (current - start) % cycle
        candidates = [c for c in candidates if (c - start) % cycle > position]
        if not candidates:
            continue
        distance = (candidates[0] - start) % cycle - position
        if distance == 1:
            child = start_on(command, candidates[0])
            if child is not None:
                return child
        else:
            burn(distance - 48 if distance > 64 else 1)
    fail(f'no pid of the job family {family:04x} could be taken within {limit} s')
    return None


def launch_same_job_id(state, command):
    """The launcher of the samejobid check (its MPIRUN): starts command, an mpirun with its
    arguments, so that Open MPI gives its job the id of the first job started this way. The first
    launcher writes its job's family to <state>.first, the later ones place their mpirun on a pid
    of that family. Each launcher then looks for the job's shared-memory files under that id in
    the job's directory for them and appends "<job id> seen" (or "unseen") to state, so that the
    check knows the ids really were equal. Returns the exit code of the job."""
    host = socket.gethostname().split('.')[0]
    try:
        first = os.open(state + '.first', os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        first = None
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
        out.write(f'{job_id:x} {"seen" if seen else "unseen"}\n')
    if status is None:
        status = os.waitpid(child, 0)[1]
    code = os.waitstatus_to_exitcode(status)
    return code if code >= 0 else 128 - code


def check_same_job_id(program, workdir, reference_dir):
    """As check_subclients() with beast, but both client jobs get the same Open MPI job id."""
    os.makedirs(workdir, exist_ok=True)
    state = os.path.join(workdir, 'jobids.txt')
    for name in (state, state + '.first'):
        if os.path.exists(name):
            os.remove(name)
    launcher = os.environ.get('MPIRUN', 'mpirun --oversubscribe -np')
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
        fail(f'the two client jobs did not run with one Open MPI job id: {jobs}')
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
        # The directory the script made for its MPI jobs (the client jobs' directories lie in it)
        job_dirs = backing_directories(session_processes(script.pid))
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
    launcher = os.environ.get('MPIRUN', 'mpirun --oversubscribe -np').split()
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
    elif len(args) >= 3 and args[0] == 'launch-same-job-id':
        sys.exit(launch_same_job_id(args[1], args[2:]))
    elif len(args) >= 4 and args[0] == 'interrupt':
        check_interrupt(os.path.abspath(args[1]), os.path.abspath(args[2]),
                        os.path.abspath(args[3]))
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
