#!/bin/sh
# Usage (under an MPI launcher): runInRankDirectory.sh <program> [arguments...]
#
# Runs <program> in a directory of its own per rank, below the current one, so that ranks which
# write configuration files do not write the same files at the same time. The rank comes from the
# launcher's environment (Open MPI or a PMI launcher such as MPICH's); the process id stands in for
# it under any other launcher.
dir="rank-${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-$$}}"
mkdir -p "$dir" && cd "$dir" && exec "$@"
