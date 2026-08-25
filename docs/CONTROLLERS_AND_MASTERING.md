# Controller correctness and optional mastering

This milestone fixes the controller semantics exposed by `Hypernova.mid` and
adds an optional listening/mastering stage after the raw float mix. Neither
phase processing nor limiting is enabled by default, and the original coherent
SHA guard remains exact.

## Controller contract

The engine now implements:

- independent retained state for all 128 CC values on each of sixteen channels;
- CC7/39, CC10/42, and CC11/43 as 14-bit MSB/LSB pairs;
- CC101/100 RPN and CC99/98 NRPN selection with CC6/38 Data Entry;
- RPN 0 pitch-bend sensitivity, including immediate retuning of held voices;
- mid-note volume, pan, expression, pitch bend, and master-volume updates;
- CC120 All Sound Off as immediate channel silence, regardless of data byte;
- CC121 Reset All Controllers while preserving bank and program;
- CC123 through CC127 as sustain-aware All Notes Off behavior;
- retained SysEx payload offsets and Universal Real-Time Master Volume
  recognition (`7F device 04 01 ll mm`).

CC124 through CC127 perform the specified note-off implication. Offline SAFSYN
does not otherwise model omni/local-control or mono/poly routing state.
Unimplemented sound controls such as reverb and chorus sends are retained per
channel and available to later DSP stages; retaining a CC does not imply that
its corresponding effect processor exists yet. CC121 resets the addressed
channel's controller table, parameter selection, bend value, and bend range
while preserving bank and program.

Analysis always emits a per-channel controller histogram. A bounded trace of
any controller can
be requested with:

```text
safsyn-render input.mid --analyze \
  --controller-trace-start-seconds 0 \
  --controller-trace-seconds 131 \
  --controller-trace-cc 120 \
  --controller-trace-limit 1000
```

Each trace record includes stable tick/track/ordinal order, effective 14-bit
volume/pan/expression, sustain state, and active channel notes before and after
the event.

## Hypernova diagnosis

The full 48 kHz scan found:

- 2,707,574 CC7 events;
- 197 CC120 events across fourteen channels, using both data values 0 and 127;
- no CC11, CC39, CC42, CC43, or CC121 events;
- no SysEx events, hence no Universal Master Volume events.

It also found RPN 0 / Data Entry sequences assigning a twelve-semitone
pitch-bend range on all sixteen channels. Earlier renders ignored CC6/38 and
CC101/100 and therefore applied the former hard-coded two-semitone range. Those
renders compressed the intended pitch trajectories to one sixth of their
requested span; their audio measurements below are retained only as historical
pre-RPN baselines.

The lifetime-count failure was ignored CC120, not fine-resolution automation or
SysEx. At 113.352458 seconds, channel 7 has 39,279 active logical notes before a
CC120 and zero after it. At 113.389813 seconds, another CC120 removes 7,980 new
channel-7 notes. Ignoring those messages allowed voices hidden by CC7=0 to
survive and reappear when channel volume rose. The separate pitch-trajectory
failure was ignored RPN 0, and the weak ending was aggravated by an oldest-first
capacity tie-break that discarded long-held carriers.

Accounting for CC120 reduces the MIDI lifetime estimate from the earlier
102,321 logical notes to 55,088 (55,054 onset cohorts). Hypernova still exceeds
even the corrected 4,096-cohort listening cap by more than an order of
magnitude, so a capped render cannot be identical to unlimited polyphony.

## Optional mastering

Available flags:

```text
--output-gain-db N
--limiter
--limiter-ceiling-db N
--limiter-lookahead-ms N
--limiter-release-ms N
```

Output gain is applied first. The limiter uses a stereo-linked sliding maximum
over the current sample and its configured future window, immediate gain
reduction, and exponential release. Lookahead delays streamed writing only;
the final flush retains the exact input frame count. Reports contain separate
`raw_peak`/`raw_rms` and output `peak`/`rms` values plus lookahead, affected
frames, and maximum gain reduction.

This is a deterministic sample-peak limiter, not an oversampled true-peak
limiter. It intentionally lives outside `SynthEngine`; omit the mastering flags
to retain raw, unlimited float evidence.

## Historical pre-RPN Hypernova listening renders

Fixture and bank remain local and uncommitted:

