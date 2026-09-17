# physmem2

A small threaded memory tester for userspace. It writes patterns to locked
anonymous pages, reads them back, and for every mismatch prints the **physical**
address of the failing word alongside the expected and observed values.

Written for a single diagnosis: intermittent single-bit corruption on a non-ECC
desktop board, which a conventional single-threaded test reported as clean.
Offered as-is under the MIT license. Not maintained, and not intended to grow
into a general-purpose suite.

## Why this and not something else

- **memtest86+** is the right tool when you can take the machine out of service.
  It tests all of memory, including what a running kernel holds. This does not.
- **stressapptest** overlaps most: threaded, bandwidth-hungry, verifies as it
  goes. Reach for it first if it is packaged for your distro. It reports virtual
  addresses, so tying a failure to a location takes an extra step.
- **memtester** runs in userspace like this one, but single-threaded and without
  physical addresses, so it can miss faults that only appear under sustained
  multi-channel bandwidth.
- **physmem2** is for the narrow case where you want, from a running system,
  repeatable physical addresses for failing words under heavy multi-threaded
  load, in a file short enough to read before trusting it.

It is deliberately 74 lines of C with no dependencies beyond glibc and pthreads.

## Build

```sh
gcc -O2 -pthread -o physmem2 physmem2.c
```

Linux only: it uses `/proc/self/pagemap`, `MAP_LOCKED` and `MAP_POPULATE`.

## Run

```sh
physmem2 [threads] [MB_per_thread] [passes]
```

Defaults are 24 threads, 1024 MB per thread, 3 passes. The total footprint is
`threads × MB_per_thread`, all of it locked resident, so keep it comfortably
below installed RAM. For example:

```sh
physmem2 24 1200 6     # 28 GB across 24 threads, 6 passes
```

**Run it as root.** Two reasons:

- `/proc/self/pagemap` only reveals page frame numbers to a privileged process.
  Unprivileged, the physical address column reads `0x000000000000` and results
  cannot be tied to a location.
- `MAP_LOCKED` needs privilege or a raised `RLIMIT_MEMLOCK`; otherwise the
  worker reports `mmap failed` and exits.

## What each pass does

Per pass, for each of six patterns (`AA`, `55`, `FF`, `00`, `0F0F`, `CCCC`):

1. fill, then verify;
2. march up, verifying and writing the inverse;
3. march down, verifying the inverse and restoring;
4. a random-access sweep over a quarter of the words, to defeat the prefetcher
   and spread row activations;
5. a final verify.

## Output

```
threads=24 bytes/thread=1200MB passes=6 total=28GB
ERR t=17 phase=minv-dn paddr=0x000c14794e78 exp=0xaaaaaaaaaaaaaaaa got=0xaaaaaabaaaaaaaaa xor=0x0000001000000000 nbits=1
TOTAL ERRORS: 7
```

`phase` names which step caught it, `xor` isolates the flipped bits and `nbits`
counts them. Only the first 300 error lines print; the total counts them all.

A single bit failing repeatedly at a fixed physical address, across patterns and
phases, is the signature of a defective cell. Bursty multi-bit errors scattered
over many addresses point at the controller, timing or a slot instead.

## Two things worth knowing

- **Compiler barriers, not `volatile`.** In the first version the verify loops
  were optimized away, and the test reported a clean run on hardware that was in
  fact failing. `volatile` would have prevented that but also serialized the
  loops, and the reduced memory bandwidth made the fault far harder to
  reproduce. This version uses empty `asm` memory barriers, which keeps the
  loads real while leaving the loops vectorized. If you modify it, check that
  your verify loops still emit loads.
- **A physical address is not a slot number.** Translating one to a DIMM, rank
  and bank needs the memory controller's interleave configuration, which this
  program does not read. Repeated failures at a fixed address are strong
  evidence of a defect at that location; identifying which module requires
  testing sticks in isolation.

## Limitations

- Not a substitute for a full march test suite: no address-decode or
  data-retention coverage.
- It reports what the CPU read back. On an ECC system a corrected error may
  never surface here; read the EDAC counters alongside it.
- It only tests memory the kernel hands it, so reserved regions and memory in
  use elsewhere are out of reach.
- Physical addresses are stable only for the life of the mapping, and only
  meaningful on the machine and configuration that produced them.
