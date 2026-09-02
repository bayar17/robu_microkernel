#!/bin/bash
set -e

cd "$(dirname "$0")/.."

YELLOW='\033[0;33m'
BLUE='\033[0;34m'
RESET='\033[0m'

OS="$(uname -s)"
KERNEL="$(uname -r)"

printf 'OS is %b%s%b, %b%s%b Kernel Version %s\n' \
    "$BLUE" "$OS" "$RESET" \
    "$BLUE" "$OS" "$RESET" \
    "$KERNEL"

if [ -n "$ROBU_IN_DOCKER" ]; then
    exec make "$@"
fi

case "$1" in
    _all|_mlibc)
        if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
            exec "$(dirname "$0")/build-docker.sh" "$@"
        elif command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
            exec "$(dirname "$0")/build-docker.sh" "$@"
        else
            printf '%bidentify-os.sh: no working Docker/Podman daemon found, building natively%b\n' "$YELLOW" "$RESET" >&2
            printf '%bresults may not exactly match the Docker-built CI environment%b\n' "$YELLOW" "$RESET" >&2
            exec make "$@"
        fi
        ;;
    *)
        exec make "$@"
        ;;
esac
