"""Containerfile (Dockerfile) generation for the release-test images.

A single image is produced per guest OS. That image carries BOTH toolchains
(a recent GCC and a recent Clang) plus CMake, Doxygen and, optionally,
OpenMPI, and it builds **Boost 1.91.0 from source in C++23 mode** once with the
system GCC -- the same mode Geneva is compiled in (INSTALL, "Compiling Boost").

Why one image for both compilers: on Linux, GCC and Clang share the same
``libstdc++`` C++ standard library, so a Boost built with GCC links cleanly
against Geneva object code produced by Clang. This is verified by actually
building Geneva with both compilers against the single GCC-built Boost (see the
release matrix). Only if a future Clang genuinely fails to link against the
GCC-built Boost would a per-compiler Boost be warranted.

The distro ``libboost-*-dev`` packages are intentionally NOT used: the Ubuntu
archives ship Boost releases older than the required 1.91, and not compiled in
C++23 mode.
"""

from __future__ import annotations

from ..model import GuestOS

# Pinned Boost release built from source inside every image.
BOOST_VERSION = "1.91.0"
BOOST_UNDERSCORE = BOOST_VERSION.replace(".", "_")
BOOST_TARBALL = f"boost_{BOOST_UNDERSCORE}.tar.bz2"
BOOST_URL = (
    f"https://archives.boost.io/release/{BOOST_VERSION}/source/{BOOST_TARBALL}"
)

# Where the from-source Boost is installed inside the image.
IMAGE_BOOST_ROOT = "/opt/boost"

# The compiled Boost components Geneva links against: GENEVA_BOOST_LIBS in
# CMakeModules/CommonGenevaBuild.cmake. (Serialization and Filesystem are gone
# since the move to Gem::Weft; the tests use Catch2, not Boost.Test.)
BOOST_LIBRARIES = "atomic,json,program_options,regex"


def _base_packages(with_mpi: bool) -> str:
    """APT packages common to every image (toolchains + build tooling).

    Both GCC and Clang are installed so the one image serves both compilers.
    No Boost packages here — Boost is built from source below.
    """
    pkgs = [
        "ca-certificates",
        "curl",
        "wget",
        "git",
        "build-essential",   # gcc/g++ + make
        "g++",
        "clang",             # distro clang (22 on 26.10)
        "lld",
        "llvm",
        "libomp-dev",
        "cmake",             # 4.3 on 26.10, >= 3.27
        "ninja-build",
        "catch2",            # Catch2 v3 (test framework; CMake config + lib)
        "doxygen",
        "graphviz",
        "bzip2",
        "xz-utils",
        "python3",
    ]
    if with_mpi:
        pkgs += ["openmpi-bin", "libopenmpi-dev"]
    return " ".join(pkgs)


def containerfile(guest: GuestOS, *, with_mpi: bool = False,
                  boost_root: str | None = None) -> str:
    """Render the Containerfile for one guest OS.

    The image installs both toolchains and builds Boost 1.91 from source with
    GCC in C++23 mode (``cxxflags=-std=c++23``). Build jobs later mount the read-only
    Geneva source and a writable build dir and run ``prepareBuild.sh``.

    ``boost_root`` is where Boost is installed inside the image (and exported as
    ``BOOST_ROOT``); it MUST match the build-time ``BOOSTROOT`` derived from the
    same config value, so the from-source Boost is actually found. Defaults to
    IMAGE_BOOST_ROOT (``/opt/boost``).
    """
    boost_root = boost_root or IMAGE_BOOST_ROOT
    pkgs = _base_packages(with_mpi)
    return f"""\
FROM {guest.image}

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# --- Toolchains and build tooling (both GCC and Clang) ---------------------
RUN apt-get update \\
 && apt-get install -y --no-install-recommends {pkgs} \\
 && rm -rf /var/lib/apt/lists/*

# --- Clang compiler-rt sanitizer runtimes (for the Sanitize build type) -----
# The distro `clang` package does not pull in libclang_rt.tsan/asan/etc., so a
# Clang Sanitize build fails at link ("cannot find libclang_rt.tsan-*.a").
# Install the runtime package matching the installed Clang major version.
RUN CLANG_MAJOR="$(clang --version | sed -nE 's/.*version ([0-9]+).*/\\1/p' | head -1)" \\
 && apt-get update \\
 && apt-get install -y --no-install-recommends "libclang-rt-${{CLANG_MAJOR}}-dev" \\
 && rm -rf /var/lib/apt/lists/*

# --- Boost {BOOST_VERSION} from source, C++23 ABI --------------------------------
# Built ONCE with the system GCC; GCC and Clang share libstdc++ on Linux so the
# same Boost serves both compilers. The distro libboost packages are
# deliberately NOT installed: Geneva requires Boost >= 1.91 built in C++23.
RUN cd /tmp \\
 && curl -fsSL -o {BOOST_TARBALL} "{BOOST_URL}" \\
 && tar xf {BOOST_TARBALL} \\
 && cd boost_{BOOST_UNDERSCORE} \\
 && ./bootstrap.sh --prefix={boost_root} \\
        --with-libraries={BOOST_LIBRARIES} \\
 && ./b2 -j"$(nproc)" \\
        cxxflags="-std=c++23" \\
        variant=release \\
        link=shared \\
        threading=multi \\
        runtime-link=shared \\
        install \\
 && cd / \\
 && rm -rf /tmp/boost_{BOOST_UNDERSCORE} /tmp/{BOOST_TARBALL}

# The jobs run as root inside the container, which Open MPI refuses unless told
# otherwise; without this every mpirun-launched test fails before it starts.
ENV OMPI_ALLOW_RUN_AS_ROOT=1
ENV OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1

ENV BOOST_ROOT={boost_root}
ENV LD_LIBRARY_PATH={boost_root}/lib:$LD_LIBRARY_PATH

WORKDIR /work
"""
