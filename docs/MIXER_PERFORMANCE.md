# Mixer SIMD and worker handoff

The September 2026 optimization pass adds an SSE2 kernel for four consecutive
frames of a coherent, multiplicity-one cohort in its sustain stage. Mono,
interleaved stereo, and linked planar stereo are supported. Sequential double
position updates, interpolation rounding, gain multiplication order, and cohort
accumulation order are preserved. The envelope is multiplied before region and
channel gains, matching the current scalar DSP. Active filters and pending
filter/bypass ramps use the scalar kernel to retain their recursive histories.
Loop/sample boundaries, changing envelopes,
grouped cohorts, ping-pong loops, and experimental phase modes use the general
kernel. One-frame calls have a separate specialization without SIMD setup or
an inner frame loop. Builds without SSE2 retain scalar rendering.

Persistent workers now use cache-line-separated atomic request/completion
generations. A release request publishes immutable job data; an acquire
completion makes each lane's samples and retirement list visible to the owner.
Workers briefly spin (256 pause iterations on x86), then use C++20 atomic wait.
Idle workers park, and shutdown publishes a reserved stop generation before
joining. There is no shared mutex or completion-counter update in this handoff.
Each lane clears its own audio buffer. Fixed lane-order reduction and serial
retirement are retained.

## Measurements

Host: AMD Ryzen 9 7900X, 12 cores / 24 logical processors, Windows x64,
MSVC 19.51.36256.0, Release `/O2 /Ob2 /DNDEBUG`, default precise floating point.
Baseline: `a2bb5356d9688f72ed811afb1db5bc926cadb432`, with the same new benchmark
harness linked against the original engine. No AVX requirement or fast-math
flags were added.

Each case uses 4,096 distinct sustained cohorts, a synthetic 1,024-frame looping
sample, two warm-up passes, and the median of 15 timed 1,024-output-frame passes.
An interval of nine means repeated nine-frame calls plus a final shorter call.
Phase preprocessing and note dispatch are outside the timed section.

| Mode | Threads | Interval | Before (ms) | After (ms) | Speedup |
|---|---:|---:|---:|---:|---:|
| Coherent mono | 1 | 1 | 29.013 | 28.237 | 1.03x |
| Coherent mono | 1 | 9 | 21.690 | 11.363 | 1.91x |
| Coherent mono | 1 | 1,024 | 20.835 | 7.741 | 2.69x |
| Coherent stereo | 1 | 1,024 | 25.004 | 10.474 | 2.39x |
| Coherent mono | 4 | 9 | 8.346 | 3.055 | 2.73x |
| Coherent stereo | 4 | 9 | 8.989 | 3.924 | 2.29x |
| Coherent mono | 4 | 1,024 | 5.877 | 2.043 | 2.88x |
| Coherent stereo | 4 | 1,024 | 6.763 | 2.859 | 2.37x |
| Coherent mono | 16 | 9 | 8.557 | 6.022 | 1.42x |
| Coherent mono | 16 | 1,024 | 2.505 | 0.909 | 2.76x |
| Analytic mono | 4 | 9 | 11.130 | 11.668 | 0.95x |
| Analytic mono | 4 | 1,024 | 8.548 | 8.527 | 1.00x |

These are mixer microbenchmarks, not full-song or device-latency measurements.
The coherent results combine SIMD and handoff changes; they do not isolate a
synchronization-only speedup. Analytic performance is essentially unchanged on
long blocks, with a roughly 5% slower four-thread nine-frame median in this run.
Worker scheduling varies between runs. The single-frame 4,096-cohort cases
remain below the existing parallel-work threshold and run on the caller.

The full before/after matrix, including minimum/maximum times and checksums,
is in `mixer_performance_2026-09-06.csv`. All reported benchmark checksums match
the corresponding baseline. Checksums are a performance-harness sanity check;
exactness is established separately by tests.

## Validation and reproduction

All eight Release CTest suites pass, including the unchanged coherent WAV
SHA-256. New fixtures compare SIMD output bit-for-bit with individual voices
and one-frame cohort rendering across mono/stereo layouts, fractional pitch,
controllers, high playback increments, loop/envelope transitions, and odd tails.
Worker fixtures exercise 2/5/16 threads, repeated generations, scalar fallback,
buffer growth, parking/wakeup, phase modes, retirement, and pool replacement.
The cohort and playback suites also pass under MSVC AddressSanitizer, with the
matching ASan runtime staged beside the test binaries to resolve a CTest loader
failure. This checks memory access, not data races; no ThreadSanitizer run is
claimed.

