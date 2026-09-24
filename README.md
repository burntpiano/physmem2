# physmem2

A small threaded memory tester for userspace. It writes patterns to locked
anonymous pages, reads them back, and for every mismatch prints the **physical**
address of the failing word alongside the expected and observed values, the
failing data lines and byte lane, and -- given a rank rule and a layout file --
the rank and the chip.

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
  repeatable physical addresses and failing lanes for failing words under heavy
  multi-threaded load, in a file short enough to read before trusting it.

It is deliberately short -- about 200 lines of C, a quarter of them comments --
with no dependencies beyond glibc and pthreads.

## Build

```sh
gcc -O2 -pthread -o physmem2 physmem2.c
```

Linux only: it uses `/proc/self/pagemap`, `MAP_LOCKED` and `MAP_POPULATE`.

## Run

```sh
physmem2 [-r RULE] [-l LAYOUT] [threads] [MB_per_thread] [passes]
```

`-r` and `-l` only change what an error line says, not what is tested; see
[Naming the chip](#naming-the-chip-rank-rules-and-layout-files).

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
   while the failing bit lane stays put. The `LANE` summary is that comparison.
4. To tell the two ranks apart, and name the part, give the program a rank
   rule and a layout file: see the next section.

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
ERR t=17 phase=minv-dn paddr=0x000c14794e78 exp=0xaaaaaaaaaaaaaaaa got=0xaaaaaabaaaaaaaaa xor=0x0000001000000000 nbits=1 lane=4 dq=36
LANE 4 (DQ32-39): 7 bit flips: DQ36=7
PADDR bits set in every error: 0x000c14794e78  clear in every error: 0x0003eb86b187  (7 errors with a known address)
TOTAL ERRORS: 7
```

`phase` names which step caught it, `xor` isolates the flipped bits and `nbits`
counts them. Only the first 300 error lines print; the total counts them all.

`dq` names the flipped data lines and `lane` the byte lanes they sit in. Each
64-bit word the test checks is one beat of the module's 64-bit data bus, so bit
*n* of `xor` is DQ*n*, and DQ*n* belongs to byte lane *n*/8. The `LANE` lines,
printed before the total, count flips per lane and per DQ over **every** error,
including the ones past the 300-line print limit.

A lane identifies a chip **position**, not yet a chip. On an x8 module each lane
is one chip per rank, so on a dual-rank (2Rx8) module a lane narrows the fault
to two chips: the one on the rank-0 side and the one on the rank-1 side. Which
reference designators those are, and which side is rank 0, is set by the
module's layout; x16 chips span two lanes each. On an ECC module the ninth
(check-bit) lane never reaches this program.

A single bit failing repeatedly at a fixed physical address, across patterns and
phases, is the signature of a defective cell. Bursty multi-bit errors scattered
over many addresses point at the controller, timing or a slot instead.

The `PADDR` line, printed whenever an error had a readable address, gives the
address bits that were set in every error and the ones clear in every error.
It is the input to calibrating a rank rule, below.

## Naming the chip: rank rules and layout files

A lane is a chip position. On a dual-rank module the same lane has a chip in
each rank, and which rank an access hit is decided by the memory controller
from the physical address, by a mapping this program cannot read. So the
mapping is supplied with `-r`, and the part names with `-l`:

```sh
physmem2 -r above:0x480000000 -l mymodule.layout 16 1000 6
```

```
ERR t=03 phase=minv-dn paddr=0x0004c1a2e078 ... lane=4 dq=36 rank=1 ic=U14
LANE 4 RANK 1 (DQ32-39) IC U14: 7 bit flips: DQ36=7
```

**Rank rules (`-r`).** At most four ranks.

| Rule | Rank is | Use when |
|---|---|---|
| `single` | always 0 | single-rank modules, to use a layout file |
| `above:A[,B,C]` | how many of the ascending boundaries the address is at or above | rank interleaving is off, so each rank is a contiguous block |
| `mask:M[,N]` | bit *i* is the parity of `paddr & mask i` | the rank is one address bit, or the XOR of several (the usual controller hash) |

When a rule is given but an address could not be read (not running as root),
the rank prints as `?` rather than a guess.

**Finding the rule for a bench.** The rule belongs to one board, CPU, firmware
and population; it has to be found once per test station, and again if any of
those change.

- *Easiest:* one module, one channel, rank interleaving turned off in firmware
  where it is offered. Rank 0 then fills the lower part of that module's range
  and rank 1 the upper. The boundary is not listed anywhere: work it out from
  the `System RAM` ranges in `/proc/iomem`, as the address where the module's
  second half begins, counting capacity across the hole below 4 GB that the
  PCI range occupies. Use it with `above:`.
- *Otherwise, calibrate:* run a module with a known-bad chip on a known side
  and note its `PADDR` line, then one with a known-bad chip on the other side.
  A bit set in every error from one side and clear in every error from the
  other is a rank-bit candidate: use `mask:` with it. Many controllers XOR
  several address bits into the rank, so often no single bit separates the
  sides; then the mask is a set of bits whose parity is even for every error
  from one side and odd for every error from the other, which takes the
  platform's documentation or some trial to find. Either way, confirm the rule
  against both known sides before trusting it, and collect a few hundred
  errors: a handful leaves many bits constant by chance.

**Layout file (`-l`, needs `-r`).** One line per chip position,
`rank lane designator`, `#` for comments. Designators come from the module's
own drawing: this program assumes no raw card. An x16 part appears on both of
its lanes. A (rank, lane) with no line prints `?`. See `layout.example`.

A layout file names parts only as well as the rank rule places them, and a
rule that is wrong for the bench will name the wrong side confidently. Check
it with a module whose bad chip is known before relying on it.

## Exit status

| Code | Meaning |
|---|---|
| 0 | Every thread ran and no error was observed. |
| 1 | Errors were observed. The count is on the last stdout line. |
| 2 | The test could not be completed, so the result is not a verdict. Either `/proc/self/pagemap` would not open, a `-r` or `-l` argument was invalid (nothing is tested), or a thread could not lock its memory; in the last case an `INCOMPLETE:` line on stderr says how many. |

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
  testing sticks in isolation, and identifying the rank needs a rule supplied
  with `-r` (see [Naming the chip](#naming-the-chip-rank-rules-and-layout-files)).

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
