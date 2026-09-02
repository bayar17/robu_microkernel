FROM ubuntu:24.04

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    clang lld llvm make flex bison mtools e2fsprogs bzip2 \
    qemu-system-x86 meson libc++-dev libc++abi-dev git ca-certificates \
    python3 \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*
