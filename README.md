# SAFSYNCore

SAFSYNCore is currently a deterministic, coherent, offline sample renderer. This
foundation deliberately stops before Standard MIDI File parsing, phase
decorrelation, normalization, limiting, and platform audio drivers so later DSP
experiments have a reproducible baseline.

## What is included

- `safsyn`: portable C++20 static library with SF2/SFZ loading and an
  instance-based synthesizer
- `safsyn-render`: headless scripted-note renderer producing stereo float32 WAV
- `safsyn-tests`: engine, MIDI-message, SFZ-loader, determinism, and WAV tests
- `SAFSYN/dllmain.cpp`: preserved Windows driver sketch, excluded by default

The coherent engine supports configurable preallocated polyphony, deterministic
voice stealing, pitch and pitch bend, linear interpolation, AHDSR envelopes,
note-off and sustain behavior, forward and ping-pong loops, stereo samples,
constant-power panning, channel volume/expression, and unclipped float mixing.

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
build/Release/safsyn-render piano.sf2 piano-baseline.wav
build/Release/safsyn-render instrument.sfz sfz-baseline.wav
```

Options currently exposed are `--sample-rate N` and `--voices N`. The command
does not accept MIDI files yet; SMF scheduling is milestone 3.

## Project boundary

`SAFSYN_BUILD_WINMM` defaults to `OFF`. Enabling it only compiles the preserved
DLL shell; it does not claim a functioning WinMM/WASAPI backend. See
`docs/DSP_CONTRACT.md` for the experimental contract and evidence requirements.
