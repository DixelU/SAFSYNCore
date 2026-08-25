#pragma once

#include "phase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace safsyn
{

enum class LoopMode : uint8_t { None, Forward, Sustain, PingPong, OneShot };

struct SampleRegion
{
	// Stable within a loaded bank and used for deterministic phase-cache keys.
	uint64_t logical_sample_id = 0;
	// SFZ regions use the General MIDI default preset (bank 0/program 0).
	// SF2 regions retain the preset identity resolved from the phdr table.
	uint16_t preset_bank = 0;
	uint16_t preset_program = 0;

	uint8_t lo_key = 0;
	uint8_t hi_key = 127;
	uint8_t lo_vel = 0;
	uint8_t hi_vel = 127;
	uint8_t root_key = 60;

	// Audio is signed 16-bit PCM. Stereo is interleaved unless pcm_right points
	// at a separate right plane. Soundfont owns the storage for an engine render.
	const int16_t* pcm = nullptr;
	// Linked SF2 stereo is planar in the smpl pool; pcm_right identifies that
	// representation. Stereo SFZ/WAV data remains interleaved with this null.
	const int16_t* pcm_right = nullptr;
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

struct PresetInfo
{
	uint16_t bank = 0;
	uint16_t program = 0;
	std::string name;
	size_t first_region = 0;
	size_t region_count = 0;
};

struct Soundfont
{
	std::vector<SampleRegion> regions;
	// Seed-loader view retained solely for the explicit all-regions stress mode.
	std::vector<SampleRegion> stress_regions;
	std::vector<PresetInfo> presets;
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
	uint64_t logical_voices_started = 0;
	size_t peak_active_logical_voices = 0;
	uint64_t cohorts_created = 0;
	size_t peak_active_cohorts = 0;
	uint64_t logical_voices_merged = 0;
	uint64_t cohort_splits = 0;
	uint64_t cohort_merges = 0;
	uint64_t cohort_capacity_steals = 0;
	uint64_t channel_scoped_steals = 0;
	uint64_t channel_reserve_steals = 0;
	uint64_t global_fallback_steals = 0;
	uint64_t steal_searches = 0;
	uint64_t steal_candidate_visits = 0;
	uint32_t maximum_steal_probe = 0;
	uint64_t parallel_render_calls = 0;
	uint64_t parallel_rendered_frames = 0;
	double average_cohort_multiplicity = 0.0;
	uint64_t maximum_cohort_multiplicity = 0;
};

enum class VoiceModel : uint8_t
{
	Individual,
	Cohorts,
};

class CohortEngine;
struct CohortEngineState;

class SynthEngine
{
public:
	explicit SynthEngine(uint32_t sample_rate = 48000, size_t voice_capacity = 256);
	~SynthEngine();

	SynthEngine(const SynthEngine&) = delete;
	SynthEngine& operator=(const SynthEngine&) = delete;
	SynthEngine(SynthEngine&&) noexcept;
	SynthEngine& operator=(SynthEngine&&) noexcept;

	uint32_t sample_rate() const noexcept { return sample_rate_; }
	// The constructor reserve/capacity belongs to the individual reference path.
	// Cohorts grow dynamically unless maximum_cohorts() is nonzero.
	size_t voice_capacity() const noexcept { return voices_.size(); }
	// In cohort mode this is represented logical region voices, not slot count.
	size_t active_voice_count() const noexcept;
	size_t active_cohort_count() const noexcept;
	const RenderStats& stats() const noexcept { return stats_; }
	const PhaseSettings& phase_settings() const noexcept { return phase_processor_.settings(); }
	PhaseCacheStats phase_cache_stats() const noexcept { return phase_processor_.stats(); }

	// The caller owns the soundfont and must keep it alive and unmodified while
	// it is attached. Replacing it immediately silences all current voices.
	void set_soundfont(const Soundfont* soundfont) noexcept;
	const Soundfont* soundfont() const noexcept { return soundfont_; }

	void reset() noexcept;
	void note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
	void note_on_batch(uint8_t channel, uint8_t note, uint8_t velocity,
		uint64_t count) noexcept;
	void note_off(uint8_t channel, uint8_t note) noexcept;
	void note_off_batch(uint8_t channel, uint8_t note, uint64_t count) noexcept;
	void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
	// Every MIDI CC has independent retained state on every channel, including
	// controls whose DSP behavior is not implemented yet (for example effects sends).
	uint8_t controller_value(uint8_t channel, uint8_t controller) const noexcept;
	void program_change(uint8_t channel, uint8_t program) noexcept;
	void set_pitch_bend(uint8_t channel, uint16_t value14) noexcept;
	uint16_t pitch_bend_value(uint8_t channel) const noexcept;
	float pitch_bend_range_semitones(uint8_t channel) const noexcept;
	// Universal real-time Master Volume, normalized from MIDI's 14-bit value.
	void set_master_volume(uint16_t value14) noexcept;
	void consume_short_message(uint32_t packed_message) noexcept;
	// Stable-order dispatch that combines only consecutive compatible note runs.
	void consume_short_messages(const uint32_t* packed_messages, size_t count) noexcept;
	void render_audio(float* interleaved_stereo, uint32_t frames) noexcept;
	void set_phase_settings(const PhaseSettings& settings) noexcept;
	// A zero cohort ceiling means dynamic offline growth without stealing.
	void set_voice_model(VoiceModel model, size_t maximum_cohorts = 0) noexcept;
	VoiceModel voice_model() const noexcept { return voice_model_; }
	size_t maximum_cohorts() const noexcept { return maximum_cohorts_; }
	// Threaded cohort mixing is an opt-in fast path. One preserves the scalar
	// accumulation order used by the reference hashes.
	void set_render_threads(size_t threads) noexcept;
	size_t render_threads() const noexcept;

	// Explicit compatibility/stress path for reproducing the old flattened SF2
	// behavior. Normal playback always selects the channel bank and program.
	void set_all_regions_mode(bool enabled) noexcept { all_regions_mode_ = enabled; }
	bool all_regions_mode() const noexcept { return all_regions_mode_; }

private:
	struct Voice
	{
		enum class Stage : uint8_t { Off, Attack, Hold, Decay, Sustain, Release };

		const SampleRegion* region = nullptr;
		uint8_t note = 0;
		uint8_t velocity = 0;
		uint8_t channel = 0;
		double pos = 0.0;
		double base_inc = 0.0;
		bool loop_dir_fwd = true;
		Stage stage = Stage::Off;
		float env = 0.0f;
		float env_inc = 0.0f;
		uint32_t hold_samples_left = 0;
		float gain_l = 0.0f;
		float gain_r = 0.0f;
		bool note_off_pending = false;
		uint64_t serial = 0;
		PhaseVoiceState phase;

		bool active() const noexcept { return stage != Stage::Off; }
	};

	struct ChannelState
	{
		enum class ParameterSelection : uint8_t { None, Rpn, Nrpn };

		ChannelState() noexcept
		{
			controllers[7] = 100;
			controllers[10] = 64;
			controllers[11] = 127;
			controllers[98] = 127;
			controllers[99] = 127;
			controllers[100] = 127;
			controllers[101] = 127;
		}

		std::array<uint8_t, 128> controllers{};
		float volume = 100.0f / 127.0f;
		float expression = 1.0f;
		float pan = 0.0f;
		float pitch_bend_semitones = 0.0f;
		float pitch_bend_range_semitones = 2.0f;
		double pitch_bend_ratio = 1.0;
		uint16_t pitch_bend_value = 8192;
		ParameterSelection parameter_selection = ParameterSelection::None;
		bool sustain_pedal = false;
		uint8_t program = 0;
	};

	Voice* allocate_voice(uint8_t request_channel, uint8_t request_note) noexcept;
	double compute_base_increment(const SampleRegion& region, uint8_t note) const noexcept;
	void compute_gains(const SampleRegion& region, uint8_t channel, uint8_t velocity,
		float& left, float& right) const noexcept;
	void begin_release(Voice& voice, float seconds_override = -1.0f) noexcept;
	void begin_envelope(Voice& voice) noexcept;
	float advance_envelope(Voice& voice) noexcept;
	void update_channel_voice_gains(uint8_t channel) noexcept;
	void update_channel_pitch_bend(uint8_t channel) noexcept;
	void all_notes_off(uint8_t channel) noexcept;
	void all_sound_off(uint8_t channel) noexcept;
	void silence_all() noexcept;
	friend class CohortEngine;
	friend struct CohortEngineState;

	const Soundfont* soundfont_ = nullptr;
	std::vector<Voice> voices_;
	std::unique_ptr<CohortEngine> cohort_engine_;
	ChannelState channels_[16] = {};
	uint32_t sample_rate_ = 48000;
	uint64_t next_serial_ = 1;
	bool all_regions_mode_ = false;
	float master_volume_ = 1.0f;
	VoiceModel voice_model_ = VoiceModel::Individual;
	size_t maximum_cohorts_ = 0;
	PhaseProcessor phase_processor_;
	RenderStats stats_;
};

} // namespace safsyn
