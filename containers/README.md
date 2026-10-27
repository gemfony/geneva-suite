# Containers

Container images that take the setup of Geneva off the user's hands. Each subdirectory holds one
image definition, buildable with Podman or Docker:

- [`geneva/`](geneva/README.md) -- Ubuntu 26.10 with the compilers, Boost and a complete, installed
  and tested Geneva, optionally with CUDA. It is the base to build one's own optimization problems
  on, interactively or as an image of their own, for example to run them on a cluster.
