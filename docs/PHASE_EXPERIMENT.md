# Deterministic phase-decorrelation experiment

This milestone adds opt-in phase processing before Standard MIDI File parsing.
It does not select a new playback default: `coherent` remains the default and
its existing piano WAV stays bit-identical.

## Modes and controls

- `coherent`: original PCM path, with no phase cache.
- `polarity`: deterministic event polarity. Strength zero bypasses it; any
  nonzero strength enables the binary polarity experiment.
- `analytic`: `x cos(theta) - H{x} sin(theta)`, using either a deterministic
  finite pool or a continuous event-derived angle.
- `smooth-field`: FFT phase displacement with linearly interpolated random
  anchors. `--phase-correlation-hz` controls the approximate anchor spacing.
- `independent-bins`: an independent deterministic phase displacement per FFT
  bin.

For the analytic and FFT methods, `--phase-strength S` bounds displacement to
`[-pi*S, +pi*S]`. FFT processing leaves DC and Nyquist unchanged, applies the
same phase sheet to linked stereo channels, and RMS-matches each transformed
channel before optional attack blending. `--phase-preserve-attack-ms N` keeps
the first N milliseconds exact and then applies a 10 ms smoothstep transition.

Finite pools use deterministic indices in a 1, 8, 32, or 64-entry space.
Analytic mode caches one quadrature signal per logical sample and derives its
angles without storing rendered copies. FFT variants are materialized lazily.
Both are keyed by logical sample identity and all transform settings.

The event identity is independent of voice slots: configured seed, event
serial, channel, note, and logical region determine the assignment. Therefore
voice stealing cannot reassign surviving voices. Quadrature construction, FFT,
variant allocation, and cache insertion happen during note dispatch, never in
`render_audio`.

Looping samples have an explicitly periodic transformed loop body, plus a
bounded entry crossfade. The implementation does not merely wrap the end of an
arbitrary transformed whole-sample buffer.

## Metric definitions

- `peak`: maximum absolute stereo sample. Output is not normalized or clipped.
- `rms`: RMS over both output channels.
- `periodicity`: normalized mono autocorrelation at exactly one dispatch
  interval. This is a periodic-envelope proxy, not mean inter-voice coherence.
- `dispatch_db`: amplitude of the direct mono sinusoidal coefficient at the
  dispatch frequency, relative to `mono_rms * sqrt(2)`. It is not a
  strongest-bin ratio or a sum of harmonics.
- `preprocessing_ms`: cumulative phase-cache construction measured outside
  `render_audio`.
- `render_ms`: wall time spent inside `render_audio` calls only.
- `phase_cache_bytes`: phase-owned cached float storage, excluding source PCM
  and container overhead.

Timing and memory figures are diagnostic measurements from one Release run,
not performance gates.

## Real-piano matrix

Fixture: `sDetrimental Concert Grand Piano.sf2`, bank 0/program 0, the existing
four-second 48 kHz chord script, 512 voices, strength 1, seed 7, and 250 Hz
smooth-field correlation. Every row started 9 voices, peaked at 9 active
voices, and reported zero preprocessing failures.

| Mode | Peak | RMS | Periodicity | Dispatch dB | Preprocess ms | Render ms | Cache bytes | Variants | WAV SHA-256 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| coherent | 0.390678585 | 0.068718166 | 0.233102611 | -40.7475 | 0.0 | 4.76 | 0 | 0 | `1A79343D659CD744970FB57AA5C1E6CD7E859679A0E8C3CF0C43E89DB285983D` |
| analytic pool 1 | 0.418326080 | 0.068657035 | 0.236172369 | -57.8355 | 7994.5 | 10.65 | 91,406,336 | 7 | `1FBBB391E9907B027F17C3A0B6C1BC664C7C764A7B47FAA5D677AB700DE92BD0` |
| analytic pool 32 | 0.409843713 | 0.067873947 | -0.091285519 | -51.0397 | 7409.1 | 10.29 | 91,406,336 | 9 | `CD07D0F257CABE0728CC59DF07CD62815053C087F1D74683527D64F5E9FF217D` |
| analytic pool 64 | 0.407314539 | 0.067670476 | -0.058513044 | -49.6797 | 7394.4 | 10.26 | 91,406,336 | 9 | `A12D50CF0D81E506A8E246E2E993B91AFC53A3554B3F5B26281AC8D913A95751` |
| analytic continuous | 0.429725707 | 0.068065302 | 0.026939406 | -50.6144 | 7448.3 | 10.56 | 91,406,336 | 0 | `AF7B78C0F56A5C3BACF2B34FFF12E5667023FAE88244A5E554406FA9A7865FB8` |
| smooth field pool 64 | 0.425961614 | 0.068176671 | -0.031159402 | -57.5708 | 30435.2 | 11.15 | 117,522,432 | 9 | `06426D9E9909E656EBE28FA524D726D2BAFA9B4D74EEA1C4D2AEFC6182D41DA5` |
| independent bins pool 64 | 0.060656250 | 0.012787701 | 0.048321893 | -46.4110 | 31580.8 | 11.17 | 117,522,432 | 9 | `316ED545F635EA68C3A8677B7DE13A336B3F23972D736AB67C396B0CEAC9818E` |