```text
C:\Users\User\Downloads\Hypernova.mid
C:\Users\User\Downloads\sDetrimental Concert Grand Piano.sf2
```

Both full passes use exact cohorts with a deterministic 512-cohort ceiling, a
two-second tail, limiter ceiling -1 dBFS, 5 ms lookahead, 100 ms release, and no
pre-gain. Analytic uses continuous assignment and seed 7.

| Measurement | Coherent | Continuous analytic |
|---|---:|---:|
| Frames | 6,381,797 | 6,381,797 |
| Scheduled / channel events | 124,570,127 / 124,550,232 | same |
| Peak logical / physical cohorts | 869 / 512 | same |
| Capacity steals / logical voices stolen | 34,750,026 / 35,884,109 | same |
| Raw peak | 12.3604012 | 5.25209761 |
| Raw RMS | 1.21742912 | 0.335055884 |
| Limited peak | 0.891250908 (-1 dBFS) | 0.891250908 (-1 dBFS) |
| Limited RMS | 0.203041607 | 0.192789519 |
| Maximum limiter reduction | 22.8406513 dB | 15.4066558 dB |
| Limited/recovery frames | 5,081,671 | 5,085,411 |
| Phase preprocessing | 0 ms | 104,698.153 ms |
| Total render pass | 208.398 s | 312.122 s |
| File bytes | 51,054,420 | 51,054,420 |
| SHA-256 | `DDEE013FE1E2D434025AF579878E246B1874A182A54974D0F5FADBA7C69D570B` | `09D9DD189CC0A5C7915F852B9E9436FAA752638A218F0CBDDAD911ACDCDF2A42` |

The first coherent second peaks at only 0.705593407, so it triggers zero limiter
frames and remains unchanged. This proves the stage is not blanket
normalization: it leaves below-ceiling material alone and controls the later
bursts that otherwise force ordinary playback chains into hard clipping.

## Corrected RPN and 4,096-cohort Hypernova render

The recorded corrected evidence pass uses continuous analytic phase assignment
with seed 7, the MIDI's per-channel RPN 0 sensitivity of 12 semitones, a
4,096-cohort ceiling, a two-second tail, and the same -1 dBFS limiter settings.
It predates the later channel-scoped reserve policy and used a global victim
search that preferred release-stage cohorts and then the smallest estimated
audible contribution. The artifact remains useful as the controller/RPN
baseline, but it is not evidence for the current allocator.

| Measurement | Corrected result |
|---|---:|
| Frames / bytes | 6,381,797 / 51,054,420 |
| Peak physical cohorts | 4,096 |
| Logical region voices started | 44,750,713 |
| Cohort-capacity steals | 36,115,807 |
| Raw peak / RMS | 7.18318653 / 0.456786126 |
| Limited peak / RMS | 0.891250908 / 0.231781332 |
| Limited frames / maximum reduction | 5,311,614 / 18.1263429 dB |
| Phase failures | 0 |
| Phase preprocessing / total render | 50.146 s / 894.773 s |
| SHA-256 | `8F4DA04123E62EFDDE2196A97FE0F9296EABA406182D5782AA2784F7D8A0B7FC` |

Against the earlier 4,096-cohort pre-RPN render, whole-file limited RMS rises
from 0.216448 to 0.231781. Mean per-second RMS over seconds 123 through 130
rises by approximately 9.7%; the last sounding bins improve by approximately
29% at second 127, 50% at second 129, and 62% at second 130. The MIDI then has
no active notes at second 129, so the remaining decay is expected tail rather
than a voice-selection failure. Most importantly, honoring the requested
12-semitone sensitivity restores six times the pitch span of the former
hard-coded two-semitone interpretation.

## Verification

Seven CTests pass in MSVC Release. Coverage includes independent retained state
for arbitrary CCs on all channels, RPN/NRPN selection, fractional RPN 0 pitch
sensitivity and held-voice retuning, active-note 14-bit controller changes,
both CC120 data-byte forms, sustain-aware CC123-127, CC121 reset,
audibility-prioritized individual/cohort stealing, individual/cohort
equivalence, retained and dispatched Universal Master Volume, mastering
identity/gain, linked stereo ceiling, exact lookahead, release, block
invariance, streamed raw/output metrics, unchanged frame count, and the
pre-existing coherent SHA-256 guard.
