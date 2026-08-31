# SAFSYNCore

SAFSYNCore includes a standalone Windows MIDI synthesizer and a deterministic
streaming Standard MIDI File renderer with a
bit-exact coherent baseline, opt-in experimental phase-decorrelation modes, and
an optional post-mix mastering stage. Raw unclipped float output remains the
offline default so DSP experiments retain a reproducible baseline.

## What is included

- `safsyn`: static library with a C++20 public API, SF2/SFZ loading, and an
  instance-based synthesizer
- `safsyn-render`: headless scripted-note renderer producing stereo float32 WAV
- `safsyn-render-gui`: native Windows SMF renderer with organized settings,
  live render metrics, remaining-time estimates, and cancellation
- `safsyn-synth`: native Windows synth with WASAPI audio, WinMM MIDI input,
  direct MIDI-file playback, persistent render workers, and live overload metrics
- `safsyn-play`: command-line Windows playback and device diagnostics
- `safsyn-tests`: engine, MIDI-message, SFZ-loader, determinism, and WAV tests
- `safsyn-phase-tests`: phase policy, determinism, pool, loop, and cache tests
- `safsyn-mastering-tests`: gain, lookahead, stereo-link, and invariance tests
- `safsyn-smf-tests`: SMF parsing, timing, merge, streaming, and RF64 tests
- `safsyn-playback-tests`: concurrent queues, live recovery, dense file bursts,
  sample timing, threaded playback, limiting, and lifecycle tests
- `SAFSYN/dllmain.cpp`: preserved Windows driver sketch, excluded by default

The coherent engine supports the preserved fixed individual-voice reference
path plus dynamically growing exact onset cohorts for normal offline SMF
rendering, bounded channel-scoped safety-limit stealing, O(1) pitch automation,
an opt-in persistent threaded cohort mixer, linear interpolation, AHDSR envelopes,
note-off and sustain behavior, forward and ping-pong loops, stereo samples,
constant-power panning, 14-bit volume/pan/expression pairs, universal master
volume, per-channel retention of all 128 CC values, RPN/NRPN selection,
RPN 0 pitch-bend sensitivity, channel-mode messages, and unclipped float mixing.
SF2 playback resolves preset zones through instruments to sample zones, combines
the supported generator subset, intersects key/velocity ranges, and reconstructs
linked left/right samples as one logical stereo region. MIDI bank select and
program change choose the active SF2 preset per channel.

## Build and test

