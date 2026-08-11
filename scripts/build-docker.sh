#!/bin/bash
set -e
cd "$(dirname "$0")/.."

if command -v docker >/dev/null 2>&1; then
    ENGINE=docker
    echo "Trying Docker..."
elif command -v podman >/dev/null 2>&1; then
    ENGINE=podman
    echo "Docker not found. Trying Podman..." 
else
    echo "build-docker.sh: neither docker nor podman found on PATH" >&2
    exit 1
fi

IMAGE=robu-builder:latest

"$ENGINE" build --platform=linux/amd64 -t "$IMAGE" -f scripts/robu-builder.Dockerfile . >&2

"$ENGINE" run --rm --platform=linux/amd64 ${QEMU_APPEND+-e QEMU_APPEND} \
    -v "$(pwd)":/work -w /work "$IMAGE" \
    bash -c '
        set -e
        git config --global --add safe.directory /work
        make BUILD_SYS=clang "$@"
    ' _ "$@"
