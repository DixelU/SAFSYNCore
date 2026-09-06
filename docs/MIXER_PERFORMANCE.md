# Mixer SIMD and worker handoff

The September 2026 optimization pass adds an SSE2 kernel for four consecutive
frames of a coherent, multiplicity-one cohort in its sustain stage. Mono,
interleaved stereo, and linked planar stereo are supported. Sequential double
position updates, interpolation rounding, gain multiplication order, and cohort
accumulation order are preserved. Loop/sample boundaries, changing envelopes,
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

## Next targets

1. **SIMD across cohorts for short intervals and analytic phase.** The current
   frame kernel cannot vectorize one-frame intervals or changing envelopes.
   Separate hot position/envelope/gain/sample fields from logical identity,
   phase vectors, and intrusive list metadata, then benchmark batches of compatible
   cohorts. Gather traffic and stable summation order need explicit measurement
   before choosing AVX2 or wider kernels.
2. **Choose useful worker counts per workload.** Four workers outperform sixteen
   in the measured coherent nine-frame case; sixteen win on long blocks. The
   current threshold uses active cohort count times frames, while partitioning
   still divides allocated slot ranges. Sparse/skewed pools and stage-dependent
   costs need profiling before changing lane assignment and its rounding order.
3. **Producer wakeups and telemetry.** `BufferedSynth` polls a full audio ring
   with a one-millisecond timed wait, and `read_audio` does not signal newly
   available space. A bounded notification protocol could reduce refill delay.
   Measure underruns and delivery latency with WASAPI; mixer throughput alone
   cannot establish this benefit. The existing MIDI queue and audio ring already
   use atomics with separated producer/consumer indices.

A real soundfont/MIDI pass should precede further default thread-policy changes:
event dispatch, release-heavy passages, phase-cache bandwidth, sparse pools, and
audio delivery are not represented by these sustained-cohort measurements.
