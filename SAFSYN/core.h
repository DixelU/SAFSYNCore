#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace safsyn
{

enum class LoopMode : uint8_t { None, Forward, Sustain, PingPong, OneShot };

struct SampleRegion
{
	uint8_t lo_key = 0;
	uint8_t hi_key = 127;
	uint8_t lo_vel = 0;
	uint8_t hi_vel = 127;
	uint8_t root_key = 60;

	// Audio is signed 16-bit PCM, interleaved when channels == 2. The owning
	// storage lives in Soundfont for the complete lifetime of an engine render.
	const int16_t* pcm = nullptr;
	uint32_t pcm_len = 0;
	uint32_t sample_rate = 44100;
	uint8_t channels = 1;

	uint32_t loop_start = 0;
	uint32_t loop_end = 0;
	LoopMode loop_mode = LoopMode::None;

	int16_t coarse_tune = 0;
	int16_t fine_tune = 0;
	uint16_t scale_tuning = 100;

	float attack = 0.001f;
	float hold = 0.0f;
	float decay = 0.0f;
	float sustain = 1.0f;
	float release = 0.05f;

	float pan = 0.0f;
	float attenuation = 1.0f;
	uint16_t exclusive_class = 0;
};

struct Soundfont
{
	std::vector<SampleRegion> regions;
	std::vector<int16_t> pcm_pool;
	std::vector<std::vector<int16_t>> sfz_pcm;

	Soundfont() = default;
	Soundfont(const Soundfont&) = delete;
	Soundfont& operator=(const Soundfont&) = delete;
	Soundfont(Soundfont&&) noexcept = default;
	Soundfont& operator=(Soundfont&&) noexcept = default;
};

bool load_sf2(const char* path, Soundfont& sf);
bool load_sfz(const char* path, Soundfont& sf);

struct RenderStats
{
	uint64_t rendered_frames = 0;
	uint64_t started_voices = 0;
	uint64_t stolen_voices = 0;
	size_t peak_active_voices = 0;
};

class SynthEngine
{
public:
	explicit SynthEngine(uint32_t sample_rate = 48000, size_t voice_capacity = 256);

	SynthEngine(const SynthEngine&) = delete;
	SynthEngine& operator=(const SynthEngine&) = delete;
	SynthEngine(SynthEngine&&) noexcept = default;
	SynthEngine& operator=(SynthEngine&&) noexcept = default;

	uint32_t sample_rate() const noexcept { return sample_rate_; }
	size_t voice_capacity() const noexcept { return voices_.size(); }
	size_t active_voice_count() const noexcept;
	const RenderStats& stats() const noexcept { return stats_; }

	// The caller owns the soundfont and must keep it alive and unmodified while
	// it is attached. Replacing it immediately silences all current voices.
	void set_soundfont(const Soundfont* soundfont) noexcept;
	const Soundfont* soundfont() const noexcept { return soundfont_; }

	void reset() noexcept;
	void note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
	void note_off(uint8_t channel, uint8_t note) noexcept;
	void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
	void set_pitch_bend(uint8_t channel, uint16_t value14) noexcept;
	void consume_short_message(uint32_t packed_message) noexcept;
	void render_audio(float* interleaved_stereo, uint32_t frames) noexcept;

private:
	struct Voice
	{
		enum class Stage : uint8_t { Off, Attack, Hold, Decay, Sustain, Release };

		const SampleRegion* region = nullptr;
		uint8_t note = 0;
		uint8_t velocity = 0;
		uint8_t channel = 0;
		double pos = 0.0;
		double inc = 0.0;
		bool loop_dir_fwd = true;
		Stage stage = Stage::Off;
		float env = 0.0f;
		float env_inc = 0.0f;
		uint32_t hold_samples_left = 0;
		float gain_l = 0.0f;
		float gain_r = 0.0f;
		bool note_off_pending = false;
		uint64_t serial = 0;

		bool active() const noexcept { return stage != Stage::Off; }
	};

	struct ChannelState
	{
		float volume = 100.0f / 127.0f;
		float expression = 1.0f;
		float pan = 0.0f;
		float pitch_bend_semitones = 0.0f;
		bool sustain_pedal = false;
	};

	Voice* allocate_voice() noexcept;
	double compute_increment(const SampleRegion& region, uint8_t note,
		float bend_semitones) const noexcept;
	void compute_gains(const SampleRegion& region, uint8_t channel, uint8_t velocity,
		float& left, float& right) const noexcept;
	void begin_release(Voice& voice, float seconds_override = -1.0f) noexcept;
	void begin_envelope(Voice& voice) noexcept;
	float advance_envelope(Voice& voice) noexcept;
	void update_channel_voice_gains(uint8_t channel) noexcept;
	void update_channel_pitch(uint8_t channel) noexcept;
	void all_notes_off(uint8_t channel) noexcept;
	void silence_all() noexcept;

	const Soundfont* soundfont_ = nullptr;
	std::vector<Voice> voices_;
	ChannelState channels_[16] = {};
	uint32_t sample_rate_ = 48000;
	uint64_t next_serial_ = 1;
	RenderStats stats_;
};

} // namespace safsyn
