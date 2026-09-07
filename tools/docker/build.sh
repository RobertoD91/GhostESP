#!/usr/bin/env bash
# Build one GhostESP board target inside the Docker image, no VSCode, no PlatformIO.
#
#   tools/docker/build.sh <config-name> <idf-target> [idf.py args...]
#
# e.g.  tools/docker/build.sh m5core2_aws esp32
#       tools/docker/build.sh cardputer   esp32s3
#       tools/docker/build.sh m5core2_aws esp32 size-components
#
# <config-name> is the suffix of a file in configs/, i.e. "m5core2_aws" for
# configs/sdkconfig.m5core2_aws. <idf-target> must match that config's chip --
# see the matrix in .github/workflows/compile_all.yml.
set -euo pipefail

CONFIG_NAME=${1:?usage: build.sh <config-name> <idf-target> [idf.py args...]}
IDF_TARGET=${2:?usage: build.sh <config-name> <idf-target> [idf.py args...]}
shift 2
IDF_ARGS=("$@")
[ ${#IDF_ARGS[@]} -eq 0 ] && IDF_ARGS=(build)

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IMAGE=${GHOSTESP_IMAGE:-ghostesp-idf:v6.1}
SDKCONFIG="configs/sdkconfig.${CONFIG_NAME}"

# Extra `docker run` flags. The first build has to reach the Espressif
# component registry for the managed components in main/idf_component.yml, so
# behind a proxy you may need e.g.
#   GHOSTESP_DOCKER_ARGS="--network host -e HTTPS_PROXY=$HTTPS_PROXY"
read -r -a EXTRA_DOCKER_ARGS <<< "${GHOSTESP_DOCKER_ARGS:-}"

[ -f "${REPO_ROOT}/${SDKCONFIG}" ] || { echo "no such config: ${SDKCONFIG}" >&2; exit 1; }

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">> building $IMAGE (one-off, a few minutes)"
  docker build -t "$IMAGE" "${REPO_ROOT}/tools/docker"
fi

# Mirror the CI "Apply Custom SDK Config" step exactly: both sdkconfig and
# sdkconfig.defaults must be the board profile, or the build silently uses
# defaults for anything only present in one of them.
docker run --rm -t \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -e IDF_TARGET="$IDF_TARGET" \
  -e SDKCONFIG_DEFAULTS=sdkconfig.defaults \
  -v "${REPO_ROOT}:/project" -w /project \
  ${EXTRA_DOCKER_ARGS[@]+"${EXTRA_DOCKER_ARGS[@]}"} \
  "$IMAGE" \
  bash -lc '
    set -euo pipefail
    rm -f sdkconfig sdkconfig.defaults
    cp "'"$SDKCONFIG"'" sdkconfig.defaults
    cp "'"$SDKCONFIG"'" sdkconfig
    printf "\n# CONFIG_FATFS_USE_DYN_BUFFERS is not set\n" | tee -a sdkconfig.defaults >> sdkconfig
    printf "\n# CONFIG_ESP_GDBSTUB_ENABLED is not set\n"   | tee -a sdkconfig.defaults >> sdkconfig
    export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
    idf.py fullclean >/dev/null 2>&1 || true
    idf.py '"${IDF_ARGS[*]}"'
  '

echo
echo ">> artifacts in build/:"
ls -lh "${REPO_ROOT}/build/Ghost_ESP_IDF.bin" "${REPO_ROOT}/build/bootloader/bootloader.bin" 2>/dev/null || true
