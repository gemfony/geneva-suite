# Intended use of the Gemfony Scientific Library Collection

The one home of the collection's intended use (Invariant 20). Invariant 14 measures a generalization
against it, Invariant 23 measures reviews of public API against it, and the manual's introduction
reflects it in a box (manual invariant D15). Status: first version of 2026-11-22, to be curated by the
maintainer.

## Who uses the collection

The collection is a set of C++23 libraries that **other programs link against**. Its users write those
programs. The programs inside this repository (examples, tests, benchmarks, documentation programs) only
demonstrate and check the libraries; they are not its users and do not show the full range of use
(Invariant 23).

## What it is for

- **Geneva: parametric optimization of expensive problems.** The typical evaluation of one candidate
  takes seconds, minutes or hours: a simulation, a numerical model, a measurement chain, an external
  program. Problems with evaluations in the microsecond range are allowed, but the libraries are not
  tuned for them.
- **Several algorithms on one problem description.** A user describes the problem once and can run any
  of the algorithms on it, alone or chained.
- **The same program from one core to many machines.** It runs serially, multi-threaded, on GPUs, or
  distributed over a network (MPI, websocket, MQTT), from a workstation to clusters and HPC systems
  under a batch system, with on the order of a thousand nodes. The choice is made at start-up (command
  line, configuration file), without changing the user's code.
- **The libraries below Geneva on their own.**
  - courtier distributes any kind of compute task;
  - hap supplies random numbers in quantity;
  - weft serializes objects;
  - dietrich turns collected data into plots for several backends;
  - common holds the shared foundation, including the recording of data series.
  Programs may use them without any optimization.
- **Extension at run time.** Users add their own individuals, optimization algorithms and consumers as
  loadable modules, on an equal footing with the shipped ones.
- **Watching and recording a run.** Recorded data go to files that other tools read (CSV, JSON Lines,
  plot scripts).

## The conditions it assumes

- **A trusted network** between the processes of one run (a cluster, or a VPN or ssh tunnel). The
  libraries do not authenticate or encrypt.
- **Linux** as the main platform, with recent GCC and Clang, CMake and a small set of Boost components.
  Other platforms are not excluded but are not tested.
- **Random numbers are not reproducible** run to run (Invariant 18). Results are judged statistically.
  Reproducible sequences are not hap's use case: they would need a very large rebuild of the framework,
  and they depend on the machine architecture as well. A program that needs them uses the engines of
  the C++ standard library (maintainer, 2026-11-22).
- **Long runs end cleanly** on a batch system's warning (SIGHUP, SIGUSR1) and resume from checkpoints.

## What it is not meant for

- A stand-alone optimization application with a user interface. It is a toolkit for programs.
- Hard real-time use, or results that must be bit-for-bit reproducible.
- Untrusted networks without a protecting layer.
- Being the fastest library for small, cheap problems on one machine.

## How this file is used

- **Invariant 14:** a generalization is built when its use case can be named, and this file is where
  the intended uses are. A use case that this file covers counts as named.
- **Reviews (Invariant 23):** they judge public API against this file, not against the callers in the
  tree.
- **The manual's introduction** reflects this file in a box and follows every change.