```text
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows, open `build/Release/safsyn-render-gui.exe` for the graphical
renderer. Choose a MIDI file, an SF2/SFZ sound bank, and an output WAV; the
Essentials, Phase, and Mastering tabs expose the same defaults as the CLI.
Rendering stays off the UI thread and reports exact frame progress, scheduled
events, active voices/cohorts, peak level, elapsed time, and estimated time
remaining. Cancelling finalizes the frames already written as a valid partial
WAV.

The public library headers remain C++20. The library implementation uses the
pinned `DixelU/utility` `long_uint` header for exact scheduling arithmetic and
therefore requires a compiler with C++23 implementation support.

Generate a self-contained coherent baseline without a sound bank:

```text
build/Release/safsyn-render --demo coherent-demo.wav
```

## Play the synth

On Windows, launch `build/Release/safsyn-synth.exe` (or
`build/safsyn-synth.exe` with a single-configuration Ninja build). Select an
SF2/SFZ bank and click **Start live synth**, or leave the bank blank to try the
built-in sine instrument with **Test chord**. Choose a MIDI input for a keyboard
or an existing virtual MIDI cable. For black MIDI files, choose the `.mid` and
click **Play MIDI file**; this uses the exact scheduler directly, without
routing millions of events through WinMM. Drag-and-drop accepts banks and MIDI.
The **Black MIDI preset** selects 512 cohorts, four render threads, and ten
seconds of buffering for dense files. The **Phase mode** selector defaults to
coherent (off), with polarity, analytic, and FFT modes available. Stop to change
mode. Phase samples and finite variant pools are precomputed before playback,
with progress, cancellation, and an adjustable cache limit; large banks can take
time and memory to prepare. Cohort storage and worker buffers are reserved at
startup, while cohort membership follows MIDI events.

Live playback uses a finite cohort ceiling (default 4096), an automatic persistent
render pool (up to 16 threads, reserving two logical CPUs), and a separate WASAPI
delivery thread. Buffering is adjustable; the GUI starts at 100 ms. Live output
starts at -12 dB with a -1 dB sample-peak limiter and a final safety clamp.
Offline renderer settings and reference hashes are unchanged.

```text
build/Release/safsyn-play --list-devices
build/Release/safsyn-play --bank piano.sf2 --midi song.mid --threads 8
build/Release/safsyn-play --bank piano.sf2 --midi black.mid --threads 4 --cohorts 512 --buffer-frames 480000
build/Release/safsyn-play --bank piano.sf2 --midi-in 0 --buffer-frames 2048
build/Release/safsyn-play --test-note --mute --seconds 2
```

See `docs/LIVE_SYNTH.md` for buffering, overflow recovery, setup, and validation
limits. This is a standalone host, not an installed MIDI-output driver or a
VST plugin; it does not change the registry or create a virtual MIDI port.

## Offline rendering

Render a sample-accurate scripted chord sequence through a bank:

```text
build/Release/safsyn-render piano.sf2 piano-baseline.wav --bank 0 --program 0
build/Release/safsyn-render instrument.sfz sfz-baseline.wav
```

Options are `--bank N`, `--program N`, `--sample-rate N`, and `--voices N`.
`--all-regions` explicitly selects the legacy flattened SF2 stress view; it is
not normal instrument playback.

Analyze a Standard MIDI File without loading a sound bank:

```text
build/Release/safsyn-render input.mid --analyze --sample-rate 48000 --tail-seconds 2
```

Or stream a bounded or complete render directly to RIFF/RF64 WAV:

```text
build/Release/safsyn-render input.mid piano.sf2 output.wav --bank 0 --program 0 --tail-seconds 2
build/Release/safsyn-render input.mid piano.sf2 excerpt.wav --max-render-seconds 30 --phase-mode analytic --phase-continuous --phase-seed 7
build/Release/safsyn-render input.mid piano.sf2 drained.wav --drain-tail --max-tail-seconds 30
build/Release/safsyn-render input.mid piano.sf2 listening.wav --limiter --limiter-ceiling-db -1 --limiter-lookahead-ms 5 --limiter-release-ms 100
```

SMF types 0 and 1, PPQN and SMPTE timing, running status, tempo changes,
channel events, retained bounded SysEx/meta payloads, Universal Master Volume,
and deterministic multi-track merge are supported. Type 2 is rejected. Input bytes are buffered, but parser
and scheduler state is O(track count); audio output is streamed in fixed blocks.
`--max-render-seconds` bounds both event dispatch and output, while
`--tail-seconds` controls the fixed natural-end release tail. SMF rendering uses
exact cohorts by default. `--individual-voices --voices N` selects the fixed
reference path, while `--max-cohorts N` gives cohorts an explicit deterministic
safety ceiling; zero means dynamic offline growth. `--drain-tail` renders until
all represented logical voices finish or `--max-tail-seconds` is reached.
`--render-threads N` enables the fast cohort mixer; the default of one retains
the scalar accumulation order used by the reference hashes.

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

SMF analysis also reports identical-note group histograms, group locations,
logical-note/cohort peak estimates, and onset compression. These are MIDI-only
estimates; exact post-preset region/cohort statistics are reported by renders.
It also reports a controller histogram and supports bounded traces for any CC.

Optional mastering controls are `--output-gain-db`, `--limiter`,
`--limiter-ceiling-db`, `--limiter-lookahead-ms`, and
`--limiter-release-ms`. The stereo-linked limiter is a sample-peak listening
stage after the raw float mix; it is not an oversampled true-peak meter.

## Project boundary

`SAFSYN_BUILD_WINMM` defaults to `OFF`. Enabling it only compiles the preserved
DLL shell; it is separate from the functioning standalone WASAPI/MIDI-input
host and does not expose a system MIDI-output device. See
`docs/DSP_CONTRACT.md` for the contract and `docs/PHASE_EXPERIMENT.md` for the
phase architecture, measurements, and evidence boundaries. See
`docs/SMF_RENDERER.md` for the file/scheduling contract and the `Hypernova.mid`
integration evidence.
See `docs/BLACK_MIDI_COHORTS.md` for logical-note/cohort architecture, tail
drain, validation, and the measured Hypernova compression limit.
See `docs/CONTROLLERS_AND_MASTERING.md` for controller semantics, the Hypernova
CC120 diagnosis, optional mastering, and current full-render evidence.
