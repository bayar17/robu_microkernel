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
else
    exec "$(dirname "$0")/build-docker.sh" "$@"
fi