Each independent-bin variant is RMS-matched to its source before mixing. The
lower final RMS in that row is inter-voice cancellation, not normalization.

An analytic continuous render with a 50 ms protected attack produced peak
`0.424358457`, RMS `0.067751374`, and SHA-256
`C2995E743710C351807EFA47D30AD4CA827B18CCBC56E4CB2913B1CAE3A05BDC`.
Analytic mode at strength zero and seed 999 reproduced the coherent hash exactly
with zero preprocessing and zero phase-cache bytes.

## Controlled repeated-trigger matrix

Fixture: the built-in one-second 440 Hz mono sine, triggered at 40 Hz 128 times
at note 69, 48 kHz, 256 voices, strength 1, seed 7. Forty voices overlap at the
maximum. All rows started 128 voices, peaked at 40 active voices, rendered the
same 212,400 frames, and reported zero preprocessing failures.

| Mode | Peak | RMS | Periodicity | Dispatch dB | Preprocess ms | Render ms | Cache bytes | Variants | WAV SHA-256 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| coherent | 7.08492804 | 4.00548860 | 0.999784811 | -64.0329 | 0.0 | 25.00 | 0 | 0 | `D0C7B4903E7D3401E405C331248C80EA0DF9033812DF6CFF3C1C64383E5E7F26` |
| analytic pool 1 | 7.09201384 | 4.00513642 | 0.999784731 | -61.0558 | 6.29 | 41.29 | 192,000 | 1 | `DE440D53EE90E5DE847CE55F26616FDC7B1BC3DE0B31CE54692182FE79F84E8F` |
| analytic pool 32 | 2.76523042 | 0.99458290 | 0.986288802 | -70.6040 | 6.40 | 41.96 | 192,000 | 31 | `0B6E3E6CD9201C107EF657FEE9269EB23578BC93B660E16D0D362FE44EBACECE` |
| analytic pool 64 | 2.29473162 | 0.73030364 | 0.972433311 | -68.7659 | 6.35 | 41.99 | 192,000 | 58 | `648327A37326544FE1F43FBCE91AE593915ED9F15127BFCD05EA99535F186A83` |
| analytic continuous | 1.66642094 | 0.55803105 | 0.954459812 | -86.4635 | 6.80 | 41.51 | 192,000 | 0 | `26E981B3BED68A356128C8A71FAF37352EBC53035083BFC6D1444136D9451CAA` |
| smooth field pool 64 | 3.78910542 | 1.87658316 | 0.996323504 | -65.1183 | 1370.4 | 41.65 | 11,136,000 | 58 | `92C38CD611D85190D978848EEE286E116CA13940C0E3849F5DC97C72439861F9` |
| independent bins pool 64 | 2.30795979 | 0.85709567 | 0.981308494 | -75.5827 | 1379.5 | 42.37 | 11,136,000 | 58 | `40D9C6DB59A03DC0F7074E0C873AC72E1D9440FDD0930F470B31EF1E84A62FD4` |

Pool 1 is the required transformed-but-mutually-coherent control. The finite
pool progression demonstrates reduced coherent build-up, and continuous
analytic rotation gives the lowest peak and RMS on this fixture. The remaining
high autocorrelation is expected: phase diversity does not remove the periodic
40 Hz event envelope.

## Evidence boundary and next decision

The matrix and automated tests were run in Release with MSVC 19.51.36256.0.
Tests establish same-executable repeatability, block-size invariance, stable
event identity through voice stealing, loop/cache policy, unchanged trigger
counts, and exact coherent/strength-zero bypass. They do not establish
cross-platform bit identity, multithread execution invariance, real-time audio
safety of note-dispatch preprocessing, or subjective audio quality.

The measurements support keeping continuous analytic, finite analytic pools,
and independent-bin FFT as listening candidates. Smooth-field FFT is more
expensive here and less effective on the controlled repeated fixture. No mode
should become the default until paired listening decides whether the reduced
build-up is worth its attack and timbral changes.
