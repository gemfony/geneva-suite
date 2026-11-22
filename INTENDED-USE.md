# Intended use of the Gemfony Scientific Library Collection

The one home of the collection's intended use (Invariant 20). Invariant 14 measures a generalization
against it, Invariant 23 measures reviews of public API against it, and the manual states it in its
appendix chapter "Intended Use" (manual invariant D15). Curated by the maintainer, 2026-11-22.

## Who uses the collection

The collection is a set of C++23 libraries that other programs link against. Its users are the authors
of those programs. Examples, tests, benchmarks and documentation programs of the source tree
demonstrate and check the libraries, but do not show the full range of their use.

## What it is for

- **Parametric optimization of computationally expensive problems.** The typical evaluation of one
  candidate takes seconds, minutes or hours: a simulation, a numerical model, a chain of measurements or
  an external program. Problems whose evaluations take microseconds can be solved as well, but should
  restrict themselves to local execution.
- **Several optimization algorithms target one problem description.** A problem is described once, and
  any of the algorithms can run on it, alone or chained, one after the other.
- **The same program from one core to many machines.** A program runs serially, multi-threaded, on GPUs
  or distributed over MPI, websockets or MQTT, from a workstation to clusters under a batch system with
  up to about a thousand nodes. The choice is made at start-up, without changes to the user's code.
- **The libraries below Geneva on their own.** courtier distributes compute tasks, hap supplies random
  numbers, weft serializes objects, dietrich plots collected data, and common holds the shared
  foundation; each can be used without any optimization.
- **Extension at run time.** Users add individuals, algorithms and consumers as modules loaded at
  start-up, on an equal footing with the shipped ones.
- **Watching and recording a run.** Recorded data go to files that other tools read (CSV, JSON Lines,
  plot scripts).

## The conditions it assumes

- The processes of a distributed run communicate over a trusted network, a cluster or a connection
  protected by a VPN or an ssh tunnel. The libraries do not authenticate their peers or encrypt their
  traffic.
- Linux is the main platform, with recent versions of GCC and Clang, CMake and a small set of Boost
  components. Other platforms are not excluded, but they are not tested.
- Random numbers are not reproducible from one run to the next, and results are judged statistically. A
  program that needs reproducible random numbers uses the engines of the C++ standard library.
- A long run ends cleanly when a batch system sends its warning (SIGHUP or SIGUSR1), and it can be
  resumed from a checkpoint.

## What it is not meant for

- A stand-alone optimization application with a user interface. The collection can be the basis of
  one, but is itself a toolkit for programs.
- Hard real-time use, or results that must be reproducible bit for bit.
- Untrusted networks without a protecting layer.
- Being the fastest library for small, cheap problems on a single machine. Geneva targets large,
  computationally expensive problems.
