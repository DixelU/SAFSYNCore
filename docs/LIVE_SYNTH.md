# Standalone Windows synth

`safsyn-synth.exe` wraps the existing renderer in a playable native Windows
application. `safsyn-play.exe` exposes the same host from the command line.
Both are built by default on Windows; no driver installation, registry edits,
extra audio SDK, or downloaded runtime library is needed to build them.

## Use

1. Choose an SF2/SFZ bank, or leave it blank for the built-in sine instrument.
2. Choose an active Windows output. The default follows the multimedia endpoint
   selected when the session starts.
3. For a keyboard or another application, select a WinMM MIDI input and click
   **Start live synth**. An external MIDI player must send to a hardware input
   or an already-installed virtual MIDI cable. This app creates neither a virtual
   port nor a system MIDI-output driver. **Test chord** works without MIDI hardware.
4. For dense files, select a `.mid` and click **Play MIDI file**. The file is read
   once, but events are parsed and scheduled incrementally without an analysis
   pass or a second, expanded event array. It retains existing PPQN/SMPTE timing,
   tempo handling, and stable multi-track merge behavior.
   **Black MIDI preset** sets 512 cohorts, four render threads, and ten seconds
   of render-ahead buffering. This trades polyphony and startup latency for
   headroom; do not use a ten-second buffer for playing a live keyboard.
5. **Panic / stop** stops the device and the session, including buffered audio.
   Starting again reloads the selected inputs and restarts the file from zero.

The MIDI file, bank, output, and synthesis settings are immutable during a
session. Loading happens outside the UI thread. Closing while loading requests
cancellation and waits for the current bank/file load to finish safely; those
loaders are not interruptible. Output device loss is reported and stops playback;
choose another endpoint or refresh the device list and start again.
Phase mode is selectable in the synth and console; coherent (off) remains the
default. Stop before changing the mode, then start again to prepare its cache.

The console supports `--help`, `--list-devices`, `--bank`, `--midi`, `--midi-in`,
`--output`, `--threads`, `--cohorts`, `--buffer-frames`, `--block-frames`,
`--sample-rate`, `--gain-db`, `--no-limiter`, `--seconds`, `--test-note`, and
`--mute`. `--output` uses the displayed output index; `--midi-in` uses the MIDI
device ID. Ctrl+C/Break requests a joined shutdown. `--seconds` bounds time after
the device starts, excluding file loading, phase preparation, and initial buffering. `--mute`
still renders and delivers audio through WASAPI, but marks device packets silent.

## Phase selection and startup preparation

The **Phase mode** selector offers coherent, random polarity, analytic rotation,
smooth phase field, and independent FFT bins. Settings that do not apply to the
selected mode are disabled and ignored; their text is retained when switching
modes. Analytic continuous assignment disables the finite pool field. The GUI
starts with a pool of eight; the console retains the core default of 64, so use
`--phase-pool` explicitly for FFT modes with large banks. The Black MIDI preset
only changes threads, cohort ceiling, and buffering; it preserves phase settings.

Before dispatching any notes, the producer prepares every unique sample across
the bank's presets. Analytic mode prepares one quadrature buffer per sample,
shared by all its angles; smooth/independent modes prepare every variant in the
finite pool. Coherent and random polarity require no transformed PCM. Shared
sample layers reuse the same cache entry. No dummy notes are played, event serials
are unchanged, and MIDI still starts at sample zero. This avoids FFT work on the
first note of a new key, velocity layer, or program. Preparation is currently
serial; the persistent cohort workers handle playback mixing afterward.

The status panel shows completed/total transforms, current/planned cache MiB,
and preparation time. **Panic / stop**, closing the window, and console Ctrl+C
cancel preparation, including inside a long FFT. The test keyboard and WinMM
input start accepting notes only after preparation and initial buffering.

**Cache MiB** limits persistent transformed PCM (2048 MiB by default). An
oversized pool is rejected before generating any transformed samples, with the
required size and suggestions. Temporary FFT arrays, the original bank, cohort
state, and MIDI data are additional memory; this setting is not a process memory
limit. Large banks can still take substantial time to prepare. Reduce the pool
or use analytic, polarity, or coherent mode if the cost is too high. Caches are
session-local and rebuilt on a fresh start; they are not saved to disk.

The synth also reserves finite cohort slots, initial logical bookkeeping,
region-match scratch, and per-worker mix/retirement buffers before playback.
Actual cohort membership is constructed from arriving MIDI events: note timing,
sustain, releases, and controllers cannot be known ahead of live input. Further
logical-note and phase-aggregate bookkeeping can still allocate during playback.

```text
safsyn-play --bank piano.sf2 --phase analytic --phase-pool 8 --midi song.mid
safsyn-play --phase smooth --phase-pool 4 --phase-correlation-hz 250 --test-note --seconds 2
```

Additional console settings are `--phase-strength 0..1`, `--phase-seed N`,
`--phase-continuous`, `--phase-preserve-attack-ms N`, and `--phase-cache-mib N`.
Mode names for `--phase` are `coherent`, `polarity`, `analytic`, `smooth`, and
`independent`. Finite caches remove startup transforms, not the ongoing mixing
cost of phase decorrelation; the usual cohort ceiling and underrun metrics apply.

