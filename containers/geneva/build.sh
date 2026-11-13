#!/usr/bin/env bash
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

# Builds the Geneva container image with Podman or Docker, whichever is installed (Podman first;
# CONTAINER_ENGINE=docker chooses Docker when both are). It only assembles the build command:
# everything that happens inside the image is in the Containerfile next to this script.
#
#   containers/geneva/build.sh                 the CPU image, tagged geneva:cpu
#   containers/geneva/build.sh --cuda          with the CUDA toolkit and the GPU parts, geneva:cuda
#   containers/geneva/build.sh -- --build-arg COMPILER=clang
#                                              anything after "--" goes to the engine unchanged
#
# The script may be called from anywhere; the build context is always the root of the checkout
# it belongs to.

set -euo pipefail

usage() {
	cat <<EOF
Usage: $(basename "$0") [--cuda] [--tag NAME] [--no-tests] [-- ENGINE-ARGS...]

  --cuda        add the CUDA toolkit and build the GPU consumer (default tag geneva:cuda)
  --tag NAME    the image name (default geneva:cpu, or geneva:cuda with --cuda)
  --no-tests    skip the test suite during the image build
  --            pass the remaining arguments to podman/docker build unchanged,
                e.g. -- --build-arg COMPILER=clang --build-arg BUILD_MODE=Debug

The engine is podman if installed, else docker; CONTAINER_ENGINE overrides the choice.
EOF
}

WITH_CUDA=0
TAG=""
RUN_TESTS=1
EXTRA=()
while [ $# -gt 0 ]; do
	case "$1" in
		--cuda) WITH_CUDA=1 ;;
		--tag) shift; TAG="${1:?--tag needs a name}" ;;
		--no-tests) RUN_TESTS=0 ;;
		-h|--help) usage; exit 0 ;;
		--) shift; EXTRA=("$@"); break ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
	shift
done

if [ -n "${CONTAINER_ENGINE:-}" ]; then
	ENGINE="${CONTAINER_ENGINE}"
elif command -v podman >/dev/null 2>&1; then
	ENGINE=podman
elif command -v docker >/dev/null 2>&1; then
	ENGINE=docker
else
	echo "Neither podman nor docker was found. Install one of them (see containers/geneva/README.md)." >&2
	exit 1
fi

if [ -z "${TAG}" ]; then
	if [ "${WITH_CUDA}" = "1" ]; then TAG=geneva:cuda; else TAG=geneva:cpu; fi
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"

set -x
exec "${ENGINE}" build \
	-f "${HERE}/Containerfile" \
	-t "${TAG}" \
	--build-arg WITH_CUDA="${WITH_CUDA}" \
	--build-arg RUN_TESTS="${RUN_TESTS}" \
	"${EXTRA[@]}" \
	"${ROOT}"
