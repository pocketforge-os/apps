#!/bin/sh
# Cross-build the apps tools for the device (aarch64, glibc 2.33) inside the
# pinned toolchain container. Run on a host that has the container image
# (modelmaker or the Dell):
#
#   ./scripts/cross-build.sh
#
# From the laptop, against a checkout on modelmaker:
#
#   rsync -a --delete ./ mm@10.0.40.90:/tmp/apps-build/
#   ssh mm@10.0.40.90 'cd /tmp/apps-build && ./scripts/cross-build.sh'
#   rsync -a mm@10.0.40.90:/tmp/apps-build/build/aarch64/ ./build/aarch64/
#
# Why the container: the device userspace is glibc 2.33; stock Ubuntu cross
# gcc emits glibc-2.34+ symbol versions the device loader rejects
# (build-integration-reference.md §3.6). The container carries the ARM
# A-profile 10.3-2021.07 toolchain.
set -eu

IMAGE=${PF_BUILD_IMAGE:-pocketforge/build:10.3-2021.07-bookworm}
TOOLCHAIN=${PF_CROSS_PREFIX:-/opt/arm-10.3-2021.07/bin/aarch64-none-linux-gnu-}

exec docker run --rm \
    -v "$(pwd)":/work -w /work \
    --user "$(id -u):$(id -g)" \
    "$IMAGE" \
    make cross CROSS_COMPILE="$TOOLCHAIN"
