# SAFSYNCore

SAFSYNCore is a deterministic offline sample renderer with a bit-exact coherent
baseline and opt-in experimental phase-decorrelation modes. The project still
deliberately stops before Standard MIDI File parsing, normalization, limiting,
and platform audio drivers so DSP experiments retain a reproducible baseline.

## What is included

- `safsyn`: portable C++20 static library with SF2/SFZ loading and an
  instance-based synthesizer
- `safsyn-render`: headless scripted-note renderer producing stereo float32 WAV
- `safsyn-tests`: engine, MIDI-message, SFZ-loader, determinism, and WAV tests
- `safsyn-phase-tests`: phase policy, determinism, pool, loop, and cache tests
- `SAFSYN/dllmain.cpp`: preserved Windows driver sketch, excluded by default

The coherent engine supports configurable preallocated polyphony, deterministic
voice stealing, pitch and pitch bend, linear interpolation, AHDSR envelopes,
note-off and sustain behavior, forward and ping-pong loops, stereo samples,
constant-power panning, channel volume/expression, and unclipped float mixing.
SF2 playback resolves preset zones through instruments to sample zones, combines
the supported generator subset, intersects key/velocity ranges, and reconstructs
linked left/right samples as one logical stereo region. MIDI bank select and
program change choose the active SF2 preset per channel.

## Build and test

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Generate a self-contained coherent baseline without a sound bank:

```text
build/Release/safsyn-render --demo coherent-demo.wav
```

Or render the same sample-accurate scripted chord sequence through a bank:

```text
build/Release/safsyn-render piano.sf2 piano-baseline.wav --bank 0 --program 0
build/Release/safsyn-render instrument.sfz sfz-baseline.wav
```

Options are `--bank N`, `--program N`, `--sample-rate N`, and `--voices N`.
`--all-regions` explicitly selects the legacy flattened SF2 stress view; it is
not normal instrument playback. The command does not accept MIDI files yet; SMF
scheduling is milestone 3.

Run an opt-in phase experiment with the same scripted events:

```text
build/Release/safsyn-render piano.sf2 analytic.wav --bank 0 --program 0 --phase-mode analytic --phase-pool 64 --phase-seed 7
build/Release/safsyn-render --demo repeated.wav --script repeated --repeat-hz 40 --repeat-count 128 --phase-mode analytic --phase-continuous
```

Available modes are `coherent`, `polarity`, `analytic`, `smooth-field`, and
`independent-bins`. Phase controls are `--phase-strength 0..1`,
`--phase-pool 1..64`, `--phase-continuous`, `--phase-seed N`,
`--phase-correlation-hz N`, and `--phase-preserve-attack-ms N`. Coherent is the
default, strength zero takes the exact coherent path, and no experimental mode
has been selected as a production default.

## Project boundary

`SAFSYN_BUILD_WINMM` defaults to `OFF`. Enabling it only compiles the preserved
DLL shell; it does not claim a functioning WinMM/WASAPI backend. See
`docs/DSP_CONTRACT.md` for the contract and `docs/PHASE_EXPERIMENT.md` for the
phase architecture, measurements, and evidence boundaries.
