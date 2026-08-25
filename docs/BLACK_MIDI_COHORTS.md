# Logical voices and exact render cohorts

This milestone separates offline MIDI identity from physical sample rendering.
It does not optimize the phase cache, add multithreading or SIMD, add filters,
convolution, or change the coherent default. Optional post-mix mastering was
added in the subsequent controller/mastering milestone.

## Representation

The original `SynthEngine` individual-voice model remains available as the
reference path. Standard MIDI File rendering selects the cohort model by
default; `--individual-voices --voices N` explicitly selects the old fixed
path.

A logical batch stores a contiguous deterministic event-serial range, channel,
note, velocity, onset frame, held/sustain counts, and stable handles to the
sample-region cohorts it triggered. Per-channel/note stacks preserve newest
note/LIFO release semantics without scanning all physical voices. Sustain
keeps a compact suffix count and releases that suffix when the pedal lifts.

A physical cohort stores one exact playback position, increment, loop state,
AHDSR state, gain state, logical multiplicity, and aggregate phase terms.
Cohorts merge only when region, channel, note, velocity, onset frame, and
channel/preset-dependent state are exact. Rendering clears onset candidates;
controller, program, and pitch changes invalidate candidates on their channel.
Staggered voices are never merged merely because their positions later happen
to resemble each other.

Stable slot handles plus generation counters protect logical links from slot
reuse. Physical allocation uses a free list and maintained active counters.
Offline cohort growth has no implicit limit. `--max-cohorts N` installs an
explicit safety ceiling and enables deterministic cohort stealing; a zero
ceiling means dynamic growth without stealing.

## Phase aggregation

Coherent cohorts multiply the source by logical multiplicity. Random polarity
cohorts retain the signed polarity sum. Analytic cohorts retain separate left
and right sums of `cos(theta) * RMS_scale` and `sin(theta) * RMS_scale`, so each
logical serial keeps its own finite-pool or continuous deterministic angle.
Linked stereo channels retain separate RMS scales while sharing the event
angle.

Attack protection is applied to the aggregate identity:

- before the protected attack ends, the output is `N` coherent copies;
- during the transition, it interpolates from `N` coherent copies to the
  accumulated analytic representation;
- after the transition, it uses the accumulated coefficients.

Note-off reconstructs the deterministic contribution without incrementing the
phase-assignment counter, subtracts it from the sustaining cohort, and clones
the exact playback/envelope state into a release cohort. Compatible releases
at the same frame join one release cohort.

Multiplicity-one cohorts call the original `PhaseVoiceState::apply` operation.
Therefore fixtures that do not merge retain exact arithmetic and hashes.
Grouped summation deliberately permits strict floating tolerance because its
addition order differs from individual voices.

## Analysis metrics

SMF analysis now reports compatible note-on and note-off groups per output
sample, exact group histograms, group locations, a MIDI-level logical-note
peak, a conservative same-onset cohort peak, and an optimistic onset
compression bound.
The grouping key includes channel, note, note-on velocity, and a per-channel
semantic epoch that changes on controller/program/pitch traffic.

Analysis does not load a soundfont. Its logical and cohort estimates are MIDI
note estimates, not a claim about how many selected SF2 regions each note will
trigger. Render statistics provide the exact post-preset region-voice and
physical-cohort counts.

## Hypernova result

Fixture: `C:\Users\User\Downloads\Hypernova.mid`, 48 kHz. The file remains
local and is not committed.

| Measurement | Result |
|---|---:|
| Note-ons | 44,750,713 |
| Compatible note-on groups | 29,784,208 |
| Overall onset compression | 1.502498x |
| Single-note onset groups | 24,076,250 |
| Largest compatible note-on group | 38 at frame 5,817,394 |
| Compatible note-off groups | 29,437,779 |
| Largest compatible note-off group | 317 at frame 5,802,681 |
| Estimated peak active logical MIDI notes | 55,088 |
| Conservative peak same-onset cohorts | 55,054 |
| Maximum events at one sample | 51,911 at frame 5,802,711 |
| Supplemental analysis time | approximately 11.8 s |
| Parser/heap state | 3,432 bytes |

The maximum event spike occurs at approximately 120.890 seconds and is mainly
note-off traffic. It is not an enormous identical-note onset. Controller-aware
lifetimes, especially CC120, reduce the earlier estimate substantially. Even
then, compatible duplicates almost never overlap at the active peak: 55,054
estimated cohorts versus 55,088 logical notes.