From the parent SAFC checkout:

```text
cmake -S SYNCore -B build/syncore-perf -DSAFSYN_BUILD_BENCHMARKS=ON
cmake --build build/syncore-perf --config Release
ctest --test-dir build/syncore-perf -C Release --output-on-failure
build/syncore-perf/Release/safsyn-engine-benchmark --matrix 4 15 4096
```

The positional matrix arguments are threads, repetitions, and cohort capacity.
Repeat with 1 and 16 threads to compare scaling. Preserve the same compiler,
flags, harness, and workload when comparing engine revisions.

## Analytic and filtered kernels (September 28, 2026)

The random-polarity, smooth-field, and independent-bins phase modes were
retired, leaving direct sampling (coherent) and analytic rotation. The
four-frame SSE2 kernel was then generalized. It now covers:

- every envelope stage. Attack, hold, and decay advance the scalar envelope
  once per frame. Release multiplies in frame order and falls back before the
  frame that would end the note, so retirement timing is unchanged.
- four source forms, each evaluating its scalar reference exactly:
  a single coherent voice, a coherent group (`float(x * count)` in double), a
  single analytic voice (`PhaseVoiceState::apply` in float), and an analytic
  group (`PhaseAggregate::sample` in double).
- one 64-bit load per frame for interleaved stereo PCM and for each quadrature
  endpoint pair.

Filtered cohorts have a separate per-frame kernel: the original and quadrature
bases for both channels share one vector through the unchanged
`StereoFilterState` operation order. Coefficient ramps advance per frame, and a
ramp that finishes at bypass hands back to the unfiltered path. Without a
protected attack, the protected basis only ever filters zeros and the changed
basis equals the original basis. Both are skipped in scalar and SIMD code. A
protected attack, ping-pong loops, and loop/sample boundaries still use the
general kernel.

Analytic preparation keeps every cached quadrature bit-identical while
precomputing radix-2 twiddle recurrences once per stage and running stages that
fit in a 16K-point block depth-first. It also shares one Bluestein chirp and
chirp spectrum across a loop body's forward and inverse transforms and both
stereo channels, and transforms unique samples on up to eight threads.

### Measurements

Host: Intel Core i5-13600K (6P+8E cores, 20 logical processors), Windows 11,
MSVC 19.51, Release `/O2`, no AVX or fast-math flags. The median of seven
1,024-frame passes over 4,096 cohorts is reported. The same benchmark source was
compiled against the previous library for "before". All 90 checksums match.
`analytic_performance_2026-09-28.csv` holds the full matrix. The new
cases use eight onsets per cohort (`grouped`), a ten-second release in progress
(`release`), and a region low-pass (`analytic-filtered`).

| Case | Threads | Before (ms) | After (ms) | Speedup |
|---|---:|---:|---:|---:|
| analytic mono | 1 | 35.836 | 7.749 | 4.62x |
| analytic stereo | 1 | 50.732 | 10.181 | 4.98x |
| analytic grouped | 1 | 41.831 | 11.505 | 3.64x |
| analytic release | 1 | 35.873 | 8.298 | 4.32x |
| analytic filtered | 1 | 94.351 | 23.849 | 3.96x |
| coherent grouped | 1 | 25.772 | 8.728 | 2.95x |
| coherent release | 1 | 26.016 | 7.044 | 3.69x |
| coherent stereo | 1 | 8.794 | 7.480 | 1.18x |
| analytic mono | 4 | 9.061 | 1.947 | 4.65x |
| analytic stereo | 4 | 12.725 | 2.529 | 5.03x |
| analytic filtered | 4 | 23.570 | 5.966 | 3.95x |

These rows use 1,024-frame intervals. Nine-frame intervals gain 2.2-3.5x on
one thread for the analytic, grouped, and release cases. One-frame intervals
never enter a frame kernel and change by at most about 15%. The exception is
filtered analytic, 1.46x faster, which comes from skipping the redundant
filter bases in scalar code.
Four-thread nine-frame medians varied by 2-3x between repeated runs of *both*
builds on this hybrid CPU, so those rows are scheduling noise rather than a
comparison.

