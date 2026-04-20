#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

enum class LoopMode : uint8_t { None, Forward, PingPong, OneShot };

struct SampleRegion
{
	// Key / velocity mapping
	uint8_t  lo_key = 0;
	uint8_t  hi_key = 127;
	uint8_t  lo_vel = 0;
	uint8_t  hi_vel = 127;
	uint8_t  root_key = 60;

	// Audio (16-bit PCM, interleaved if stereo)
	// Points into Soundfont::pcm_pool (SF2) or Soundfont::sfz_pcm[n] (SFZ).
	const int16_t* pcm = nullptr;
	uint32_t       pcm_len = 0;      // frames (samples per channel)
	uint32_t       sample_rate = 44100;
	uint8_t        channels = 1;      // 1 = mono, 2 = stereo

	// Loop (frame indices, relative to pcm[0])
	uint32_t loop_start = 0;
	uint32_t loop_end = 0;
	LoopMode loop_mode = LoopMode::None;

	// Tuning
	int16_t  coarse_tune = 0;    // semitones
	int16_t  fine_tune = 0;    // cents
	uint16_t scale_tuning = 100;  // cents per semitone (100 = normal pitch tracking)

	// Volume envelope (times in seconds; sustain is linear 0..1)
	float attack = 0.001f;
	float hold = 0.0f;
	float decay = 0.0f;
	float sustain = 1.0f;
	float release = 0.05f;

	// Mix
	float pan = 0.0f;  // -1 (left) .. 1 (right)
	float attenuation = 1.0f;  // linear gain

	// Exclusive class: voices with the same nonzero class mute each other (hi-hats, etc.)
	uint16_t exclusive_class = 0;
};

struct Soundfont
{
	std::vector<SampleRegion>         regions;
	std::vector<int16_t>              pcm_pool;  // SF2: all sample data lives here
	std::vector<std::vector<int16_t>> sfz_pcm;   // SFZ: one buffer per loaded sample file
};

// ------------------------------------------------------------
//  Voice - one active note being rendered
// ------------------------------------------------------------
// TODO: tune MAX_VOICES vs. polyphony needs (64 is a reasonable default)
constexpr int MAX_VOICES = 64;

struct Voice
{
	const SampleRegion* region = nullptr;
	uint8_t  note     = 0;
	uint8_t  velocity = 0;
	uint8_t  channel  = 0;

	// Sample playback
	double   pos = 0.0;   // fractional frame index into region->pcm
	double   inc = 0.0;   // frames-per-output-sample (pitch ratio * sample_rate_ratio)
	bool     loop_dir_fwd = true;  // current ping-pong direction

	// Volume envelope state machine
	enum class Stage : uint8_t { Off, Attack, Hold, Decay, Sustain, Release } stage = Stage::Off;
	float    env     = 0.0f;  // instantaneous envelope amplitude [0..1]
	float    env_inc = 0.0f;  // per-sample delta (sign depends on stage)
	uint32_t hold_samples_left = 0;

	// Derived per-voice stereo gain (pre-computed on note-on)
	float gain_l = 0.0f;
	float gain_r = 0.0f;

	// TODO: per-voice LFO state (vibrato, tremolo) if modulation is added

	bool     note_off_pending = false;  // note-off received but sustain pedal held
	bool     active() const { return stage != Stage::Off; }
};

// ------------------------------------------------------------
//  Synth global state (owned by core, read by render thread)
// ------------------------------------------------------------
// TODO: wrap in a struct if multiple synth instances are needed
// TODO: decide on thread-safety strategy (lock-free ring for MIDI, mutex for load)

struct SynthState
{
	// TODO: support multiple soundfonts / program changes
	Soundfont* soundfont = nullptr;

	Voice     voices[MAX_VOICES] = {};
	uint32_t  sample_rate = 48000;   // set by audio backend on init

	// Per-channel MIDI state (channels 0-15)
	// TODO: expand as more CCs are handled
	float     channel_volume[16]     = {};  // CC7,  linear [0..1]
	float     channel_expression[16] = {};  // CC11, linear [0..1]
	float     channel_pan[16]        = {};  // CC10, [-1..1]
	float     pitch_bend[16]         = {};  // [-2..+2] semitones (or per bend range)
	bool      sustain_pedal[16]      = {};  // CC64 >= 64

	// TODO: modulation wheel (CC1) → LFO depth routing
	// TODO: per-channel bend range (RPN 0)
};

extern SynthState g_synth;  // defined in core.cpp

// ------------------------------------------------------------
//  Public API
// ------------------------------------------------------------

bool load_sf2(const char* path, Soundfont& sf);
bool load_sfz(const char* path, Soundfont& sf);

// Called once by the audio backend before rendering starts.
// sample_rate: the output device sample rate (e.g. 48000).
void init(uint32_t sample_rate = 48000);

// Called from the MIDI input path (WinMM modMessage / VST processEvents).
// msg: packed Windows short-message format  [status | data1<<8 | data2<<16].
void consume_short_msg(uint32_t msg);

// Called from the audio render thread once per buffer.
// out: interleaved stereo float, 'frames' frames (2*frames floats total).
// Must be real-time safe: no allocation, no blocking.
void render_audio(float* out, uint32_t frames);
