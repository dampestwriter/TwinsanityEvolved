# tools/ci/linux.Dockerfile -- the Linux lane's toolchain image for an OFFLINE reproduction of the release workflow's
# Linux job (the engine's package list + what the port's toolchain fetch and the static FFmpeg recipe need):
#
#   docker build -f tools/ci/linux.Dockerfile -t te-linux .
#   docker run --rm -e TE_MIN_FREE_GB=3 -v "$PWD:/src" -w /src te-linux tools/ci/release_dist.sh --jobs 3
#   docker build -f tools/ci/linux.Dockerfile --platform linux/amd64 -t te-linux-amd64 .     # the x86_64 lane on an arm64 host
#
# The tree is bind-mounted, not copied: dist/linux-<arch>/ and the zip land in the host's checkout. Game data is never
# in the image or the tree (tools/ci/asset_scan.py).
ARG UBUNTU=24.04
FROM ubuntu:${UBUNTU}
ENV DEBIAN_FRONTEND=noninteractive
# The engine's list (engine/tools/ci/linux_packages.txt) + curl/unzip/xz (the toolchain fetch), nasm + make (the static
# FFmpeg recipe), file (the artifact check).
COPY engine/tools/ci/linux_packages.txt /tmp/linux_packages.txt
RUN apt-get update && apt-get install -y --no-install-recommends \
        $(grep -v '^#' /tmp/linux_packages.txt | xargs) curl unzip xz-utils nasm make file \
    && rm -rf /var/lib/apt/lists/* \
    && clang --version | head -1 && cmake --version | head -1
ENV SDL_VIDEODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 CC=clang CXX=clang++
WORKDIR /src
