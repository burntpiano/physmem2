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

It is deliberately short -- about 90 lines of C -- with no dependencies beyond
glibc and pthreads.

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
  worker reports `mmap failed`, tests nothing, and the run exits 2.

## Running it so the result means something

**Use a large working set.** This is the one that decides whether the test works
at all. A run that fits in cache never reaches DRAM: on the machine this was
written for, a 6 MB working set ran for 13 minutes and found nothing, while a
28 GB working set found 7 errors in under 5 minutes. Size it to most of
installed RAM, leaving the OS room to breathe.

**Pick threads to match the machine.** Roughly one per logical CPU is a
reasonable start. The point is to keep every memory channel busy at once.

**Budget the time.** 28 GB across 24 threads at 6 passes took about 5 minutes on
a 16-core desktop part. Cost scales roughly with total bytes times passes.

**Expect the machine to be unresponsive.** Every page is locked resident, so
most of RAM is unavailable to anything else for the duration. Run it on an idle
box, not a production one.

**Repeat before concluding anything.** Marginal faults are probabilistic: the
errors above were 7 across 6 passes, not 7 per pass. A single clean run is not a
pass, and `exit 0` means only that nothing failed under this load in this time.
Absence of errors here is not evidence that the memory is sound.

**Non-root, if you must:** raise the lock limit (`ulimit -l unlimited`), and
accept that the physical address column will read as zeros.

## Isolating a module

A failing physical address names a location, not a part. To get from one to the
other:

1. Test one module at a time, in the same slot, so a failure follows the module
   rather than the socket.
2. Move a suspect module to a different slot and re-test. A fault that stays
   with the module is the module; one that stays with the slot is the board.
3. Compare **bit positions**, not addresses, between configurations. The map
   from physical address to a DRAM row and column depends on channel and rank
   interleaving, so changing which slots are populated changes the addresses
   while the failing bit lane stays put.

## What each pass does

Per pass, for each of six patterns (`AA`, `55`, `FF`, `00`, `0F0F`, `CCCC`):

1. fill, then verify;
2. march up, verifying and writing the inverse;
3. march down, verifying the inverse and restoring;
4. a random-access sweep of `n/4` indices, to defeat the prefetcher and spread
   row activations. The draws are with replacement, so it touches roughly 22% of
   the words, some more than once: this pass exists for activation pressure, not
   coverage. The three sequential passes are what touch every word.
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

## Exit status

| Code | Meaning |
|---|---|
| 0 | Every thread ran and no error was observed. |
| 1 | Errors were observed. The count is on the last stdout line. |
| 2 | The test could not be completed, so the result is not a verdict. Either `/proc/self/pagemap` would not open, or a thread could not lock its memory; in the latter case an `INCOMPLETE:` line on stderr says how many. |

Code 2 exists so that a clean-looking run cannot be mistaken for a passing one.
A thread whose `mmap` failed tested nothing, and `TOTAL ERRORS: 0` would
otherwise read as a pass over memory that was never touched.

This differs from the version used in the diagnosis that prompted the tool,
which always exited 0. The test itself is unchanged.

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

- **No retention testing.** Nothing ever waits, so a weak cell that leaks over
  seconds or minutes is not caught. That test wants a quiet machine and a long
  pause between write and verify, which is the opposite of this tool's
  sustained-bandwidth design; memtest86+'s bit-fade test is the right instrument
  for it.
- Not a substitute for a full march test suite: there is no address-decode
  coverage.
- It reports what the CPU read back. On an ECC system a corrected error may
  never surface here; read the EDAC counters alongside it.
- It only tests memory the kernel hands it, so reserved regions and memory in
  use elsewhere are out of reach.
- Physical addresses are stable only for the life of the mapping, and only
  meaningful on the machine and configuration that produced them.