The first-second coherent and continuous-analytic renders each start 210
logical region voices, peak at 210 cohorts, merge zero voices, and steal zero.
They exactly reproduce the pre-cohort artifacts:

| Mode | Peak | RMS | SHA-256 |
|---|---:|---:|---|
| coherent | 0.711105943 | 0.113753696 | `66633701755B83A64BB5F615FDFA00B2ABAA8260C985B0FEE27C8785EEE0831E` |
| analytic continuous, seed 7 | 0.759180307 | 0.122729830 | `EF83224556EA7607C3AE9DC1A089F694121E009FF26976A8B113FB607B709CD4` |

The analytic excerpt retains 63 cached samples and 825,901,056 phase-cache
bytes. Timing varies with the host run; phase preprocessing remains the known
dominant analytic cost.

A paired safety-ceiling render was then bounded at frame 5,803,200 (120.9
seconds), just after the maximum event spike. Both modes deliberately used
`--max-cohorts 512`, so it validates dense dispatch and deterministic cohort
stealing rather than unlimited-polyphony sound quality:

| Measurement | Result |
|---|---:|
| Scheduled / channel events | 109,650,968 / 109,648,984 |
| Logical region voices started | 42,213,387 |
| Logical voices merged | 2,851,344 |
| Realized onset compression | 1.07243892x |
| Peak represented logical voices / cohorts | 869 / 512 |
| Maximum cohort multiplicity | 13 |
| Cohort-capacity steals | 34,801,834 |
| Logical voices removed by stealing | 35,888,498 |

All counts above are identical between coherent and continuous analytic. Their
audio/cache measurements are:

| Measurement | Coherent | Continuous analytic, seed 7 |
|---|---:|---:|
| Peak | 12.383873 | 5.14215946 |
| RMS | 1.16408053 | 0.327656441 |
| Phase preprocessing | 0 s | 94.79 s |
| Phase cache | 0 bytes | 1,143,619,584 bytes / 87 samples |
| Render-audio time | 22.43 s | 34.93 s |
| Total render time | 166.63 s | 277.18 s |
| WAV SHA-256 | `F328A49FFF8AD1D5A53CE897D3BD39D3EA13AB640DDB74D20374DF128911094A` | `1E68977F61330998CBC961F39958B686A13EDB27A025C38559D4466BD6E65EC1` |

Analytic reduces peak by 58.48% (`-7.63 dB`) and RMS by 71.85%
(`-11.01 dB`) in this capped dense render. It remains decorrelation evidence,
not limiting evidence; both WAVs retain raw float output.

The realized compression is lower than the MIDI grouping bound because stable
ordering, already-released candidates, preset-region behavior, and capacity
churn constrain which candidate cohorts still exist when later events at the
same sample arrive. Most steals remain.

An unlimited full cohort render was not dispatched. The corrected analysis
predicts an active physical peak near 55k rather than 512, so this design would increase
per-sample rendering work by roughly two orders of magnitude while eliminating
steals. The densest validation window is near the end of the file and requires
about 121 seconds of faithful warm-up. Running it would not be a practical
sound-quality comparison for this representation.

The result rejects the original central hypothesis for Hypernova: exact
same-onset cohorts do not eliminate most steals. They remain valuable for
duplicate bursts and establish the logical/physical separation needed by a
later event-domain or convolution-style renderer, but that more radical design
is outside this milestone.

## Tail drain and validation

`--drain-tail --max-tail-seconds N` releases sustain and held notes at natural
EOF, renders one frame at a time until all represented logical voices are
inactive, and reports whether the ceiling was reached. The fixed
`--tail-seconds N` behavior remains available.

Tests compare cohorts with a generously sized individual reference across
thousands of coherent, polarity, finite-analytic, and continuous-analytic
duplicates; interleaved onset runs; stereo and linked-planar stereo samples;
attack/decay note-offs; sustain; pitch bend and controllers; block sizes;
deterministic seeds; dynamic growth; deterministic safety stealing; and
one-shot playback. A synthetic 51,911-note same-sample fixture collapses to one
onset cohort without stealing. Existing coherent SHA tests remain exact.

Controller-correct full renders and optional limiter measurements supersede the
listening artifacts above; see `CONTROLLERS_AND_MASTERING.md`.