Analytic preparation of a synthetic 48-sample bank (60k-400k frames, one third
each mono/interleaved/planar, half looped with non-power-of-two bodies, 64 MiB
of cache) took 7.69 s before, 5.03 s serial after, and 2.26 s with automatic
threads. A render depending on every cached quadrature hashed identically in
all three runs.

### Validation

All eight CTest suites pass, including the unchanged coherent SHA-256. New
fixtures compare one-frame (always scalar) cohort rendering with 4-, 9-, and
64-frame blocks bit-for-bit. They cover analytic singles and groups, all three
sample layouts, all loop modes, filters with automation and bypass ramps,
protected attacks, splits, and releases. Parallel and serial preparation must
produce identical audio, with progress reported on the calling thread only.

An out-of-tree A/B harness rendered 20 randomized three-second scenarios
(individual and cohort models, 1-4 threads, 1-4096-frame blocks, filters,
batches, protected attacks, and prepared or lazy caches) against the previous
library. Every output hash matched. Perturbing each of the six SIMD source
forms by one ulp changed exactly the scenarios that exercise it, which confirms
the kernels run. The analytic-group and filtered-analytic perturbations also
fail the new cohort fixtures, so the suite itself checks exactness.

## Chunked handoff and note dispatch (October 2, 2026)

`safsyn-live-profile` replays a real MIDI file through the engine the way the
live producer does (events dispatched at block boundaries, one render call per
256-frame block) and times dispatch and mixing separately. With an 867 MB piano
SF2, 8,192 cohorts and analytic phase it showed two limits that the
sustained-cohort microbenchmarks above cannot:

- Each lane owned a fixed range of cohort slots. Occupied slots and expensive
  cohorts are not spread evenly, so every block waited for its slowest lane
  while the others were parked: 16 threads mixed live material only 7.6 times
  faster than one.
- In dense passages the owner's serial note dispatch, not the mixer, sets the
  block time. At 2.4 million events per second it cost 0.69 s per second of
  audio against 0.31 s of mixing.

### Mixer

The slot range is now cut into up to eight chunks per thread (at least 16 slots
each). Every participant pulls the next chunk from one atomic cursor, mixes it
into that chunk's own buffer, and the owner adds the buffers in chunk order.
A thread that is preempted, on a slower core, or holding costlier cohorts
delays a block by one chunk at most. The audio depends on the chunk layout
(thread count, slot count and block length), never on which thread rendered
which chunk, so fixed-thread renders stay deterministic. Their last bits differ
from the earlier lane-order reduction; one thread keeps the scalar reference
order and its hashes. Retirements are still applied by the owner in ascending
slot order.

Each lane has one atomic word holding an epoch and a phase. The owner requests,
the worker claims and completes, and the owner withdraws any request that its
worker has not claimed once the chunks have run out, so a worker that wakes late
costs nothing. A block wakes one participant per 8,192 cohort frames, which
replaces the per-workload worker count from the earlier target list: the
nine-frame, 16-thread coherent case above drops from 4.76 to 3.22 ms. Summing
the chunk buffers takes about 10 microseconds per 256-frame block.

Automatic live thread selection now uses every logical CPU but two (at most 64)
instead of stopping at 16.

### Dispatch

Three changes remove repeated work from each note without changing a sample:

- The regions a preset plays for a key and velocity, with their pitch
  increments, are filtered once and reused until the bank is replaced.
- A pool variant's rotation (cosine, sine and channel scales) is computed once
  per sample, and a region index resolves its cache entry without hashing.
- A recycled logical batch keeps its link storage instead of allocating per
  note, and voice stealing estimates each probed cohort's level once.

### Measurements

Host: AMD Ryzen 9 7900X, 12 cores / 24 logical processors, MSVC 19.51, Release
`/O2`. Mixing seconds for 16 s of audio (coherent, up to 4,380 live cohorts):

