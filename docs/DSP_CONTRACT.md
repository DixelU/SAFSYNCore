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
voice count. A future decorrelated mode should reduce that coherent addition
without moving the scheduled note-on or note-off frames.

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
- A sound bank is immutable while attached to an engine.
- Output is IEEE float32 WAV; values outside `[-1, 1]` are retained as evidence.

The current SF2 loader preserves the seed implementation. Full preset/global
generator composition, bank/program selection, and stereo-linked SF2 samples
remain milestone 5 work and are not claimed by this baseline.

## Required fixtures and measurements for the later phase milestone

Keep reproducible fixtures for simultaneous identical notes, fixed-rate
retriggers, fixed-rate chopped notes, dense mixed-pitch chords, real piano
samples, and stereo samples. Compare 32-phase, 64-phase, and continuous phase
distributions using:

- dispatch-frequency and harmonic energy
- autocorrelation at the dispatch period
- mean inter-voice coherence
- peak, RMS, and crest factor
- render time and memory
- paired coherent/decorrelated WAV outputs

The piano attack experiment must compare fully rotated playback with an
unmodified attack crossfaded into rotation. No phase mode enters production
until its output is deterministic across block size and thread count.
