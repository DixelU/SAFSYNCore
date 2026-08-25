# Streaming Standard MIDI File renderer

This milestone turns the deterministic sample engine into a streaming SMF
converter while retaining coherent playback as the default. A later milestone
adds optional post-mix gain and sample-peak limiting; the raw float path remains
the default. Filtering, multithreaded rendering, realtime audio, and a
production phase policy remain out of scope.

## Parsing and scheduling architecture

`SmfFile` buffers the input bytes and validates the chunk envelope. Buffering is
explicitly excluded from the parser-state bound; it permits inexpensive second
passes without retaining decoded events. Each track cursor stores only its
chunk bounds, byte position, absolute tick, event ordinal, and running status.

`MergedSmfStream` primes one event per track and uses a min-heap ordered by:

1. absolute tick;
2. track index;
3. track-local ordinal.

When one event is consumed, only that track is advanced. Decoder and heap state
are therefore O(track count), and no complete event list is constructed.

Supported input behavior:

- format 0 and format 1; format 2 is rejected;
- bounded one-to-four-byte VLQs;
- independent running status per track;
- all MIDI channel message sizes;
- tempo and end-of-track meta events;
- retained bounded SysEx and unknown meta payloads;
- PPQN and SMPTE `-24/-25/-29/-30` divisions;
- explicit diagnostics for invalid status, overflow, malformed VLQ, and
  truncated chunks or payloads.

The sample clock uses pinned `dixelu::long_uint<0>` multiplication for
`delta_ticks * rate_factor + carried_remainder`. Division is an exact optimized
128-by-64 operation on MSVC/GCC-compatible targets, with a portable bitwise
fallback. PPQN tempo changes reuse one denominator and preserve the numerator
remainder across segments. SMPTE `-29` is represented as `30000/1001`; no
floating-point value participates in event timestamps.

## Rendering and file output

The renderer advances in fixed blocks but stops exactly at every scheduled
sample. All channel events assigned to that sample are routed in merge order
through the stable batched dispatch API, then audio rendering continues. Only
compatible note runs are combined. Bank
select, program change, sustain, note-on/off, pitch bend, 14-bit controller
pairs, channel-mode events, and Universal Master Volume therefore use the
engine contract and retain phase event identity.

Audio is accumulated only for the current block and written immediately.
`FloatWavWriter` predicts RIFF versus RF64 from the expected frame count and
patches actual sizes at close. RF64 writes a `ds64` chunk and 32-bit sentinels.
The legacy in-memory `write_float_wav` helper now delegates to the same writer
and retains its canonical RIFF bytes.

At natural end, sustain is lifted on all channels, all non-one-shot notes are
released, and the configured tail is streamed. `--max-render-seconds` is an
exclusive hard boundary and can truncate both the event sequence and tail.
An opt-in drain mode renders until all represented logical voices end, subject
to an explicit maximum tail.

The optional mastering stage follows the raw float mix and precedes the WAV
writer. Output gain is applied first. The limiter then uses a stereo-linked
future sample-peak window, immediate attack, and exponential release. Its
lookahead delays writing only; flushing preserves the exact source frame count.
Raw and output peak/RMS are reported separately.

## Analysis mode

Use:

```text
safsyn-render input.mid --analyze --sample-rate 48000 --tail-seconds 2
```

The report includes format, tracks, division, duration, total/channel/note/meta
counts, tempo changes, bank/program use at note-on, maximum events sharing a
tick and output sample, parser-state bytes, estimated output/container, scan
time, throughput, and malformed/truncated diagnostics. Analysis performs the
same incremental scheduling pass used for rendering.

## `Hypernova.mid` integration (initial milestone)

Local fixture only: `C:\Users\User\Downloads\Hypernova.mid`. Neither it nor the
generated WAV files are committed.

These measurements preserve the initial pre-controller milestone. The current
CC120-aware analysis and full mastered renders are documented in
`CONTROLLERS_AND_MASTERING.md` and supersede the listening artifacts below.

Full-file analysis at 48 kHz:

| Measurement | Result |
|---|---:|
| Input bytes | 498,345,921 |
| Format / tracks / division | 1 / 26 / PPQN 960 |
| Duration | 6,285,797 frames / 130.954104 s |
| Total events | 124,570,127 |
| Channel events | 124,550,232 |
| Note-ons / note-offs | 44,750,713 / 75,810,553 |
| Tempo changes | 19,844 |
| Maximum events at one tick/sample | 51,911 / 51,911 |
| Parser and heap state | 3,016 bytes |
| Analysis time | approximately 4.45 s |
| Throughput | approximately 28.0 million events/s |

All sixteen channels use bank 0/program 0 at their note-ons. The analysis
requires the 498 MB input buffer, but the measured decoder/heap state remains
3,016 bytes.

The bounded A/B uses the first 48,000 frames (one second), 512 voices, no tail,
the selected bank-0/program-0 piano, and seed 7 for continuous analytic mode:

| Measurement | Coherent | Continuous analytic |
|---|---:|---:|
| Frames | 48,000 | 48,000 |
| Scheduled events | 114,755 | 114,755 |
| Dispatched channel events | 114,726 | 114,726 |
| Triggered / peak voices | 210 / 210 | 210 / 210 |
| Stolen voices | 0 | 0 |
| Peak | 0.711105943 | 0.759180307 |
| RMS | 0.113753696 | 0.122729830 |
| Phase preprocessing | 0 ms | 65,846.8 ms |
| Phase cache | 0 bytes | 825,901,056 bytes |
| Cached logical samples | 0 | 63 |
| `render_audio` time | 33.44 ms | 85.60 ms |
| Total render pass | 43.33 ms | 65,996.0 ms |
| WAV SHA-256 | `66633701755B83A64BB5F615FDFA00B2ABAA8260C985B0FEE27C8785EEE0831E` | `EF83224556EA7607C3AE9DC1A089F694121E009FF26976A8B113FB607B709CD4` |

Both WAVs are 384,044 bytes and contain exactly the same number and order of
scheduled/triggered events. This multi-key excerpt is not a coherent-copy
suppression fixture: analytic peak and RMS are slightly higher, so these values
do not contradict the repeated-sine experiment. The important result here is
the real workload cost. Sixty-three analytic sample preparations consume about
826 MB and 65.8 seconds, making coherent playback the only justified default at
this milestone. Listening and a later cache/preparation design pass are needed
before continuous analytic can be considered production-ready for arbitrary
multi-key MIDI files.

## Verification boundary

Automated tests cover type 0/1, stable cross-track ordering, running status,
tempo changes, bank/program routing, controller pairs and channel-mode events,
sustain and unfinished-note release, Universal Master Volume, PPQN remainder
preservation, SMPTE drop-frame timing, malformed VLQs, truncated chunks/payloads,
RIFF/RF64 finalization, mastering block invariance, and identical
coherent/analytic SMF output. A separate CTest keeps
the pre-SMF scripted coherent WAV at SHA-256
`319EBFEB7C562DD50BD557DDA0DF18E6C462E9395CBFAE8BBB06896AE2595731`.

The current evidence is same-executable MSVC Release output. Cross-platform bit
identity, multithreaded scheduling, realtime dispatch, and subjective listening
remain outside the proof boundary.

The follow-up logical-voice/cohort implementation, expanded analysis, and
Hypernova compression result are documented in `BLACK_MIDI_COHORTS.md`.
