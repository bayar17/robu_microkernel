<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="images/logo-robu-dark.svg">
    <img src="images/logo-robu.svg" alt="Robu Microkernel" width="320">
  </picture>
</div>

<div align="center">

![Architecture](https://img.shields.io/badge/Architecture-armv7%20%7C%20armv8%20%7C%20x86__64%20%7C%20i386%20%7C%20i486%20%7C%20riscv32%20%7C%20riscv64-red)
![Host](https://img.shields.io/badge/Host-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![GCC](https://img.shields.io/badge/GNU-gcc-A42E2B?logo=gnu)
![Clang](https://img.shields.io/badge/LLVM-clang-262D3A?logo=llvm)
![Build](https://img.shields.io/badge/Build-Makefile-427819)
![License](https://img.shields.io/badge/License-MIT-blue)
![Status](https://img.shields.io/badge/Status-alpha-orange)
![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/bayar17/robu_microkernel/ci.yml)

</div>



## Robu Microkernel

A microkernel written from scratch in C and assembly, built on one premise: **the performance cost of a microkernel is an engineering problem, not a law of nature.**

**Prior Art.** The mechanisms below — synchronous register IPC, timeslice donation, lazy scheduling with direct switch, user-space paging — come from the [L4 lineage](https://en.wikipedia.org/wiki/L4_microkernel_family), beginning with Liedtke's work in the early 1990s.

> **Pre-alpha.** Robu does not boot yet and has no stable API. Nothing here is usable for anything but reading and contributing. Interfaces described below will change.

---
#### Project Description 🖥️

| | |
|---|---|
| **Project name** | Robu Microkernel |
| **Shortname** | `robu_kernel` |
| **Language** | C + assembly |
| **Target architectures** | armv7, armv8, x86_64, i386/i486, riscv32, riscv64 |
| **Current dev platform** | x86_64 |
| **Build system** | GNU Make |
| **Toolchains** | gcc, clang |
| **Test environment** | QEMU |
| **License** | MIT |

---

## Design Criteria

### What a microkernel is 🧠

A microkernel keeps only what cannot live anywhere else in the privileged part of the system: threads, address spaces, and message passing. Drivers, filesystems, network stacks, and pagers all run as ordinary user-space processes.

The payoff is isolation. A driver that faults takes down a restartable process instead of the machine. Components hold only the privileges they need, so a compromise stays contained. Subsystems can be replaced, upgraded, or restarted without recompiling — or rebooting — the kernel. That combination is why microkernels dominate in embedded and safety-critical work.

### What it costs — and what Robu does about it 📉

The standard objection is real: pushing services into user space converts function calls into IPC, and naive IPC is expensive. Every cost has a specific location, though, and each one is where Robu spends its design budget.

**IPC round-trip cost.** Every user-space service call becomes at least one message send and one reply. Robu's core path carries short messages in registers, so the common case copies nothing — no marshalling into a kernel buffer, no buffer to allocate, size-check, or free. Transfers are a synchronous rendezvous: send blocks until the peer receives, which removes queue management and buffer lifetime from the fast path entirely.

**Scheduler pressure.** If every IPC blocks a thread and wakes another, a naive kernel runs the scheduler twice per call. Robu uses *lazy scheduling* — the ready queue is not reworked on every block and unblock — and *direct switch*: on a send to a waiting receiver, control transfers straight to that thread without consulting the scheduler at all. The caller's remaining timeslice is *donated* to the callee, so a client-server round trip costs roughly what a protected procedure call should, and a service does not need its own quantum to answer promptly.

**Context-switch cost.** With address-space switches on the critical path, they have to be cheap rather than merely correct. Robu keeps the switch path minimal, saves only what the ABI requires, and avoids touching structures the fast path does not need.

**Memory management policy.** Robu's kernel implements the mapping mechanism and nothing more. Page faults are delegated to a user-space pager, which decides what to do about them. Policy — paging strategy, allocation, sharing — lives outside the kernel, where it can be replaced per-workload.

**Interrupt handling.** In-kernel handlers do the minimum: acknowledge, and turn the event into a message for the user-space driver. Device logic never runs in privileged mode.

### API surface

The kernel exposes a small, deliberately boring set of primitives around threads, address spaces, and IPC. **POSIX is not a kernel interface here** — POSIX-like semantics are provided by a user-space personality server built on those primitives. Applications get a familiar API; the kernel stays small enough to reason about.

## License
 
MIT. See [LICENSE](LICENSE).