| Threads | Before (s) | After (s) | Scaling before | Scaling after |
|---:|---:|---:|---:|---:|
| 1 | 9.78 | 9.87 | 1.0x | 1.0x |
| 2 | 5.61 | 5.08 | 1.7x | 1.9x |
| 4 | 3.25 | 2.66 | 3.0x | 3.7x |
| 8 | 1.85 | 1.40 | 5.3x | 7.1x |
| 12 | 1.27 | 1.04 | 7.7x | 9.5x |
| 16 | 1.28 | 1.00 | 7.6x | 9.9x |
| 22 | - | 0.93 | - | 10.6x |

Whole pipeline, analytic phase, 8,192 cohorts, automatic threads (16 before,
22 after). Late blocks took longer than their own 5.33 ms; the realtime factor
is audio duration over dispatch plus mixing time.

| Material | Dispatch (s) | Mixing (s) | Realtime factor | Late blocks |
|---|---:|---:|---:|---:|
| 18M-note file, 30 s at 250k events/s, before | 3.11 | 8.90 | 2.50 | 3 of 5,625 |
| after | 2.44 | 6.62 | 3.31 | 0 |
| Hypernova, 8 s at 2.4M events/s, before | 5.52 | 2.45 | 1.00 | 281 of 1,500 |
| after | 3.79 | 1.85 | 1.42 | 168 |
| Hypernova, 2.5 s at 10M events/s, before | 2.62 | 0.78 | 0.74 | 446 of 469 |
| after | 1.67 | 0.60 | 1.10 | 83 |
| Hypernova, whole file (131 s, 125M events), before | 26.59 | 24.77 | 2.55 | 1,567 of 24,555 |
| after | 18.29 | 17.72 | 3.64 | 515 |

`--live` runs the real `BufferedSynth` with a wall-clock sender and a 10 ms
audio consumer. Hypernova from 95 to 125 s (104M events, 4,096-frame ring,
automatic threads) had 7.54 s of underrun silence in 1,134 gaps before and
0.55 s in 110 gaps after.

### Validation

Single-thread renders of three references (40 s coherent and analytic at 8,192
cohorts, 70 s analytic at 512 cohorts with one million steals) hash identically
before and after. New fixtures cover remembered layers across program, bank and
stress-mode changes and a replaced bank, reused pool rotations against a first
computation, and 64 mixer threads on fewer processors.

```text
build/syncore-perf/Release/safsyn-live-profile bank.sf2 file.mid --analytic
    --cohorts 8192 --threads 16,22 --start 103 --duration 8
build/syncore-perf/Release/safsyn-live-profile bank.sf2 file.mid --analytic
    --cohorts 8192 --threads 0 --start 95 --duration 30 --live
```

## Next targets

1. **SIMD across cohorts for short intervals.** Frame kernels cannot help
   one-frame intervals, which still cost about 30-70 ms per 1,024 frames here.
   Separate hot position/envelope/gain/sample fields from logical identity,
   phase aggregates, and intrusive list metadata, then benchmark batches of
   compatible cohorts. Gather traffic and stable summation order need explicit
   measurement before choosing AVX2 or wider kernels.
2. **Analytic preparation arithmetic.** A real-input or radix-4 FFT would cut
   preparation further, but changes cached quadratures in their last bits
   and needs new reference hashes for analytic renders.
3. **Serial note dispatch.** After the October pass dispatch is half of all work
   on dense files and two thirds of it in their densest passages, on one thread
   at roughly 70-200 ns per event. The remaining cost is structural: stealing
   probes up to 32 cohorts that render workers wrote last, a note-off copies
   its cohort into a release cohort, and both walk shared lists. Going further
   needs a layout that keeps steal-relevant fields with the owner, or a
   dispatch that is partitioned rather than only cheaper.
4. **Producer wakeups and telemetry.** `BufferedSynth` polls a full audio ring
   with a one-millisecond timed wait, and `read_audio` does not signal newly
   available space. A bounded notification protocol could reduce refill delay.
   Measure underruns and delivery latency with WASAPI; mixer throughput alone
   cannot establish this benefit. The existing MIDI queue and audio ring already
   use atomics with separated producer/consumer indices.

The sustained-cohort measurements do not represent event dispatch,
release-heavy passages, phase-cache bandwidth, or sparse pools. Check thread
policy and dispatch changes with `safsyn-live-profile` on a real bank and file,
and audio delivery with WASAPI.
