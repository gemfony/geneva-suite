# MPI Sub-Clients

This example shows parallelism inside one evaluation: the processes of an MPI job share the
evaluation of an individual. It uses courtier's MPI cascade, `Gem::Courtier::GMPICascade`
(`courtier/mpi/GMPICascade.hpp`, library `gemfony-courtier-mpi`).

Rank 0 of the job is the submitter: it runs the optimization through `GOptimizer`. The other
processes form groups of `sub_client_group_size` processes, set in `config/GMPICascade.json` (4 by
default). The first process of a group is its lead, a client of the MPI consumer; the others are
its members (sub-clients), which run no Geneva code. A lead evaluates every individual in a round
of its group: it scatters the 64 parameter values over the group, every process squares its part
(each square stands for 50 ms of work) and the lead gathers the sums. Where the group size does not
divide the number of parameters, the lead also takes the rest. The individual itself knows nothing
of MPI; with `--consumer stc` it is evaluated in one process.

The roles follow the rank r in the job, with the group size g:

- r = 0: the submitter;
- r > 0 with (r - 1) % g == 0: a lead (a client), the first of group (r - 1) / g;
- every other r > 0: a member of group (r - 1) / g.

A last group with fewer than g processes is allowed.

The executable can be started as follows:

```
mpirun -np <1 + number of clients * subgroup-size> ./GMPISubClients --consumer mpi
```

For example to run this example with 1 server, 4 clients and 3 sub-clients for each client set the `sub_client_group_size` parameter in the config file to 4 (1 client + 3 sub-clients) and start it with:

```
mpirun -np 17 ./GMPISubClients --consumer mpi
```

The shipped `config/GEvolutionaryAlgorithm.json` has a small population of 8, so that a run is
short.

Note: if you request more ranks than your machine has cores, Open MPI additionally needs the
`--oversubscribe` flag. MPICH/Hydra (where `mpirun` is `mpiexec.hydra`) does not know that flag and
oversubscribes by default, so the plain commands above work there as-is.

The cascade has a second layout, in which the leads are clients of a networked consumer (websockets)
and each group is an MPI job of its own; the benchmark `GSubClientOverhead` shows it.

## Files

- `GMPISubClients.cpp` -- `main()`: the cascade first, then courtier's launcher (the long form);
  the leads hand the launcher the group's item processor.
- `GParaboloidGroupIndividual.{hpp,cpp}` -- the individual: 64 parameters and an `evaluate()` that
  computes the parabola in one process. It knows nothing of MPI.
- `GParaboloidGroup.{hpp,cpp}` -- the lead's and the members' halves of one evaluation on the
  cascade: the lead scatters the parameters over its group, takes the rest itself, and returns the
  sum of the parts it gathers.
- `GParaboloidPart.hpp` -- the work every process does on its part: square the values (each square
  standing for a fixed delay) and sum them.

