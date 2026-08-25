#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace safsyn
{

class SynthEngine;
struct CohortEngineState;

class CohortEngine
{
public:
	CohortEngine();
	~CohortEngine();
	CohortEngine(const CohortEngine&) = delete;
	CohortEngine& operator=(const CohortEngine&) = delete;
	CohortEngine(CohortEngine&&) noexcept;
	CohortEngine& operator=(CohortEngine&&) noexcept;

	void configure(SynthEngine& owner, size_t maximum_cohorts) noexcept;
	void set_render_threads(size_t threads) noexcept;
	size_t render_threads() const noexcept;
	void clear(SynthEngine& owner) noexcept;
	void note_on_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
		uint8_t velocity, uint64_t count) noexcept;
	void note_off_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
		uint64_t count) noexcept;
	void release_sustained(SynthEngine& owner, uint8_t channel) noexcept;
	void all_notes_off(SynthEngine& owner, uint8_t channel) noexcept;
	void all_sound_off(SynthEngine& owner, uint8_t channel) noexcept;
	void update_channel_gains(SynthEngine& owner, uint8_t channel) noexcept;
	void invalidate_onset_merges(uint8_t channel) noexcept;
	void invalidate_all_onset_merges() noexcept;
	void render_audio(SynthEngine& owner, float* interleaved_stereo,
		uint32_t frames) noexcept;

	size_t active_logical_voices() const noexcept;
	size_t active_cohorts() const noexcept;

private:
	std::unique_ptr<CohortEngineState> state_;
};

} // namespace safsyn
