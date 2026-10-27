# Geneva in a container

This directory builds a container image with a complete, installed Geneva, so that nothing has to
be set up on the host except a container engine. It works with **Podman** and **Docker** alike.
The image is a development image: it holds the compilers, Boost and Geneva, and a project of your
own is built inside it exactly as it would be on a machine where Geneva is installed.

What the image build does, in this order (all of it in [`Containerfile`](Containerfile)):

1. installs the compilers, CMake, Catch2 and OpenMPI from Ubuntu 26.10, and optionally the CUDA
   toolkit;
2. compiles Boost from source in C++23 mode, as `INSTALL` describes;
3. configures Geneva with `scripts/prepareBuild.sh` from a copy of `scripts/genevaConfig.gcfg`,
   builds it, runs the test suite (without the long benchmarks), and installs it to `/opt/geneva`;
4. builds `libs/geneva/quickstart` against the installed Geneva, to check that a project
   outside the source tree can use it;
5. keeps the toolchain, `/opt/boost` and `/opt/geneva` in the final image, and drops the build tree.

The sources come from the checkout the build is started in, including any changes you made there.

## Building the image

From anywhere inside the checkout:

```sh
containers/geneva/build.sh            # CPU image with MPI support, tagged geneva:cpu
containers/geneva/build.sh --cuda     # adds the CUDA toolkit and the GPU consumer, geneva:cuda
```

`build.sh` uses Podman if it is installed and Docker otherwise; `CONTAINER_ENGINE=docker` forces
Docker. It does nothing but call the engine, so the same can be done by hand from the root of the
checkout:

```sh
podman build -f containers/geneva/Containerfile -t geneva:cpu .
docker build -f containers/geneva/Containerfile -t geneva:cpu .
```

A first build downloads the base image and Boost and compiles everything, which takes a while; the
engine caches the stages, so a rebuild after a change to Geneva's sources starts at the Geneva
stage.

The build can be adjusted with build arguments, passed to `build.sh` after `--` or to the engine
with `--build-arg`:

| Argument | Default | Meaning |
|---|---|---|
| `COMPILER` | `gcc` | `gcc` or `clang`, for Boost and Geneva |
| `BUILD_MODE` | `Release` | `Release`, `Debug` or `RelWithDebInfo` |
| `RUN_TESTS` | `1` | `0` skips the test suite (`build.sh --no-tests`) |
| `WITH_CUDA` | `0` | `1` adds the CUDA toolkit (`build.sh --cuda`) |
| `CUDA_PACKAGE` | `cuda-toolkit-13-3` | the toolkit package from the Ubuntu archive |
| `BOOST_VERSION` | `1.91.0` | the Boost release compiled from source |
| `BASE_IMAGE` | `docker.io/library/ubuntu:26.10` | the distribution image |
| `JOBS` | all cores | parallel compile jobs |

For example: `containers/geneva/build.sh -- --build-arg COMPILER=clang --build-arg BUILD_MODE=Debug`.

## Using the image

An interactive shell, with the current directory mounted as `/work`:

```sh
podman run --rm -it -v "$PWD":/work:Z geneva:cpu
docker run --rm -it -v "$PWD":/work geneva:cpu
```

(`:Z` relabels the directory for SELinux, which Podman on Fedora or RHEL needs; elsewhere it does
no harm.) Inside, `CMAKE_PREFIX_PATH` already points at Geneva and Boost, so a project that uses
`find_package(Geneva)` builds without further settings:

```sh
cmake -S /work -B /work/build
cmake --build /work/build -j"$(nproc)"
```

The installed examples are under `/opt/geneva/examples`; the manual's quick-start program, for
instance, runs with

```sh
cd /opt/geneva/examples/GQuickStart && ./GQuickStart
```

### With a GPU

The `geneva:cuda` image needs the NVIDIA Container Toolkit on the host, which hands the GPU and
the driver into the container. Ubuntu 26.10 carries it in its own archive:

```sh
sudo apt-get install nvidia-container-toolkit
```

Podman finds the GPU through a CDI description of the installed driver, which the package's
`nvidia-cdi-refresh` service writes to `/var/run/cdi/nvidia.yaml` and renews after driver updates;
`nvidia-ctk cdi list` shows it. Docker needs the runtime registered once:

```sh
sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker
```

Then:

```sh
podman run --rm -it --device nvidia.com/gpu=all geneva:cuda     # Podman, via CDI
docker run --rm -it --gpus all geneva:cuda                       # Docker
```

An image is built where no GPU needs to be present, so the image build compiles the GPU code but
cannot exercise it on a device; that happens when the container runs on a machine with a GPU. The
host driver must support CUDA 13.3 or newer (`INSTALL`, "GPU support on Ubuntu 26.10").

Geneva loads the driver library `libcuda.so.1`, which the NVIDIA Container Toolkit brings in from
the host, only when the GPU consumer is used. Started without access to a GPU, the CUDA image runs
everything else on the CPU; the `geneva:cpu` image is the smaller choice for such machines.

## Your own project in an image

To ship an optimization problem as an image of its own -- for a cluster, say -- start from the
Geneva image. A `Containerfile` in your project directory:

```dockerfile
FROM localhost/geneva:cpu
COPY . /src/myproblem
RUN cmake -S /src/myproblem -B /build/myproblem -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build/myproblem -j"$(nproc)"
WORKDIR /build/myproblem
ENTRYPOINT ["./MyProblem"]
```

`libs/geneva/quickstart` is a complete project of this shape to start from. Arguments given to
`podman run` / `docker run` after the image name then go to the program, for instance `-a swarm` or
the consumer options for a networked run.

Clusters that do not run Podman or Docker usually run Apptainer (formerly Singularity), which
converts such an image directly, for example
`apptainer build myproblem.sif docker-daemon://myproblem:latest` after a Docker build, or from an
archive written with `podman save`.
