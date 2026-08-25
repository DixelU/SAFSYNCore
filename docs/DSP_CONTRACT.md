# Offline renderer DSP contract

## Baseline

Milestones 0–2 define a coherent renderer: every trigger begins from the same
stored sample phase, MIDI-style events take effect at an exact integer output
frame, and rendering the same inputs must produce bit-identical float samples.
Changing the render block size must not change the output. Mixing is additive
float32 stereo with no implicit normalization, limiter, or soft clipping.

This baseline measures the phenomenon phase processing is intended to alter.
For identical simultaneous voices with gains `g[k]` and source `x[n]`, coherent
addition is `x[n] * sum(g[k])`; peak amplitude can therefore grow linearly with
voice count. The opt-in decorrelated modes are intended to reduce that coherent
addition without moving scheduled note-on or note-off frames.

## Separate hypotheses

Two effects must not be conflated:

1. **Waveform coherence**: identical phase-aligned samples reinforce one
   another. Polarity or analytic phase rotation can reduce that reinforcement.
2. **Periodic envelope modulation**: fixed-rate retriggering or chopping creates
   a periodic amplitude envelope. Even perfectly decorrelated carriers can
   retain energy at the dispatch frequency and its harmonics.

The project must never claim that phase decorrelation removes every audible or
spectral trace of periodic chopping. It can only be judged against the coherent
baseline while timing remains unchanged.

Timing jitter is a separate comparison mode because it changes event timing.
It must not be described or measured as phase-only processing.

## Current deterministic rules

- Engine state is instance-local; there is no process-global synthesizer.
- Events are applied before rendering their target frame.
- Note-on region traversal follows sound-bank region order.
- A note-off releases all layers belonging to the newest matching note-on.
- Voice exhaustion prefers the quietest releasing voice, then the oldest voice.
- SF2/SFZ sustain loops stop looping on note-off and then play their sample tail.
- The audio loop allocates nothing and performs no I/O.
- Phase identity is derived from the configured seed, event serial, MIDI
  channel, note, and logical sample region. Voice slots are not identities.
- Analytic quadrature and FFT phase variants are constructed outside
  `render_audio` and cached by logical sample plus phase settings. FFT variants
  are generated lazily when an event first selects them.
- Finite phase pools accept 1–64 deterministic choices; the regression and
  measurement fixtures cover 1, 8, 32, and 64. Continuous analytic mode derives
  an angle directly from the stable event identity.
- Stereo partners use the same analytic angle or FFT phase sheet.
- Looping regions use separately transformed periodic loop bodies with a
  bounded entry crossfade rather than wrapping an arbitrary transformed tail.
- A protected attack is copied exactly, followed by a 10 ms smoothstep blend
  into the transformed representation.
- Coherent mode and every mode at strength zero bypass phase preprocessing and
  retain the exact coherent sample path.
- A sound bank is immutable while attached to an engine.
- Output is IEEE float32 WAV; values outside `[-1, 1]` are retained as evidence.
- MIDI bank select uses CC 0/32 and program changes use status `0xCn`.
- Normal SF2 note-on traversal is limited to the channel's selected bank and
  program. The legacy flattened view is reachable only through the explicit
  all-regions stress setting.

Milestone 2.5 resolves SF2 preset zones through instruments to sample zones,
combines the currently supported preset and instrument generator subset, and
intersects their key and velocity ranges. Linked left/right samples become one
logical stereo region backed by the original planar sample data. This does not
claim the full SF2 modulator system, filters, LFOs, or every generator.

Bit identity is currently promised for the same MSVC executable and settings.
Other supported platforms are expected to be numerically equivalent within a
future documented tolerance; cross-platform bit identity is not claimed.

## Piano integration baselines

The coherent seed-loader reference is intentionally preserved by
`--all-regions`. With `sDetrimental Concert Grand Piano.sf2`, the scripted
four-second render at 48 kHz and 512-voice capacity reports:

- presets: 1
- legacy stress regions: 1,740
- started voices: 180
- peak active voices: 180
- unclipped peak: `11.0501`
- SHA-256: `4F2AD2A76244D43AF5E813E5DA8A3444AE510BAEE6E12D37DD41757CFA3D11AD`

The selected bank 0/program 0 playback of the same script and soundfont reports:

- resolved regions belonging to the selected preset: 1,740
- started voices: 9
- peak active voices: 9
- unclipped peak: `0.390679`
- SHA-256: `1A79343D659CD744970FB57AA5C1E6CD7E859679A0E8C3CF0C43E89DB285983D`

Two selected-preset renders produced the same SHA-256 under MSVC 19.51.36256.0.
The stress SHA also matches the pre-milestone seed-loader WAV. These WAV files
remain unnormalized float output; neither peak is evidence of limiting or
production loudness policy.

## Phase experiment status

The experimental renderer now contains random-polarity, analytic-signal,
smooth FFT phase-field, and independent FFT-bin modes. The current automated
fixtures cover coherent pool-1 behavior, 8/32/64 pools, continuous analytic
angles, FFT magnitude/DC/Nyquist preservation, attack blending, linked stereo,
periodic loops, block-size determinism, stable voice stealing, unchanged event
counts, and absence of phase-cache construction during rendering.

The measurement harness reports:

- direct dispatch-frequency energy
- autocorrelation at the dispatch period
- peak and stereo RMS
- render time and memory
- paired coherent/decorrelated WAV outputs

`PHASE_EXPERIMENT.md` records the complete seven-mode piano and controlled
retrigger matrices, hashes, metric definitions, cache costs, and conclusions.
The same-executable block-size contract is tested; multithread execution and
cross-platform numerical equivalence have not yet been exercised. No phase mode
enters production until listening tests and those remaining evidence boundaries
are addressed.