## Thread ownership and timing

```text
WinMM callback / GUI -> bounded MIDI queue ----+
                                              |
Scheduled SMF stream (file mode only) ----------+-> synth producer
                                                   | persistent cohort workers
                                                   | limiter / gain / safety clamp
                                                   v
                                              stereo audio ring
                                                   |
                                                   v
                                      WASAPI delivery thread -> Windows output
```

One producer owns every mutable synth operation. The existing pool renders
disjoint cohort ranges into private buffers and reduces them in fixed lane
order. It does not assign whole MIDI channels to separate workers, which would
perform poorly on channel-skewed black MIDI. `0` render threads chooses at most
16 lanes and leaves two logical CPUs free where possible; `1` uses the scalar
reference. The producer participates in rendering, so N means N total mixer
lanes, not N additional worker threads. Small jobs retain the existing scalar
fallback to avoid synchronization overhead. The UI reports the actual count,
including fallback to one lane if thread creation fails.

The WASAPI event thread is registered with MMCSS's Pro Audio task. All its COM
interfaces stay on that thread. Shared mode requests stereo float32 and Windows
format/rate conversion; the host requests a 20 ms device buffer and displays
the actual frame count. Audio delivery only reads a preallocated single-producer,
single-consumer ring and copies/silences samples; it never dispatches MIDI,
renders voices, builds phase caches, allocates scratch audio, or waits for a
synth mutex/worker. Normal WinMM operations, including long-buffer requeueing
and shutdown, run on a separate service thread. The MIDI callback never calls
WinMM functions or DSP. These boundaries follow Microsoft's
[WASAPI initialization contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)
and [MIDI callback restrictions](https://learn.microsoft.com/en-us/previous-versions/dd798460(v=vs.85)).

This is buffered real-time playback, not a hard real-time rewrite of the DSP.
Cohort bookkeeping and the existing limiter can still allocate on the producer.
Workers still synchronize with the producer.
Finite cohort count limits physical mixing work, but it is not a strict total
memory ceiling: bank PCM, logical-note bookkeeping, region indexes, MIDI input
bytes, and scheduler state also consume memory.

Attaching a bank prepares a preset/key index in source-region order. Note-on
dispatch checks only that key's candidate regions and reuses its matching
scratch buffer, instead of scanning the complete bank and allocating a fresh
temporary vector for every note. Velocity filtering, layering, preset selection,
exclusive classes, and all-regions stress mode preserve their existing semantics.
If index preparation runs out of memory, the reference scan remains available.

SMF events retain their exact integer sample positions. Same-sample bursts are
dispatched in fixed-size batches without dropping events or expanding the live
queue. Cancellation is checked within those bursts. Live short messages are
ordered at enqueue time and applied at render-block boundaries; incoming WinMM
millisecond timestamps are not used for sample-accurate live scheduling. The
live producer dispatches at most 65,536 queued events before producing another
block, so extreme external bursts can acquire additional latency. Use direct
file mode for reliable black MIDI scheduling.

## Overload and output protection

- Default physical cohort ceiling: 4096. The existing deterministic stealing
  policy applies and is reported. Zero/unbounded is intentionally rejected for
  this host. More threads cannot make arbitrary polyphony affordable.
- GUI render-ahead buffer: 100 ms; console default: 4096 frames (85.3 ms at
  48 kHz). Total live latency also includes up to one block, limiter lookahead
  (5 ms), the device buffer, and Windows processing. For live playing, try
  20-40 ms if the machine can sustain it. At least two render blocks must fit.
- A short ring read produces silence, increments underrun/frame counters, and
  resumes the remaining music later. File events are not skipped to catch up
  with wall time; repeated starvation therefore makes playback late. Raising
  buffering only absorbs temporary bursts. Sustained overload needs a lower
  cohort ceiling, different phase mode, or a lower synthesis workload.
- The live MIDI queue holds 262,144 messages by default and accepts multiple
  producers. Overflow is reported, increments a generation, and resets voices
  and controllers on the synth producer. Old-generation messages are discarded
  so a lost note-off does not leave a stuck voice. New input continues after
  recovery. Any already-buffered audio drains over the configured latency;
  the GUI's stop button stops the device immediately instead. Input errors
  trigger the same recovery. `MIM_MOREDATA` is valid input and is not discarded.
- Live channel messages and Universal Real-Time Master Volume SysEx are
  supported. Other SysEx is ignored. Four reusable 1024-byte buffers accept
  fragmented SysEx; the short master-volume message can span buffers.
- Playback begins at -12 dB gain and with the existing stereo sample-peak
  limiter at -1 dB. A final finite-value check and [-1,1] clamp protect the
  device even when the limiter is disabled. This does not guarantee safe
  listening volume; start with a low speaker/headphone level. The offline
  renderer's unclipped float output remains unchanged.
- Files release sustain and held notes at EOF and drain voices, up to ten
  seconds of tail. The limiter is flushed, then the device buffer drains
  before stopping. Tail completion has render-block granularity.

## Build and validation

Use the repository's CMake instructions. The standalone host does not depend
on `SAFSYN_BUILD_WINMM`; that switch still builds only the old driver sketch.
No VST, ASIO, transport seeking, hot bank replacement, or automatic endpoint
reconnection is implemented in this version. Existing SF2/SFZ generator and
phase-mode limitations still apply. The bank loader takes Windows code-page
paths; unsupported Unicode bank paths are rejected explicitly. MIDI file paths
use wide filesystem paths.

A Release build can be staged as a portable folder with:

```text
cmake --install build --config Release --prefix portable --component Synth
```

The folder contains the GUI, console host, documentation, project license, and
the MSVC redistributable DLLs selected by CMake. The DLLs retain Microsoft's
licensing terms. No sound bank or MIDI fixture is included. Copy/zip the whole
folder; `safsyn-synth.exe` is the entry point.

`safsyn-playback-tests` verifies concurrent multi-producer MIDI ordering and
audio-ring wraparound, 51,911-note same-sample file bursts with a two-entry live
queue, sample-accurate note-offs across different block partitions, actual
four-lane rendering and deterministic repeats, scalar equivalence, overflow
recovery, underrun silence/counts, panic, full-buffer shutdown, and limiting.
Existing engine/SMF/mastering/phase tests and the coherent WAV hash remain in
CTest. Device and full-song results are recorded separately below.

Phase preparation tests compare lazy and prepared output bit-for-bit across
all five modes and continuous analytic, including layered stereo, loop transforms,
four-lane mixing, note-off reconstruction, and reset/cache reuse. They verify
that note processing performs no additional preprocessing, oversized caches fail
before allocation/dispatch, and cancellation interrupts the first long FFT.
Playback tests also verify that preparation preserves the first MIDI onset and
note-off sample positions and that irrelevant phase fields do not block startup.

### Local validation, 2026-08-31

- The phase-selection/preparation update passed all eight Release CTest suites.
  A muted real-piano analytic test prepared all 87 unique sample transforms in
  85.03 seconds, using 1,143,619,584 bytes (1090.64 MiB) of phase PCM, before
  dispatching the first MIDI note. Subsequent two-second WASAPI playback had
  zero underruns, missing frames, empty device buffers, or phase failures. A
  one-MiB limit rejected the same bank before any transform or note dispatch.
- A hidden native GUI probe checked all five mode capability combinations,
  continuous analytic with invalid stale pool/correlation fields, 64-bit seed
  input, prepared test-chord playback/release, and stop/close during real-bank
  preparation. Stopping preparation took 109 ms in this run. This verifies
  control behavior and lifecycle, not visual appearance or listening quality.
- The full-song black MIDI results below were obtained with coherent phase in
  the preceding synth build; they do not establish full-song performance of
  the newly exposed phase modes.
- Windows x64 Release build and all eight CTest suites passed. The prepared
  region-index tests include bank/program switching, key and velocity layers,
  reversed stress-region order, and soundbank replacement.
- A one-second dense `Hypernova.ending.mid` excerpt through the supplied piano,
  coherent phase, 512 cohorts, and scalar mixing was rendered with the previous
  executable and the new executable. Both WAV SHA-256 values were
  `B1562562EB3BE04F9D854CFC7BB266B767FB7CE6E207056CFFA1C4CEAAF21AD2`.
- A muted two-second built-in test note traversed WASAPI with zero audio-ring
  underruns, zero missing frames, and zero empty device buffers.
- A hidden GUI probe verified the Black MIDI preset values and live
  start/chord/note-release/stop/close, including six MIDI events and zero active
  voices after release. Hidden-window capture returned a blank image, so this
  is control/lifecycle validation, not a visual layout or listening sign-off.
- No WinMM MIDI input devices were present. Real keyboard/virtual-cable
  reception, fragmented hardware SysEx delivery, and device-unplug recovery
  have not been exercised on hardware.
- With phase rotation off, 4096 cohorts, 16 render lanes, and a 100 ms buffer,
  Hypernova reached only 109.69 music seconds in 160 device seconds and recorded
  2,416,144 missing frames before the region-lookup optimization. Lowering the
  cohort count alone did not remove the event-dispatch bottleneck.
- After indexing, the full `Hypernova.mid` with 512 cohorts, four lanes, and a
  five-second buffer dispatched all 124,570,127 events but still had 283 underruns
  totaling 98,576 frames near the final dense burst. This is why the Black MIDI
  file preset uses ten seconds of buffering. It does not guarantee arbitrary
  files or external live input can run without overload.
- The final ten-second preset completed the full `Hypernova.mid` through
  `sDetrimental Concert Grand Piano.sf2`: 6,765,797 frames (including the bounded
  release tail), 124,570,127 scheduled events, four render lanes, 11,980 parallel
  render calls, zero underruns, zero missing frames, zero rejected MIDI events,
  and zero empty device buffers. The 512-cohort ceiling caused 34,206,232
  capacity steals. This is bounded-polyphony playback, not preservation of all
  simultaneous voices. The WASAPI output was muted; no listening-quality or
  arbitrary-machine performance claim follows from this test.
